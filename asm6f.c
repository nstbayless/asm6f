/* asm6f - asm6 with modifications for NES/Famicom development */

/*  asm6f History:
1.7
	* [NaOH] read iNES/NES2 header from existing .nes file
	* [NaOH] seek/skip to specific locations without padding,
	  allowing overwrites of existing data.
	* [NaOH] fixed issue when generating .nl file when ORG is unset.
	
1.6 + f002
	* [nicklausw] Added new directives for INES header generation.
	* [nicklausw] Put unstable/highly unstable opcode use behind directives,
	  instead of requiring separate executables.
	* [nicklausw] Add support for Lua symbol file generation.
	* [Sour] Add support for .cdl file generation, for use with FCEUX and Mesen.
	* [Sour] Add support for Mesen-compatible (.mlb) label export.
	* [freem] Fixed issue where the last symbol would not appear in an .nl file.

1.6 + freem modifications
	Added preliminary support for undocumented/illegal opcodes.
	Added sonder's code to export the .nl files for FCEUX symbolic debugging.
*/

/*  asm6 History:
1.6
	Prevent error overload by emitting 2 bytes when branch instructions fail to parse
	Bugfix for negative numbers being parsed incorrectly after too many passes are made
	Compatible with big-endian and 64-bit machines
	-q (quiet mode) command line option added
	Lots of miscellaneous code cleanup
1.51
	Added extra INCBIN args
1.5
	Added local labels
	Added DL, DH directives
	Added ERROR directive
	Bugfix for ACC opcode type
1.4
	Decided to start keeping a history.
	Added some special handling of IFDEF in expandline()
	Changed macro usage: no parentheses around arg list, and args must be comma separated.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <ctype.h>
#include <stdarg.h>
#include <assert.h>

#define VERSION "1.7"

#define addr firstlabel.value	// '$' value
#define NOORIGIN -0x40000000	// nice even number so aligning works before origin is defined
#define INITLISTSIZE 128		// initial label list size
#define BUFFSIZE 8192			// file buffer (inputbuff, outputbuff) size
#define STACKBUFFSIZE 512       // stack-allocated buffer size.
#define IPS_RLE_EXTRACT 0x20    // if this many bytes in a row is encountered, use RLE encoding.
#define HEADERSIZE 0x10 		// size of an ines/nes2 header
#define WORDMAX 128				// used with getword()
#define LINEMAX 2048			// plenty of room for nested equates
#define MAXPASSES 7				// # of tries before giving up
#define IFNESTS 32				// max nested IF levels
#define DEFAULTFILLER 0			// default fill value
#define LOCALCHAR '@'

static void* true_ptr = &true_ptr;

enum labeltypes {LABEL,VALUE,EQUATE,MACRO,RESERVED};
enum cdltypes {NONE=0,CODE=1,DATA=2};
//	LABEL: known address
//	VALUE: defined with '='
//	EQUATE: made with EQU
//	MACRO: macro (duh)
//	RESERVED: reserved word

typedef struct {
	const char *name;		//label name
	// ptrdiff_t so it can hold function pointer on 64-bit machines
	ptrdiff_t value;		//PC (label), value (equate), param count (macro), funcptr (reserved)

	// [freem addition (from asm6_sonder.c)]
	// location in output file; used to determine bank when exporting labels
	// (only meaningful for labels corresponding to an address.)
	int pos;				

	char *line;			//for macro or equate, also used to mark unknown label
							//*next:text->*next:text->..
							//for macros, the first <value> lines hold param names
							//for opcodes (reserved), this holds opcode definitions, see initlabels
	int type;				//labeltypes enum (see above)
	int used;				//for EQU and MACRO recursion check
	int pass;				//when label was last defined
	int scope;				//where visible (0=global, nonzero=local)
	int ignorenl;			//[freem addition] output this label in .nl files? (0=yes, nonzero=no)
	void *link;			//labels that share the same name (local labels) are chained together
} label;

// this is the current program address (in memory). 
// it is incremented every time a byte is output.
// accessed in this file via the macro `addr`.
label firstlabel={		  //'$' label
	"$",//*name
	0,//value
	0,//[freem edit (from asm6_sonder.c)] pos
	(char*)&true_ptr,//*line
	VALUE,//type
	0,//used
	0,//pass
	0,//scope
	0,//[freem addition] ignorenl
	0,//link
};

typedef struct {
	char *text;
	int pos;
} comment;

typedef unsigned char byte;
typedef void (*icfn)(label*,char**);

//unstable instruction allowance
int allowunstable = 0;
int allowhunstable = 0;

//[nicklausw] ines stuff
int ines_include = 0;
int inesprg_num  = 0;
int ineschr_num  = 0;
int inesmir_num  = 0;
int inesmap_num  = 0;

int use_nes2 = 0;
int nes2chr_num = 0;
int nes2prg_num = 0;
int nes2sub_num = 0;
int nes2tv_num = 0;
int nes2vs_num = 0;
int nes2wram_num = 0;
int nes2bram_num = 0;
int nes2chrbram_num = 0;

// icfn definitions -- these all use the same signature, though
// not all of these functions make use of the label* argument.

void inesprg(label*, char**);
void ineschr(label*, char**);
void inesmir(label*, char**);
void inesmap(label*, char**);
void incines(label*,char**);

void nes2chrram(label*, char**);
void nes2prgram(label*, char**);
void nes2sub(label*, char**);
void nes2tv(label*, char**);
void nes2vs(label*, char**);
void nes2bram(label*, char**);
void nes2chrbram(label*, char**);

void opcode(label*,char**);
void org(label*,char**);
void base(label*,char**);
void pad(label*,char**);
void seekabs(label*, char**);
void seekrel(label*, char**);
void skiprel(label*, char**);
void equ(label*,char**);
void equal(label*,char**);
void nothing(label*,char**);
void include(label*,char**);
void incbin(label*,char**);
void incnes(label*,char**);
void clearpatch(label*,char**);
void dw(label*,char**);
void db(label*,char**);
void dl(label*,char**);
void dh(label*,char**);
void hex(label*,char**);
void dsw(label*,char**);
void dsb(label*,char**);
void align(label*,char**);
void _if(label*,char**);
void ifdef(label*,char**);
void ifndef(label*,char**);
void elseif(label*,char**);
void _else(label*,char**);
void endif(label*,char**);
void macro(label*,char**);
void endm(label*,char**);
void endr(label*,char**);
void rept(label*,char**);
void _enum(label*,char**);
void ende(label*,char**);
void ignorenl(label*,char**);		// [freem addition] "ignorenl"
void endinl(label*,char**);		// [freem addition] "endinl"
void fillval(label*,char**);
void compfill(label*,char**);   // [NaOH addition]
void endcompfill(label*,char**);   // [NaOH addition]
void expandmacro(label*,char**,int,char*);
void expandrept(int,char*);
void make_error(label*,char**);
void unstable(label*,char**);
void hunstable(label*,char**);

// forward declarations
label *findlabel(char*);
void initlabels();
label *newlabel();
void getword(char*,char**,int);
int getvalue(char**);
int getoperator(char**);
int eval(char**,int);
label *getreserved(char**);
int getlabel(char*,char**);
void processline(char*,char*,int);
void listline(char*,char*);
void endlist();
void flush_output(int);
char* find_ext(char*);
char* replace_ext(char*, char*);

// [freem addition (from asm6_sonder.c)]
int filepos=0;

// [NaOH] keeps track of last byte edited in file.
int filesize=0;

// ips offset for the start of the output buffer.
// if output buffer is empty, this should agree with filepos.
int ips_outpos=0;

typedef struct ips_hunk_t {
	int offset;
	int length;
	byte* contents; // if nullptr, rle.
	byte rle_content;
	int suppress; // if true, don't write to ips output.
	struct ips_hunk_t* next;
} ips_hunk;

// linked list of IPS hunks
ips_hunk* ips_hunk_head = 0;
ips_hunk* ips_hunk_tail = 0;

void ips_append_hunk();
// writes patch to output file
void ips_write(FILE* file);

enum optypes {ACC,IMM,IND,INDX,INDY,ZPX,ZPY,ABSX,ABSY,ZP,ABS,REL,IMP};
int opsize[]={0,1,2,1,1,1,1,2,2,1,2,1,0};
char ophead[]={0,'#','(','(','(',0,0,0,0,0,0,0,0};
char *optail[]={"A","",")",",X)","),Y",",X",",Y",",X",",Y","","","",""};
byte brk[]={0x00,IMM,0x00,ZP,0x00,IMP,-1};
byte ora[]={0x09,IMM,0x01,INDX,0x11,INDY,0x15,ZPX,0x1d,ABSX,0x19,ABSY,0x05,ZP,0x0d,ABS,-1};
byte asl[]={0x0a,ACC,0x16,ZPX,0x1e,ABSX,0x06,ZP,0x0e,ABS,0x0a,IMP,-1};
byte php[]={0x08,IMP,-1};
byte bpl[]={0x10,REL,-1};
byte clc[]={0x18,IMP,-1};
byte jsr[]={0x20,ABS,-1};
byte and[]={0x29,IMM,0x21,INDX,0x31,INDY,0x35,ZPX,0x3d,ABSX,0x39,ABSY,0x25,ZP,0x2d,ABS,-1};
byte bit[]={0x24,ZP,0x2c,ABS,-1};
byte rol[]={0x2a,ACC,0x36,ZPX,0x3e,ABSX,0x26,ZP,0x2e,ABS,0x2a,IMP,-1};
byte plp[]={0x28,IMP,-1};
byte bmi[]={0x30,REL,-1};
byte sec[]={0x38,IMP,-1};
byte rti[]={0x40,IMP,-1};
byte eor[]={0x49,IMM,0x41,INDX,0x51,INDY,0x55,ZPX,0x5d,ABSX,0x59,ABSY,0x45,ZP,0x4d,ABS,-1};
byte lsr[]={0x4a,ACC,0x56,ZPX,0x5e,ABSX,0x46,ZP,0x4e,ABS,0x4a,IMP,-1};
byte pha[]={0x48,IMP,-1};
byte jmp[]={0x6c,IND,0x4c,ABS,-1};
byte bvc[]={0x50,REL,-1};
byte cli[]={0x58,IMP,-1};
byte rts[]={0x60,IMP,-1};
byte adc[]={0x69,IMM,0x61,INDX,0x71,INDY,0x75,ZPX,0x7d,ABSX,0x79,ABSY,0x65,ZP,0x6d,ABS,-1};
byte ror[]={0x6a,ACC,0x76,ZPX,0x7e,ABSX,0x66,ZP,0x6e,ABS,0x6a,IMP,-1};
byte pla[]={0x68,IMP,-1};
byte bvs[]={0x70,REL,-1};
byte sei[]={0x78,IMP,-1};
byte sta[]={0x81,INDX,0x91,INDY,0x95,ZPX,0x9d,ABSX,0x99,ABSY,0x85,ZP,0x8d,ABS,-1};
byte sty[]={0x94,ZPX,0x84,ZP,0x8c,ABS,-1};
byte stx[]={0x96,ZPY,0x86,ZP,0x8e,ABS,-1};
byte dey[]={0x88,IMP,-1};
byte txa[]={0x8a,IMP,-1};
byte bcc[]={0x90,REL,-1};
byte tya[]={0x98,IMP,-1};
byte txs[]={0x9a,IMP,-1};
byte ldy[]={0xa0,IMM,0xb4,ZPX,0xbc,ABSX,0xa4,ZP,0xac,ABS,-1};
byte lda[]={0xa9,IMM,0xa1,INDX,0xb1,INDY,0xb5,ZPX,0xbd,ABSX,0xb9,ABSY,0xa5,ZP,0xad,ABS,-1};
byte ldx[]={0xa2,IMM,0xb6,ZPY,0xbe,ABSY,0xa6,ZP,0xae,ABS,-1};
byte tay[]={0xa8,IMP,-1};
byte tax[]={0xaa,IMP,-1};
byte bcs[]={0xb0,REL,-1};
byte clv[]={0xb8,IMP,-1};
byte tsx[]={0xba,IMP,-1};
byte cpy[]={0xc0,IMM,0xc4,ZP,0xcc,ABS,-1};
byte cmp[]={0xc9,IMM,0xc1,INDX,0xd1,INDY,0xd5,ZPX,0xdd,ABSX,0xd9,ABSY,0xc5,ZP,0xcd,ABS,-1};
byte dec[]={0xd6,ZPX,0xde,ABSX,0xc6,ZP,0xce,ABS,-1};
byte iny[]={0xc8,IMP,-1};
byte dex[]={0xca,IMP,-1};
byte bne[]={0xd0,REL,-1};
byte cld[]={0xd8,IMP,-1};
byte cpx[]={0xe0,IMM,0xe4,ZP,0xec,ABS,-1};
byte sbc[]={0xe9,IMM,0xe1,INDX,0xf1,INDY,0xf5,ZPX,0xfd,ABSX,0xf9,ABSY,0xe5,ZP,0xed,ABS,-1};
byte inc[]={0xf6,ZPX,0xfe,ABSX,0xe6,ZP,0xee,ABS,-1};
byte inx[]={0xe8,IMP,-1};
byte nop[]={0xea,IMP,-1};
byte beq[]={0xf0,REL,-1};
byte sed[]={0xf8,IMP,-1};

// asm6f addition:
/* Undocumented/Illegal Opcodes (NMOS 6502 only!) */
// names/information taken from http://www.oxyron.de/html/opcodes02.html

byte slo[]={0x07,ZP,0x17,ZPX,0x03,INDX,0x13,INDY,0x0f,ABS,0x1F,ABSX,0x1B,ABSY,-1};
byte rla[]={0x27,ZP,0x37,ZPX,0x23,INDX,0x33,INDY,0x2f,ABS,0x3f,ABSX,0x3b,ABSY,-1};
byte sre[]={0x47,ZP,0x57,ZPX,0x43,INDX,0x53,INDY,0x4f,ABS,0x5f,ABSX,0x5b,ABSY,-1};
byte rra[]={0x67,ZP,0x77,ZPX,0x63,INDX,0x73,INDY,0x6f,ABS,0x7f,ABSX,0x7b,ABSY,-1};
byte sax[]={0x87,ZP,0x97,ZPY,0x83,INDX,0x8f,ABS,-1};
byte lax[]={0xa7,ZP,0xb7,ZPY,0xa3,INDX,0xb3,INDY,0xaf,ABS,0xbf,ABSY,-1};
byte dcp[]={0xc7,ZP,0xd7,ZPX,0xc3,INDX,0xd3,INDY,0xcf,ABS,0xdf,ABSX,0xdb,ABSY,-1};
byte isc[]={0xe7,ZP,0xf7,ZPX,0xe3,INDX,0xf3,INDY,0xef,ABS,0xff,ABSX,0xfb,ABSY,-1};

byte anc[]={0x0b,IMM,-1}; // has duplicate at 0x2b

byte alr[]={0x4b,IMM,-1};
byte arr[]={0x6b,IMM,-1};
byte axs[]={0xcb,IMM,-1};
byte las[]={0xbb,ABSY,-1};

// "unstable in certain matters":
byte ahx[]={0x93,INDY,0x9f,ABSY,-1};
byte shy[]={0x9c,ABSX,-1};
byte shx[]={0x9e,ABSY,-1};
byte tas[]={0x9b,ABSY,-1};

// "highly unstable (results are not predictable on some machines)":
byte xaa[]={0x8b,IMM,-1};
//byte lax[]={0xab,IMM,-1};
 
void *rsvdlist[]={	   //all reserved words
		"BRK",brk,
		"PHP",php,
		"BPL",bpl,
		"CLC",clc,
		"JSR",jsr,
		"PLP",plp,
		"BMI",bmi,
		"SEC",sec,
		"RTI",rti,
		"PHA",pha,
		"BVC",bvc,
		"CLI",cli,
		"RTS",rts,
		"PLA",pla,
		"BVS",bvs,
		"SEI",sei,
		"DEY",dey,
		"BCC",bcc,
		"TYA",tya,
		"LDY",ldy,
		"TAY",tay,
		"BCS",bcs,
		"CLV",clv,
		"CPY",cpy,
		"INY",iny,
		"BNE",bne,
		"CLD",cld,
		"CPX",cpx,
		"INX",inx,
		"BEQ",beq,
		"SED",sed,
		"ORA",ora,
		"AND",and,
		"EOR",eor,
		"ADC",adc,
		"STA",sta,
		"LDA",lda,
		"CMP",cmp,
		"SBC",sbc,
		"ASL",asl,
		"ROL",rol,
		"LSR",lsr,
		"ROR",ror,
		"TXA",txa,
		"TXS",txs,
		"LDX",ldx,
		"TAX",tax,
		"TSX",tsx,
		"DEX",dex,
		"NOP",nop,
		"BIT",bit,
		"JMP",jmp,
		"STY",sty,
		"STX",stx,
		"DEC",dec,
		"INC",inc,

		/* begin undocumented/illegal opcodes */
		"SLO",slo,
		"RLA",rla,
		"SRE",sre,
		"RRA",rra,
		"SAX",sax,
		"LAX",lax,
		"DCP",dcp,
		"ISC",isc,
		"ANC",anc,
		"ALR",alr,
		"ARR",arr,
		"AXS",axs,
		"LAS",las,

		/* somewhat unstable instructions */
		"AHX",ahx,
		"SHY",shy,
		"SHX",shx,
		"TAS",tas,

		/* highly unstable instructions */
		"XAA",xaa,

		/* end list */
		0, 0
};

char *unstablelist[]={
	"AHX", "SHY", "SHX", "TAS"
};

struct {
	char* name;
	void (*func)( label*, char** );
} directives[]={
		{"",nothing},
		{"IF",_if},
		{"ELSEIF",elseif},
		{"ELSE",_else},
		{"ENDIF",endif},
		{"IFDEF",ifdef},
		{"IFNDEF",ifndef},
		{"=",equal},
		{"EQU",equ},
		{"ORG",org},
		{"BASE",base},
		{"PAD",pad},
		{"SEEKABS",seekabs},
		{"SEEKREL",seekrel},
		{"SKIPREL",skiprel},
		{"INCLUDE",include},{"INCSRC",include},
		{"INCBIN",incbin},{"BIN",incbin},
		{"INCNES",incnes},
		{"CLEARPATCH",clearpatch},
		{"HEX",hex},
		{"WORD",dw},{"DW",dw},{"DCW",dw},{"DC.W",dw},
		{"BYTE",db},{"DB",db},{"DCB",db},{"DC.B",db},
		{"DSW",dsw},{"DS.W",dsw},
		{"DSB",dsb},{"DS.B",dsb},
		{"ALIGN",align},
		{"MACRO",macro},
		{"REPT",rept},
		{"ENDM",endm},
		{"ENDR",endr},
		{"ENUM",_enum},
		{"ENDE",ende},
		{"IGNORENL",ignorenl},
		{"ENDINL",endinl},
		{"FILLVALUE",fillval},
		{"COMPARE", compfill},
		{"ENDCOMPARE", endcompfill},
		{"DL",dl},
		{"DH",dh},
		{"ERROR",make_error},
		{"INESPRG",inesprg},
		{"INESCHR",ineschr},
		{"INESMIR",inesmir},
		{"INESMAP",inesmap},
		{"INCINES",incines},
		{"NES2CHRRAM",nes2chrram},
		{"NES2PRGRAM",nes2prgram},
		{"NES2SUB",nes2sub},
		{"NES2TV",nes2tv},
		{"NES2VS",nes2vs},
		{"NES2BRAM",nes2bram},
		{"NES2CHRBRAM",nes2chrbram},
		{"UNSTABLE",unstable},
		{"HUNSTABLE",hunstable},
		{0, 0}
};

char OutOfRange[]="Value out of range.";
char SeekOutOfRange[]="Seek position out of range.";
char BadIncbinSize[]="INCBIN size is out of range.";
char NotANumber[]="Not a number.";
char UnknownLabel[]="Unknown label.";
char Illegal[]="Illegal instruction.";
char IncompleteExp[]="Incomplete expression.";
char LabelDefined[]="Label already defined.";
char MissingOperand[]="Missing operand.";
char DivZero[]="Divide by zero.";
char BadAddr[]="Can't determine address.";
char NeedName[]="Need a name.";
char CantCreateFile[]="Can't create output file.";
char CantOpen[]="Can't open file.";
char CantWrite[]="Write error.";
char CompFailed[]="Compare failed. Byte at 0x________ was 0x++.";
char CantSeek[]="Can't seek in file.";
char CantSeekEnum[]="Can't seek in enum mode.";
char InvalidHeader[]="iNES header invalid.";
char ExtraENDM[]="ENDM without MACRO.";
char ExtraENDR[]="ENDR without REPT.";
char ExtraENDE[]="ENDE without ENUM.";
char ExtraENDINL[]="ENDINL without IGNORENL.";
char RecurseMACRO[]="Recursive MACRO not allowed.";
char RecurseEQU[]="Recursive EQU not allowed.";
char NoENDIF[]="Missing ENDIF.";
char NoENDM[]="Missing ENDM.";
char NoENDR[]="Missing ENDR.";
char NoENDE[]="Missing ENDE.";
char NoENDINL[]="Missing ENDINL.";
char IfNestLimit[]="Too many nested IFs.";
char undefinedPC[]="PC is undefined (use ORG first)";

char whitesp[]=" \t\r\n:";  //treat ":" like whitespace (for labels)
char whitesp2[]=" \t\r\n\"";	//(used for filename processing)
char tmpstr[LINEMAX];   //all purpose big string

int pass=0;
int scope;//current scope, 0=global
int nextscope;//next nonglobal scope (increment on each new block of localized code)
int lastchance=0;//set on final attempt
int needanotherpass;//still need to take care of some things..
int error=0;//hard error (stop assembly after this pass)
char **makemacro=0;//(during macro creation) where next macro line will go.  1 to skip past macro
char **makerept;//like makemacro.. points to end of string chain
int reptcount=0;//counts rept statements during rept string storage
int iflevel=0;//index into ifdone[],skipline[]
int ifdone[IFNESTS];//nonzero if current IF level has been true
int skipline[IFNESTS];//1 on an IF statement that is false
const char *errmsg;
char *inputfilename=0;
char *outputfilename=0;
char *ipsfilename=0;
char *listfilename=0;
char *cdlfilename=0;
int verboselisting=0;//expand REPT loops in listing
int genfceuxnl=0;//[freem addition] generate FCEUX .nl files for symbolic debugging
int genmesenlabels=0; //generate label files for use with Mesen
int gencdl=0; //generate CDL file
int genlua=0;//generate lua symbol file
int genips=0; //[NaOH] generate .ips patch.
const char *listerr=0;//error message for list file
label *labelhere;//points to the label being defined on the current line (for EQU, =, etc)
FILE *listfile=0;
FILE *outputfile=0;
FILE *cdlfile=0;
byte outputbuff[BUFFSIZE];
byte inputbuff[BUFFSIZE];
byte ines_extension[HEADERSIZE];
byte ines_extension_mask[HEADERSIZE] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
int outcount;//bytes waiting in outputbuff
label **labellist;  //array of label pointers (list starts from center and grows outward)
int labels;//# of labels in labellist
int maxlabels;//max # of labels labellist can hold
int labelstart;//index of first label
int labelend;//index of last label
label *lastlabel;//last label created
comment **comments;
int commentcount;
int commentcapacity;
int lastcommentpos = -1;
int nooutput=0;//supress output (use with ENUM)
int nonl=0;//[freem addition] supress output to .nl files
int defaultfiller;//default fill value
int comparefiller=0; // compare on write to defaultfiller
int insidemacro=0;//macro/rept is being expanded
int verbose=1;

static void* ptr_from_bool( int b )
{
	if ( b )
		return true_ptr;
	
	return NULL;
}

// Prints printf-style message to stderr, then exits.
// Closes and deletes output file.
static void fatal_error( const char fmt [], ... )
{
	va_list args;
	
	if ( outputfile != NULL ) {
		fclose( outputfile );
		remove( outputfilename );
	}
	
	va_start( args, fmt );
	fprintf( stderr, "\nError: " );
	vfprintf( stderr, fmt, args );
	fprintf( stderr, "\n\n" );
	va_end( args );
	
	exit( EXIT_FAILURE );
}

// Prints printf-style message if verbose mode is enabled.
static void message( const char fmt [], ... )
{
	if ( verbose ) {
		va_list args;
		va_start( args, fmt );
		vprintf( fmt, args );
		va_end( args );
	}
}

// Same as malloc(), but prints error and exits if allocation fails
static char* my_malloc( size_t s )
{
	char* p = malloc( s ? s : 1 );
	if ( p == NULL )
		fatal_error( "out of memory" );
	
	return p;
}

// Same as common strdup(), but prints error and exits if allocation fails
static char* my_strdup(const char *in)
{
	size_t size = strlen( in ) + 1;
	char* out = my_malloc( size );
		memcpy( out, in, size );
	return out;
}

//-------------------------------------------------------
//parsing functions
//-------------------------------------------------------

// Not all systems support this, so we implement our own always.
// More problematic to try to use the system's version rather than
// ours in all cases.
char *my_strupr(char *string)
{
	char *s;
	
	if (string == NULL) {
		return (char *)NULL;
	}


	for (s = string; *s; ++s) {
		*s = toupper((unsigned char) *s);
	}

	return string;
}

// takes a hex character ('0'-'F'), returns value.
int hexify(int i) {
	if(i>='0' && i<='9') {
		return i-'0';
	} else if(i>='a' && i<='f') {
		return i-('a'-10);
	} else if(i>='A' && i<='F') {
		return i-('A'-10);
	} else {
		errmsg=NotANumber;
		return 0;
	}
}

#define eatwhitespace(str) (*str+=strspn(*str,whitesp))

//find end of str, excluding any chars in whitespace
char *strend(char *str, char *whitespace) {
	char c;
	char *w=whitespace;
	char *end=str+strlen(str);
	while(*w && end!=str) {
		for(w=whitespace, c=end[-1]; *w; w++) {
			if(*w==c) {
				end--;
				break;
			}
		}
	}
	return end;
}

//decode str into a number
//set errmsg on error
char gvline[WORDMAX];
int dependant;//set to nonzero if symbol couldn't be resolved
int getvalue(char **str) {
	char *s,*end;
	int ret,chars,j;
	label *p;

	getword(gvline,str,1);

	s=gvline;
	if(!*s) {
		errmsg=MissingOperand;
		return 0;
	}
	
	ret=chars=0;
	if(*s=='$') {   //hex---------------------
		s++;
		if(!*s) {
			ret=addr;//$ by itself is the PC			
		} else do {
hexi:	   j=hexify(*s);
			s++;
			chars++;
			ret=(ret<<4)|j;
		} while(*s);
		if(chars>8)
			errmsg=OutOfRange;
	} else if(*s=='%') {	//binary----------------------
		s++;
	do {
bin:		j=*s;
			s++;
			chars++;
			j-='0';
			if(j>1) {
				errmsg=NotANumber;
			}
			ret=(ret<<1)|j;
		} while(*s);
		if(chars>32)
			errmsg=OutOfRange;
	} else if(*s=='\'') {   //char-----------------
		s++;
		if(*s=='\\') s++;
		ret=*s;
		s++;
		if(*s!='\'')
			errmsg=NotANumber;
	} else if(*s=='"') {	//char 2-----------------
		s++;
		if(*s=='\\') s++;
		ret=*s;
		s++;
		if(*s!='"')
			errmsg=NotANumber;
	} else if(*s>='0' && *s<='9') {//number--------------
		end=s+strlen(s)-1;
		if(strspn(s,"0123456789")==strlen(s))
			ret=atoi(s);
		else if(*end=='b' || *end=='B') {
			*end=0;
			goto bin;
		} else if(*end=='h' || *end=='H') {
			*end=0;
			goto hexi;
		} else
			errmsg=NotANumber;
	} else {	//label---------------
		p=findlabel(gvline);
		if(!p) {//label doesn't exist (yet?)
			needanotherpass=dependant=1;
			if(lastchance) {//only show error once we're certain label will never exist
				errmsg=UnknownLabel;
			}
		} else {
			dependant|=!(*p).line;
			needanotherpass|=!(*p).line;
			if((*p).type==LABEL || (*p).type==VALUE) {
				ret=(*p).value;
			} else if((*p).type==MACRO) {
				errmsg="Can't use macro in expression.";
			} else {//what else is there?
				errmsg=UnknownLabel;
			}
		}
	}
	return ret;
}

char mathy[]="!^&|+-*/%()<>=,";
enum prectypes {WHOLEEXP,ORORP,ANDANDP,ORP,XORP,ANDP,EQCOMPARE,COMPARE,SHIFT,PLUSMINUS,MULDIV,UNARY};//precedence levels
enum operators {NOOP,EQUAL,NOTEQUAL,GREATER,GREATEREQ,LESS,LESSEQ,PLUS,MINUS,MUL,DIV,MOD,AND,XOR,OR,ANDAND,OROR,LEFTSHIFT,RIGHTSHIFT};//all operators
char prec[]={WHOLEEXP,EQCOMPARE,EQCOMPARE,COMPARE,COMPARE,COMPARE,COMPARE,PLUSMINUS,PLUSMINUS,MULDIV,MULDIV,MULDIV,ANDP,XORP,ORP,ANDANDP,ORORP,SHIFT,SHIFT};//precedence of each operator
//get operator from str and advance str
int getoperator(char **str) {
	*str+=strspn(*str,whitesp);	 //eatwhitespace
	(*str)++;
	switch(*(*str-1)) {
		case '&':
			if(**str=='&') {
				(*str)++;
				return ANDAND;
			} else
				return AND;
		case '|':
			if(**str=='|') {
				(*str)++;
				return OROR;
			} else
				return OR;
		case '^':
			return XOR;
		case '+':
			return PLUS;
		case '-':
			return MINUS;
		case '*':
			return MUL;
		case '%':
			return MOD;
		case '/':
			return DIV;
		case '=':
			if(**str=='=')
				(*str)++;
			return EQUAL;
		case '>':
			if(**str=='=') {
				(*str)++;
				return GREATEREQ;
			} else if(**str=='>') {
				(*str)++;
				return RIGHTSHIFT;
			} else
				return GREATER;
		case '<':
			if(**str=='=') {
				(*str)++;
				return LESSEQ;
			} else if(**str=='>') {
				(*str)++;
				return NOTEQUAL;
			} else if(**str=='<') {
				(*str)++;
				return LEFTSHIFT;
			} else
				return LESS;
		case '!':
			if(**str=='=') {
				(*str)++;
				return NOTEQUAL;
			}
			//(to default)
		default:
			(*str)--;
			return NOOP;
	}
}

//evaluate expression in str and advance str
int eval(char **str,int precedence) {
	char unary;
	char *s,*s2;
	int ret,val2;
	int op;
	
	s=*str+strspn(*str,whitesp);		//eatwhitespace
	unary=*s;
	switch(unary) {
		case '(':
			s++;
			ret=eval(&s,WHOLEEXP);
			s+=strspn(s,whitesp);	   //eatwhitespace
			if(*s==')')
				s++;
			else
				errmsg=IncompleteExp;
			break;
		case '#':
			s++;
			ret=eval(&s,WHOLEEXP);
			break;
		case '~':
			s++;
			ret=~eval(&s,UNARY);
			break;
		case '!':
			s++;
			ret=!eval(&s,UNARY);
			break;
		case '<':
			s++;
			ret=eval(&s,UNARY)&0xff;
			break;
		case '>':
			s++;
			ret=(eval(&s,UNARY)>>8)&0xff;
			break;
		case '+':
		case '-':
			//careful.. might be +-label
			s2=s;
			s++;
			op=dependant;//eval() is reentrant so don't mess up dependant
			val2=needanotherpass;
			dependant=0;
			ret=getvalue(&s2);
			if (errmsg == UnknownLabel)
				errmsg=0;
			if(!dependant || s2==s) {//found something or single + -
				s=s2;
				s2=0;//flag that we got something
				dependant|=op;
			} else {//not a label after all..
				dependant=op;
				needanotherpass=val2;
			}
			if(s2) {//if it wasn't a +-label
				ret=eval(&s,UNARY);
				if(unary=='-') ret=-ret;
			}
			break;
		default:
			ret=getvalue(&s);
	}
	do {
		*str=s;
		op=getoperator(&s);
		if(precedence<prec[op]) {
			val2=eval(&s,prec[op]);
			if(!dependant) switch(op) {
				case AND:
					ret&=val2;
					break;
				case ANDAND:
					ret=ret&&val2;
					break;
				case OR:
					ret|=val2;
					break;
				case OROR:
					ret=ret||val2;
					break;
				case XOR:
					ret^=val2;
					break;
				case PLUS:
					ret+=val2;
					break;
				case MINUS:
					ret-=val2;
					break;
				case MUL:
					ret*=val2;
					break;
				case DIV:
					if(!val2) errmsg=DivZero;
					else ret/=val2;
					break;
				case MOD:
					if(!val2) errmsg=DivZero;
					else ret%=val2;
					break;
				case EQUAL:
					ret=(ret==val2);
					break;
				case NOTEQUAL:
					ret=(ret!=val2);
					break;
				case GREATER:
					ret=ret>val2;
					break;
				case GREATEREQ:
					ret=ret>=val2;
					break;
				case LESS:
					ret=ret<val2;
					break;
				case LESSEQ:
					ret=ret<=val2;
					break;
				case LEFTSHIFT:
					ret<<=val2;
					break;
				case RIGHTSHIFT:
					ret>>=val2;
					break;
			} else ret=0;
		}
	} while(precedence<prec[op] && !errmsg);
	return ret;
}

//copy next word from src into dst and advance src
//mcheck=1 to crop mathy stuff (0 for filenames,etc)
void getword(char *dst,char **src,int mcheck) {
	*src+=strspn(*src,whitesp);//eatwhitespace
	strncpy(dst,*src,WORDMAX-1);
	dst[WORDMAX-1]=0;
	strtok(dst,whitesp);//no trailing whitespace
	if(mcheck) strtok(dst,mathy);
	*src+=strlen(dst);
	if(**src==':') (*src)++;//cheesy fix for rept/macro listing
}

//grab string with optional quotes
void getfilename(char *dst, char **next) {
	char *s,*end;
	int len;
	eatwhitespace(next);
	if(**next=='"') { //look for end quote, grab everything inside
		s=*next+1;
		end=strchr(s,'"');
		if(end) {
			len=(int)(end-s);
			memcpy(dst,s,len); dst[len]=0;
			*next=end+1;
		} else { //no end quote.. grab everything minus trailing whitespace
			end=strend(s,whitesp);
			len=(int)(end-s);
			memcpy(dst,s,len); dst[len]=0;
			*next=end;
		}
	} else {
		getword(dst,next,0);
	}
}

//get word in src, advance src, and return reserved label*
label *getreserved(char **src) {
	char dst[WORDMAX];
	char upp[WORDMAX];
	label *p;
	
	*src+=strspn(*src,whitesp);//eatwhitespace
	if(**src=='=') {//special '=' reserved word
		upp[0]='=';
		upp[1]=0;
		(*src)++;
	} else {
		if(**src=='.')//reserved words can start with "."
			(*src)++;
		getword(dst,src,1);
		strcpy(upp,dst);
		my_strupr(upp);
	}

	p=findlabel(upp);//case insensitive reserved word
	if(!p) p=findlabel(dst);//or case sensitive macro
	if(p) {
		if((*p).type==MACRO) {
			if((*p).pass!=pass)
				p=0;
		} else if((*p).type!=RESERVED)
			p=0;
	}
	if(!p) errmsg=Illegal;
	return p;
}

//copy word to dst, advance src
//return true if it looks like a label
int getlabel(char *dst,char **src) {
	char *s;
	char c;
	
	getword(dst,src,1);
	if(*dst=='$'&&!dst[1])//'$' label
		return 1;

	s=dst;//+label, -label
	c=*s;
	if(c=='+' || c=='-') {
		do s++; while(*s==c);
		if(!*s)//just ++.. or --.., no text
			return 1;
	}
	c=*s;
	if(c==LOCALCHAR || c=='_' || (c>='A' && c<='Z') || (c>='a' && c<='z')) {//label can start with these
		return 1;
	} else {
		errmsg=Illegal;//fucked up instruction
		return 0;
	}
}

//Expand all equates from src into dst, and remove comment
//returns a pointer to the comment in src or null.
//CRIPES what a mess...
char *expandline(char *dst,char *src) {
	char *start;
	char *comment=0;
	char c,c2;
	label *p;
	int def_skip=0;
	char upp[WORDMAX];

	do {
		c=*src;
		if(c=='$' || (c>='0' && c<='9')) {//read past numbers (could be mistaken for a symbol, i.e. $BEEF)
			do {
				*dst=c;
				src++;
				dst++;
				c=*src;
			// note that we skip numbers of form '0x...' and '32H' etc. (see getvalue())
			} while((c>='0' && c<='9') || (c>='A' && c<='H') || (c>='a' && c<='h'));
			c=1;//don't terminate yet
		} else if(c=='"' || c=='\'') {//read past quotes
			*dst=c;
			dst++; src++;
			do {
				*dst=c2=*src;
				if(c2=='\\') {
					dst++; src++;
					*dst=*src;
				}
				dst++; src++;
			} while(c2 && c2!=c);
			c=c2;
		} else if(c=='_' || c=='.' || c==LOCALCHAR || (c>='A' && c<='Z') || (c>='a' && c<='z')) {//symbol
			start=src;
			do {//scan to end of symbol
				src++;
				c=*src;
			} while(c=='_' || c=='.' || c==LOCALCHAR || (c>='0' && c<='9') || (c>='A' && c<='Z') || (c>='a' && c<='z'));

			*src=0; //terminate @ end of word (temporarily)

			/*
			ghey hack.
			expandline() is called quite early during parsing, so

			FOO equ xxxx
			ifdef FOO
				
			  becomes

			FOO equ xxxx
			ifdef xxxx

			rendering IFDEF useless, so we will bypass expansion in this special case.
			I'm avoiding getreserved() because it does searching and other unnecessary crap.
			*/
			p=0;
			if(!def_skip) {
				strcpy(upp,start+(*start=='.'));
				my_strupr(upp);
				if(!strcmp(upp,"IFDEF") || !strcmp(upp,"IFNDEF")) {
					def_skip=1;
				} else {
					p=findlabel(start);
				}
			}

			if(p) {
				if((*p).type!=EQUATE || (*p).pass!=pass)//equates MUST be defined before being used otherwise they will be expanded in their own definition
					p=0;//i.e. (label equ whatever) gets translated to (whatever equ whatever)
				else {
					if((*p).used) {
						p=0;
						errmsg=RecurseEQU;
					}
				}
			}
			if(p) {
				(*p).used=1;
				expandline(dst,(*p).line);
				(*p).used=0;
			} else {
				strcpy(dst,start);
			}
			dst+=strlen(dst);
			*src=c;
		} else {
			if(c==';') {//comment
				c=0;
				comment=src;
			}
			*dst=c;
			dst++;
			src++;
		}
	} while(c);
	return comment;
}

int eatchar(char **str,char c) {
	if(c) {
		*str+=strspn(*str,whitesp);	 //eatwhitespace
		if(**str==c) {
			(*str)++;
			return 1;
		} else 
			return 0;
	}
	return 1;
}

//reverse string
void reverse(char *dst,char *src) {
	dst+=strlen(src);
	*dst=0;
	while(*src)
		*(--dst)=*(src++);
}

//===========================================================================================================
/* [freem addition(imported code from asm6_sonder.c)] */
void export_labelfiles() {
	// iterate through all the labels and output FCEUX-compatible label info files
	// based on their type (LABEL's,EQUATE's,VALUE's), address (ram/rom), and position (bank)

	int i;
	int bank;
	label *l;
	char str[512];
	char filename[512];
	char* strptr;
	FILE* bankfiles[64];
	FILE* ramfile;

	for(i=0;i<64;i++){ bankfiles[i]=0; }

	// ram file: <output>.ram.nl
	// bank files: <output>.bank#hex.nl

	strcpy(filename, outputfilename);
	strptr = find_ext(filename);
	sprintf(strptr, ".nes.ram.nl");
	ramfile= fopen(filename, "w");

	// the bank files are created ad-hoc before being written to.

	// iNES banks are 16kb. Subtracting the 16 byte header and dividing that by
	// 16384 will get us the bank number of a particular label.
	// this only applies to labels that are $8000 and up.
	// Anything below $8000 is assumed to be RAM.

	// todo: include EQUATES for other registers and variables

	for(i=labelstart;i<=labelend;i++){
		l=labellist[i];

		// [freem addition]: handle IGNORENL'd labels
		if((*l).ignorenl)
			continue;

		if( 
			(
				(*l).type==LABEL ||
				(((*l).type==EQUATE || (*l).type==VALUE) && strlen((*l).name) > 1)
			)
				// only positive values at most 0xffff can be addresses
				&& (*l).value < 0x10000 && (*l).value >= 0
		){
			sprintf(str,"$%04X#%s#\n",(unsigned int)(*l).value,(*l).name);
			// puts(str);

			if((*l).value < 0x8000){
				// RAM
				fwrite((const void *)str,1,strlen(str),ramfile);
			}
			else{
				// ROM
				bank=(((*l).pos - HEADERSIZE)/16384);
				if (!bankfiles[bank]){
					sprintf(strptr,".nes.%X.nl",bank);
					bankfiles[bank]=fopen(filename,"w");
				}
				fwrite((const void *)str,1,strlen(str),bankfiles[bank]);
			}
		}
	}

	fclose ( ramfile );

	for(i=0;i<64;i++){
		if(bankfiles[i]) fclose(bankfiles[i]);
	}
}


void export_lua() {
	// iterate through all the labels and output Lua-compatible label info files

	int i;
	label *l;
	char str[512];
	char* filename;
	FILE* mainfile;
	
	filename = replace_ext(outputfilename, ".lua");
	mainfile=fopen(filename, "w");
	free(filename);

	for(i=labelstart;i<=labelend;i++){
		l=labellist[i];

		if( 
			(
				(*l).type==LABEL ||
				(((*l).type==EQUATE || (*l).type==VALUE) && strlen((*l).name) > 1)
			)
				// no anonymous labels
				&& (*l).name[0] != '-'
				&& (*l).name[0] != '+'
		){
			sprintf(str,"%s = 0x%04X\n",(*l).name,(unsigned int)(*l).value);
			fwrite((const void *)str,1,strlen(str),mainfile);
		}
	}

	fclose ( mainfile );
}

int comparelabels(const void* arg1, const void* arg2)
{
	const label* a = *((label**)arg1);
	const label* b = *((label**)arg2);
	if(a->type > b->type) return 1;
	if(a->type < b->type) return -1;
	if(a->pos > b->pos) return 1;
	if(a->pos < b->pos) return -1;
	if(a->value > b->value) return 1;
	if(a->value < b->value) return -1;
	return strcmp(a->name, b->name);
}

int comparecomments(const void* arg1, const void* arg2)
{
	const comment* a = *((comment**)arg1);
	const comment* b = *((comment**)arg2);
	if(a->pos > b->pos) return 1;
	if(a->pos < b->pos) return -1;
	return strcmp(a->text, b->text);
}

void export_mesenlabels() {
	// iterate through all the labels and output Mesen-compatible label files
	// based on their type (LABEL's,EQUATE's,VALUE's) and address (ram/rom)
	int i;
	char* commenttext;
	label *l;
	char str[512];
	char* filename;
	FILE* outfile;

	filename = replace_ext(outputfilename, ".mlb");
	outfile = fopen(filename, "w");
	free(filename);

	int currentcomment = 0;

	qsort(labellist + labelstart, labelend - labelstart + 1, sizeof(label*), comparelabels);
	qsort(comments, commentcount, sizeof(comment*), comparecomments);

	for(i = labelstart; i<=labelend; i++) {
		l = labellist[i];

		if(l->value >= 0x10000 || l->name[0] == '+' || l->name[0] == '-' || l->value < 0) {
			//Ignore CHR & anonymous code labels
			continue;
		}

		if(l->type == LABEL) {
			//Labels in the actual code
			if(l->pos < HEADERSIZE) {
				//Ignore file header
				continue;
			}

			//Check if one or more comments match this address
			commenttext = 0;
			while(currentcomment < commentcount) {
				comment* c = comments[currentcomment];

				if(c->pos < l->pos) {
					//This comment is for a line before the current code label, write it to the file right away
					if(c->pos >= HEADERSIZE) {
						sprintf(str, "P:%04X::", (unsigned int)c->pos - HEADERSIZE);
						fwrite((const void *)str, 1, strlen(str), outfile);
						fwrite((const void *)c->text, 1, strlen(c->text), outfile);
						fwrite("\n", 1, 1, outfile);
					}
					currentcomment++;
				} else if(c->pos == l->pos) {
					//Same address, write it on the same line as the label
					commenttext = c->text;
					currentcomment++;
					break;
				} else {
					break;
				}
			}

			//Dump the label
			sprintf(str, "P:%04X:%s", (unsigned int)(l->pos - HEADERSIZE), l->name);
			fwrite((const void *)str, 1, strlen(str), outfile);

			if(commenttext) {
				fwrite(":", 1, 1, outfile);
				fwrite((const void *)commenttext, 1, strlen(commenttext), outfile);
			}
			fwrite("\n", 1, 1, outfile);
		} else if(l->type == VALUE || l->type == EQUATE) {
			//These are potentially aliases for variables in RAM, or read/write registers, etc.
			if(l->value < 0x2000) {
				//Assume nes internal RAM below $2000 (2kb)
				sprintf(str, "R:%04X:%s\n", (unsigned int)l->value, l->name);
			} else if(l->value >= 0x6000 && l->value < 0x8000) {
				//Assume save/work RAM ($6000-$7FFF), dump as both. (not the best solution - maybe an option?)
				sprintf(str, "S:%04X:%s\n", (unsigned int)l->value - 0x6000, l->name);
				sprintf(str, "W:%04X:%s\n", (unsigned int)l->value - 0x6000, l->name);
			} else {
				//Assume a global register for everything else (e.g $8000 for mapper control, etc.)
				sprintf(str, "G:%04X:%s\n", (unsigned int)l->value, l->name);
			}
			fwrite((const void *)str, 1, strlen(str), outfile);
		}
	}

	fclose(outfile);
}

//local:
//  false: if label starts with LOCALCHAR, make it local, otherwise it's global
//  true: force label to be local (used for macros)
void addlabel(char *word, int local) {
	char c=*word;
	label *p=findlabel(word);
	if(p && local && !(*p).scope && (*p).type!=VALUE) //if it's global and we're local
		p=0;//pretend we didn't see it (local label overrides global of the same name)
	//global labels advance scope
	if(c!=LOCALCHAR && !local) {
		scope=nextscope++;
	}
	if(!p) {//new label
		labelhere=newlabel();
		if(!(*labelhere).name)//name already set if it's a duplicate
			(*labelhere).name=my_strdup(word);
		(*labelhere).type=LABEL;//assume it's a label.. could mutate into something else later
		(*labelhere).pass=pass;
		(*labelhere).value=addr;
		(*labelhere).line=ptr_from_bool(addr>=0);
		(*labelhere).used=0;

		// [freem edit (from asm6_sonder.c)]
		(*labelhere).pos=filepos;

		// [freem addition]
		(*labelhere).ignorenl=nonl;

		if(c==LOCALCHAR || local) { //local
			(*labelhere).scope=scope;
		} else {		//global
			(*labelhere).scope=0;
		}
		lastlabel=labelhere;
	} else {//old label
		labelhere=p;
		if((*p).pass==pass && c!='-') {//if this label already encountered
			if((*p).type==VALUE)
				return;
			else
				errmsg=LabelDefined;
		} else {//first time seen on this pass or (-) label
			(*p).pass=pass;
			if((*p).type==LABEL) {
				if((*p).value!=addr && c!='-') {
					needanotherpass=1;//label position is still moving around
					if(lastchance)
						errmsg=BadAddr;
				}
				(*p).value=addr;
				(*p).pos=filepos;
				(*p).line=ptr_from_bool(addr>=0);
				if(lastchance && addr<0)
					errmsg=BadAddr;
			}
		}
	}
}

//initialize label list
void initlabels(void) {
	label *p;
	int i=0;
	
	labels=1;
	labellist=(label**)my_malloc(INITLISTSIZE*sizeof(label*));
	labelstart=INITLISTSIZE/2;
	labelend=labelstart;
	maxlabels=INITLISTSIZE;
	labellist[labelstart]=&firstlabel;//'$' label
	
	//add reserved words to label list
	
	do {//opcodes first
		findlabel(rsvdlist[i]);//must call findlabel before using newlabel
		p=newlabel();
		(*p).name=rsvdlist[i];
		(*p).value=(ptrdiff_t)opcode;
		(*p).line=rsvdlist[i+1];
		(*p).type=RESERVED;
		i+=2;
	} while(rsvdlist[i]);

	i = 0;
	do {//other reserved words now
		findlabel(directives[i].name);
		p=newlabel();
		(*p).name=directives[i].name;
		(*p).value=(ptrdiff_t)directives[i].func;
		(*p).type=RESERVED;
		i++;
	} while(directives[i].name);
	lastlabel=p;
}

void initcomments(void) {
	commentcount= 0;
	commentcapacity = 1000;
	comments = (comment**)my_malloc(commentcapacity * sizeof(comment*));
}

void growcommentlist(void) {
	if(commentcount == commentcapacity) {
		void* oldcomments = comments;
		commentcapacity *= 2;
		comments = (comment**)my_malloc(commentcapacity * sizeof(comment*));
		memcpy(comments, oldcomments, commentcount * sizeof(comment*));
		free(oldcomments);
	}
}

void addcomment(char* text) {
	static int oldpass = 0;
	if(oldpass != pass) {
		oldpass = pass;
		commentcount = 0;
	}

	text++; //ignore the leading ";"

	if(lastcommentpos == filepos) {
		//Append comment to the previous comment, since they are for the same address
		comment* c = comments[commentcount - 1];
		char* oldtext = c->text;
		int oldtextlen = strlen(oldtext);
		char* newtext = my_malloc(oldtextlen + strlen(text) + 4);
		strcpy(newtext, oldtext);
		strcpy(newtext + oldtextlen, "\\n");
		
		//Get rid of last character (newline \n)
		strcpy(newtext + oldtextlen + 2, text);
		newtext[strlen(newtext) - 1] = '\0';
		c->text = newtext;
	} else {
		//Add a new comment
		growcommentlist();

		comment* c = (comment*)my_malloc(sizeof(comment));
		c->pos = filepos;
		c->text = my_malloc(strlen(text)+1);
		strcpy(c->text, text);		
		
		//Get rid of last character (newline \n)
		c->text[strlen(text) - 1] = '\0';
		
		comments[commentcount] = c;
		commentcount++;

		lastcommentpos = filepos;
	}
}

//find label with this name
//returns label* if found (and scope/etc is correct), returns NULL if nothing found
//if name wasn't found, findindex points to where name would be inserted (name<labellist[findindex])
//if name was found but with wrong scope/whatever, findcmp=0.
//don't call if list is empty!
int findcmp;		//(these are used by newlabel)
int findindex;	  //.
label *findlabel(char *name) {
	int head,tail;
	label *p, *global;

	head=labelstart;
	tail=labelend;
	findindex=labelstart+labels/2;
	do {//assume list isn't empty
		findcmp=strcmp(name,(*(labellist[findindex])).name);
		if(findcmp<0) {
			tail=findindex-1;
			findindex-=(tail-head)/2+1;
		} else if(findcmp>0) {
			head=findindex+1;
			findindex+=(tail-head)/2+1;
		}
	} while(findcmp && (tail-head)>=0);
	if(findcmp) {
		if(findcmp<0)
			findindex++;//position findindex so the label it points to needs to shift right
		return 0;
	}
	p=labellist[findindex];

	//check scope: label only visible if p.scope=(scope or 0)
	global=0;
	if(*name=='+') {//forward labels need special treatment :P
		do {
			if((*p).pass!=pass) {
				if(!(*p).scope)
					global=p;
				if((*p).scope==scope)
					return p;
			}
			p=(*p).link;
		} while(p);
	} else {
		do {
			if(!(*p).scope)
				global=p;
			if((*p).scope==scope)
				return p;
			p=(*p).link;
		} while(p);
	}
	return global;  //return global label only if no locals were found
}

//double list capacity
void growlist(void) {
	label **tmp;
	int newhead;
	
	maxlabels<<=1;
	newhead=maxlabels/2-labels/2;
	tmp=(label**)my_malloc(maxlabels*sizeof(label*));
	memcpy(tmp+newhead,labellist+labelstart,labels*sizeof(label*));
	free(labellist);
	labellist=tmp;
	findindex=findindex-labelstart+newhead;
	labelstart=newhead;
	labelend=newhead+labels-1;
}

//make new empty label and add it to list using result from last findlabel
//ONLY use after calling findlabel
label *newlabel(void) {
	label **start,**end;
	label *p;

	p=(label*)my_malloc(sizeof(label));
	(*p).link=0;
	(*p).scope=0;
	(*p).name=0;

	if(!findcmp) {//new label with same name
		(*p).name=(*labellist[findindex]).name;//share old name
		//if(!scope) {//global always goes at the end
		//  (*lastfindlink).link=p;//add to the chain..
		//} else {//insert into the front
			(*p).link=labellist[findindex];
			labellist[findindex]=p;
		//}
		return p;
	}
	if(!labelstart || labelend>=maxlabels-1)//make sure there's room to add
		growlist();

	end=&labellist[findindex];
	if(findindex>(labelstart+labels/2)) {   //shift up
		start=&labellist[labelend];
		for(;start>=end;start--)
			*(start+1)=*start;
		labelend++;
	} else {				//shift down
		end--;
		start=&labellist[labelstart];
		for(;start<=end;start++)
			*(start-1)=*start;
		labelstart--;
	}
	*end=p;
	labels++;
	return p;
}

//==============================================================================================================

void showerror(char *errsrc,int errline) {
	error=1;
	fprintf(stderr,"%s(%i): %s\n",errsrc,errline,errmsg);
	
	if(!listerr)//only list the first error for this line
		listerr=errmsg;
}

//process the open file f
char fileline[LINEMAX];
void processfile(FILE *f, char* name) {
	static int nest=0;
	int nline=0;
	int eof;
	nest++;//count nested include()s
	do {
		nline++;
		eof=!fgets(fileline,LINEMAX,f);		 
		if(!eof)
			processline(fileline,name,nline);
	} while(!eof);
	nest--;
	nline--;
	if(!nest) {//if main source file (not included)
		errmsg=0;
		if(iflevel)
			errmsg=NoENDIF;
		if(reptcount)
			errmsg=NoENDR;
		if(makemacro)
			errmsg=NoENDM;
		if(nooutput)
			errmsg=NoENDE;
		// [freem addition]
		if(nonl)
			errmsg=NoENDINL;
		if(errmsg)
			showerror(name,nline);
	}
}

//process single line
//src=source line
//errsrc=source file name
//errline=source file line number
void processline(char *src,char *errsrc,int errline) {
	char line[LINEMAX];//expanded line
	char word[WORDMAX];
	char *s,*s2,*comment;
	char *endmac;
	label *p;

	errmsg=0;
	comment=expandline(line,src);
	if(!insidemacro || verboselisting)
		listline(line,comment);

	s=line;
	if(errmsg) {	//expandline error?
		showerror(errsrc,errline);
	} else do {
		if(makemacro) { //we're inside a macro definition
			p=getreserved(&s);
			errmsg=endmac=0;
			if(!p) {//skip over label if there is one, we're looking for "ENDM"
				endmac=s;
				p=getreserved(&s);
			}
			if(p) if((*p).value==(ptrdiff_t)endm) {
				comment=0;
				if(endmac) {
					endmac[0]='\n';
					endmac[1]=0;//hide "ENDM" in case of "label: ENDM"
				} else
					makemacro=0;//don't bother adding the last line
			}
			if(makemacro&&makemacro!=true_ptr) {
				if(comment)
					strcat(line,comment);	   //keep comment for listing
				*makemacro=my_malloc(strlen(line)+sizeof(char*)+1);
				makemacro=(char**)*makemacro;
				*makemacro=0;
				strcpy((char*)&makemacro[1],line);
			}
			if(p) if((*p).value==(ptrdiff_t)endm)
				makemacro=0;
			break;
		}//makemacro
		if(reptcount) {//REPT definition is in progress?
			p=getreserved(&s);
			errmsg=endmac=0;
			if(!p) {
				endmac=s;
				p=getreserved(&s);
			}
			if(p) {
				if((*p).value==(ptrdiff_t)rept) {
					++reptcount;//keep track of how many ENDR's are needed to finish
				} else if((*p).value==(ptrdiff_t)endr) {
					if(!(--reptcount)) {
						comment=0;
						if(endmac) {
							endmac[0]='\n';//hide "ENDR" in case of "label: ENDR"
							endmac[1]=0;
						}
					}
				}
			}
			if(reptcount || endmac) {   //add this line to REPT body
				if(comment)
					strcat(line,comment);	   //keep comment for listing
				*makerept=my_malloc(strlen(line)+sizeof(char*)+1);
				makerept=(char**)*makerept;
				*makerept=0;
				strcpy((char*)&makerept[1],line);
			}
			if(!reptcount) {//end of REPT, expand the whole thing right now
				expandrept(errline,errsrc);
			}
			break;
		}
		labelhere=0;	//for non-label symbol definitions (EQU,=,etc)
		s2=s;
		p=getreserved(&s);
		errmsg=0;
		if(skipline[iflevel]) {//conditional assembly.. no code generation
			if(!p) {	//it was a label... ignore it and move on
				p=getreserved(&s);
				if(!p) break;
			}
			if((*p).value!=(ptrdiff_t)_else && (*p).value!=(ptrdiff_t)elseif && (*p).value!=(ptrdiff_t)endif
			&& (*p).value!=(ptrdiff_t)_if && (*p).value!=(ptrdiff_t)ifdef && (*p).value!=(ptrdiff_t)ifndef)
				break;
		}
		if(!p) {//maybe a label?
			if(getlabel(word,&s2)) addlabel(word,insidemacro);
			if(errmsg) goto badlabel;//fucked up label
			p=getreserved(&s);
		}
		if(p) {
			if((*p).type==MACRO)
				expandmacro(p,&s,errline,errsrc);
			else
				// call assembler function for the given directive.
				((icfn)(*p).value)(p,&s);
		}
		if(!errmsg) {//check extra garbage
			s+=strspn(s,whitesp);
			if(*s)
				errmsg="Extra characters on line.";
		}
badlabel:
		if(errmsg) {
			showerror(errsrc,errline);
		}
	} while(0);
}

void showhelp(void) {
	puts("");
	puts("asm6f " VERSION " (+ freem modifications)\n");
	puts("Usage:  asm6f [-options] sourcefile [outputfile] [listfile]\n");
	puts("\t-?\t\tshow this help");
	puts("\t-l\t\tcreate listing");
	puts("\t-L\t\tcreate verbose listing (expand REPT, MACRO)");
	puts("\t-d<name>\tdefine symbol");
	puts("\t-q\t\tquiet mode (no output unless error)");
	// [additions from various sources (freem, nicklausw, Sour)]
	puts("\t-n\t\texport FCEUX-compatible .nl files");
	puts("\t-f\t\texport Lua symbol file");
	puts("\t-c\t\texport .cdl for use with FCEUX/Mesen");
	puts("\t-m\t\texport Mesen-compatible label file (.mlb)\n");
	puts("\t-i\t\tbuild .ips format patch file instead of binary.");
	puts("See README.TXT for more info.\n");
}

//--------------------------------------------------------------------------------------------

int main(int argc,char **argv) {
	int i,notoption;
	char* tryname;
	label *p;
	FILE *f;

	if(argc<2) {
		showhelp();
		return EXIT_FAILURE;
	}
	initlabels();
	initcomments();
	notoption=0;
	for(i=1;i<argc;i++) {
		if(*argv[i]=='-' || (*argv[i]=='/' && strlen(argv[i]) == 2)) {
			switch(argv[i][1]) {
				case 'h':
				case '?':
					showhelp();
					return EXIT_FAILURE;
				case 'L':
					verboselisting=1;
				case 'l':
					listfilename=true_ptr;
					break;
				case 'd':
					/* freem todo: allow support for defining the symbols;
					 * e.g. asm6 -dMMC=5 (etc.)
					 */
					if(argv[i][2]) {
						// freem todo: search for '=' character and split
						/*
						const char *arg2 = &argv[i][2];
						char *symbol, *equals, *value;
						equals=strchr(arg2,'=');
						if(equals!=NULL){
							int ePos = equals-arg2;
							printf("equals at %d\n",ePos);
							printf("%s\n",&arg2[ePos]);
							//strncpy(symbol,arg2,ePos);
							//symbol[ePos]=0;
							//printf("%s\n",symbol);
						}
						*/

						if(!findlabel(&argv[i][2])) {
							p=newlabel();
							(*p).name=my_strdup(&argv[i][2]);
							(*p).type=VALUE;
							(*p).value=1;
							(*p).line=true_ptr;
							(*p).pass=0;
						}
					}
					break;
				case 'q':
					verbose=0;
					break;
				// [freem addition]
				case 'n':
					genfceuxnl=1;
					break;
				case 'm':
					genmesenlabels=1;
					break;
				case 'c':
					gencdl = 1;
					break;
				case 'f':
					genlua=1;
					break;
				case 'i':
					genips=1;
					break;
				default:
					fatal_error("unknown option: %s",argv[i]);
			}
		} else {
			if(notoption==0)
				inputfilename=argv[i];
			else if(notoption==1)
				outputfilename=argv[i];
			else if(notoption==2)
				listfilename=argv[i];
			else
				fatal_error("unused argument: %s",argv[i]);
			notoption++;
		}
	}
	if(!inputfilename) 
		fatal_error("No source file specified.");
	
	if(!outputfilename) {
		outputfilename = replace_ext(inputfilename, ".bin");
	}
	
	if(genips) {
		ipsfilename = replace_ext(outputfilename, ".ips");
	}

	if(listfilename==true_ptr) {	//if listfile was wanted but no name was specified, use srcfile.LST
		listfilename = replace_ext(inputfilename, ".lst");
	}

	f=fopen(inputfilename,"rb");	//if srcfile won't open, try some default extensions
	if(!f) {
		tryname = replace_ext(inputfilename, ".asm");
		f=fopen(tryname,"rb");
		if(!f) {
			free(tryname);
			tryname = replace_ext(inputfilename, ".s");
			f=fopen(tryname,"rb");
		}
		if(f) inputfilename=tryname;
	}
	if(f) fclose(f);

	if(gencdl) {
		cdlfilename = replace_ext(inputfilename, ".cdl");
	}

	//main assembly loop:
	p=0;
	do {
		filepos=0;
		filesize=0;
		pass++;
		if(pass==MAXPASSES || (p==lastlabel))
			lastchance=1;//give up on too many tries or no progress made
		if(lastchance)
			message("last try..\n");
		else
			message("pass %i..\n",pass);
		needanotherpass=0;
		skipline[0]=0;
		scope=1;		
		nextscope=2;
		defaultfiller=DEFAULTFILLER;	//reset filler value
		addr=NOORIGIN;//undefine origin
		p=lastlabel;
		tryname=inputfilename;
		include(0,&tryname);		//start assembling srcfile
		if(errmsg)
		{
			//todo - shouldn't this set error?
			fputs(errmsg, stderr);//bad inputfile??
		}
	} while(!error && !lastchance && needanotherpass);//while no hard errors, not final try, and labels are still unresolved
	
	if(outputfile) {
		// Be sure last of output file is written properly
		int result;
		flush_output(1);
		
		result = fclose(outputfile);
		outputfile = NULL; // prevent fatal_error() from trying to close file again
		if ( result )
			fatal_error( CantWrite);
		
		if(!error) {
			message("%s written (%i bytes).\n",outputfilename,filesize);
		} else
			remove(outputfilename);
	} else if (!genips) {
		if(!error)
			fputs("nothing to do!", stderr);
		error = 1;
	}
	
	if (genips)
	{
		puts(ipsfilename);
		FILE* ipsfile = fopen(ipsfilename, "wb");
		if (!ipsfile)
		{
			errmsg = CantWrite;
		}
		else
		{
			ips_write(ipsfile);
			fclose(ipsfile);
		}
	} 
	
	if(listfile)
		listline(0,0);

	// [freem addition] only generate labelfiles if asked
	if(genfceuxnl)
		export_labelfiles();
	if(genlua)
		export_lua();
	if(genmesenlabels)
		export_mesenlabels();

	return error ? EXIT_FAILURE : 0;
}

// returns position of extension in the path, or
// end of string if no extension.
char* find_ext(char* in)
{
	char* strptr = strrchr(in, '.');
	
	// watch out for "dirname.ext\listfile"
	if (strptr) {
		if (strchr( strptr,'\\' ) || strchr(strptr, '/')) {
			strptr = 0;
		}
	}
	
	// no input extension; skip to end.
	if (!strptr) strptr = in + strlen(in);
	
	return strptr;
}

// duplicates the given input filename,
// replacing the extension with the provided ext.
// ext need not start with a '.'
char* replace_ext(char* in, char* ext)
{
	char* strptr;
	char* out = my_malloc(strlen(in) + strlen(ext) + 1);
	strcpy(out, in);
	
	// search for last '.'
	strptr = find_ext(out);
	
	strcpy(strptr, ext);
	
	return out;
}

#define LISTMAX 8//number of output bytes to show in listing
byte listbuff[LISTMAX];
int listcount;

ips_hunk* malloc_ips_hunk_regular(size_t offset, byte* p, size_t count)
{
	assert(count > 0);
	
	ips_hunk* hunk = (ips_hunk*)my_malloc(sizeof(ips_hunk));
	hunk->offset = offset;
	hunk->length = count;
	hunk->contents = (byte*)my_malloc(count);
	hunk->suppress = 0;
	hunk->next = 0;
	memcpy(hunk->contents, p, count);
	return hunk;
}

ips_hunk* malloc_ips_hunk_rle(size_t offset, byte b, size_t count)
{
	assert(count > 0);
	
	ips_hunk* hunk = (ips_hunk*)my_malloc(sizeof(ips_hunk));
	hunk->offset = offset;
	hunk->length = count;
	hunk->contents = 0;
	hunk->rle_content = b;
	hunk->suppress = 0;
	hunk->next = 0;
	return hunk;
}

void ips_append_hunk(ips_hunk* hunk)
{
	hunk->next = 0;
	
	if (ips_hunk_head == 0)
	{
		ips_hunk_head = hunk;
	}
	else
	{
		ips_hunk_tail->next = hunk;
	}
	
	ips_hunk_tail = hunk;
}

void write_ips_hunk_regular(FILE* f, size_t offset, byte *p, size_t count)
{
	assert(count <= 0xffff);
	byte header[5] = {
		(offset & 0xff0000) >> 16,
		(offset & 0xff00) >> 8,
		(offset & 0xff),
		(count & 0xff00) >> 8,
		count & 0xff
	};
	
	// write header
	if ( fwrite(header, 5, 1, f) < 1 )
	{
		errmsg=CantWrite;
		return;
	}
	
	// write payload
	if (fwrite(p,1,count,f) < count || fflush(f))
	{
		errmsg=CantWrite;
	}
}

void write_ips_hunk_rle(FILE* f, size_t offset, byte b, size_t count)
{
	assert(count <= 0xffff);
	byte header[8] = {
		(offset & 0xff0000) >> 16,
		(offset & 0xff00) >> 8,
		(offset & 0xff),
		0,
		0,
		(count & 0xff00) >> 8,
		count & 0xff,
		b
	};
	
	if (
		fwrite(header, 8, 1, f) < 1
		|| fflush(f)
	)
	{
		errmsg=CantWrite;
	}
}

void ips_free_hunk(ips_hunk* hunk)
{
	if (hunk->contents)
	{
		free(hunk->contents);
	}
	free(hunk);
}

int ips_changed = 0;

// combines/swaps adjacent/overlapping hunks.
// bubble sort ips entries.
// returns true if a change was made.
void ips_simplify_hunk(ips_hunk** hunk_ptr)
{
	ips_hunk* hunk;
	ips_hunk* next;
	
tail_recurse:
	hunk = *hunk_ptr;
	if (hunk == 0) return;
	next = hunk->next;
	
	// this node is not used.
	if (hunk->suppress || hunk->length == 0) goto remove_node;
	
	// update tail
	ips_hunk_tail = hunk;
	
	// reached end.
	if (next == 0) return;
	
	// don't merge with suppressed chunk
	if (next->suppress)
	{
		// continue
		hunk_ptr = &hunk->next;
		goto tail_recurse;
	};
	
	// totally contained in the next one?
	if (hunk->offset >= next->offset
		&& hunk->offset + hunk->length <= next->offset + next->length)
	{
		goto remove_node;
	}
	
	// the next one starts after this one
	if (next->offset > hunk->offset)
	{
		// but they overlap
		if (hunk->offset + hunk->length > next->offset)
		{
			// truncate this one
			ips_changed = 1;
			hunk->length = next->offset - hunk->offset;
		}
		else
		{
			// no change; proceed to the next one.
			hunk_ptr = &hunk->next;
			goto tail_recurse;
		}
	}
	// the next one is totally contained in this one?
	else if (next->offset >= hunk->offset
		&& next->offset + next->length <= hunk->offset + hunk->length)
	{
		ips_changed = 1;
		
		// duplicate us to appear after theirs
		if (next->offset + next->length < hunk->offset + hunk->length)
		{
			ips_hunk* newhunk = (ips_hunk*)my_malloc(sizeof(ips_hunk));
			newhunk->offset = next->offset + next->length;
			newhunk->length = hunk->offset + hunk->length - newhunk->offset;
			if (hunk->contents)
			{
				hunk->contents = (byte*)my_malloc(newhunk->length);
				memcpy(
					hunk->contents,
					hunk->contents + newhunk->offset - hunk->offset,
					newhunk->length
				);
			}
			else
			{
				newhunk->contents = 0;
				newhunk->rle_content = hunk->rle_content;
			}
			newhunk->next = next->next;
		}
		
		// truncate our length
		hunk->length = next->offset - hunk->offset;
	}
	// the next one starts before this one
	else if (next->offset < hunk->offset)
	{
		ips_changed = 1;
		
		if (next->offset + next->length > hunk->offset)
		{
			// not disjoint.
			// truncate ours from the left.
			
			int removed = next->offset + next->length - hunk->offset;
			
			hunk->length -= removed;
			if (hunk->contents)
			{
				memmove(hunk->contents, hunk->contents + removed, hunk->length);
			}
		}
		
		// swap
		*hunk_ptr = next;
		hunk->next = next->next;
		next->next = hunk;
		
		// proceed from the new location of this hunk.
		hunk_ptr = &next->next;
		goto tail_recurse;
	}
	
	if (0)
	{
	remove_node:
		ips_changed = 1;
		*hunk_ptr = next;
		ips_free_hunk(hunk);
	}
	
	// retry this node
	goto tail_recurse;
}

// writes all hunks to the output file.
void ips_write(FILE* output)
{
	assert(genips);
	
	// write IPS header.
	if ( fwrite("PATCH", 5, 1, output) < 1 )
	{
		errmsg=CantWrite;
		return;
	}
	
	// O(n^2)
	while (1) {
		ips_simplify_hunk(&ips_hunk_head);
		if (!ips_changed) break;
		ips_changed = 0;
	};
	
	// write hunks
	for (ips_hunk* hunk = ips_hunk_head; hunk != 0; hunk = hunk->next)
	{
		if (hunk->suppress) continue;
		if (hunk->contents)
		{
			write_ips_hunk_regular(output, hunk->offset, hunk->contents, hunk->length);
		}
		else
		{
			write_ips_hunk_rle(output, hunk->offset, hunk->rle_content, hunk->length);
		}
	}
	
	if ( fwrite("EOF", 3, 1, output) < 1 )
	{
		errmsg=CantWrite;
		return;
	}
}

void flush_output_ips()
{	
	// write IPS hunks.
	if (outcount == 0)
	{
		// do nothing.
	}
	else if (outcount <= 3)
	{
		// RLE is never a good choice for hunks this small.
		ips_append_hunk(malloc_ips_hunk_regular(ips_outpos, outputbuff, outcount));
	}
	else
	{
		// split into RLE and non-RLE hunks
		size_t hunk_start = 0;
		size_t rle_start = 0;
		byte rle_cmp = outputbuff[0];
		for (size_t i = 0; i <= outcount; ++i)
		{
			int b = (i == outcount)
				? -1
				: outputbuff[i];
			if (b != (int) rle_cmp)
			{
				if (i - rle_start >= IPS_RLE_EXTRACT)
				{
					if (rle_start > hunk_start)
					{
						ips_append_hunk(malloc_ips_hunk_regular(
							ips_outpos + hunk_start, outputbuff + hunk_start, rle_start - hunk_start
						));
					}
					ips_append_hunk(malloc_ips_hunk_rle(
						ips_outpos + rle_start, rle_cmp, i - rle_start
					));
					hunk_start = i;
				}
				else if (b == -1)
				{
					ips_append_hunk(malloc_ips_hunk_regular(
						ips_outpos + hunk_start, outputbuff + hunk_start, i - hunk_start
					));
					break;
				}
				
				rle_start = i;
				rle_cmp = b;
			}
		}
	}
	ips_outpos += outcount;
}

// removes all hunks from the list.
void ips_clear()
{
	// force output flush because
	// output buffer may be written to ips eventually.
	flush_output(1);
	
	// clear list.
	for (ips_hunk* hunk = ips_hunk_head; hunk != 0; )
	{
		if (hunk->contents)
		{
			free(hunk->contents);
		}
		ips_hunk* tmp = hunk;
		hunk = hunk->next;
		free(tmp);
	}
	
	ips_hunk_head = 0;
	ips_hunk_tail = 0;
}

// gets cmp value in ips file
int ips_get_cmp_value(size_t location)
{
	assert(genips);
	int value = -1;
	for (ips_hunk* hunk = ips_hunk_head; hunk != 0; hunk = hunk->next)
	{
		if (hunk->offset <= location && location < hunk->offset + hunk->length)
		{
			if (hunk->contents)
			{
				value = hunk->contents[location - hunk->offset];
			}
			else
			{
				value = hunk->rle_content;
			}
		}
	}
	return value;
}

// gets output value at the given position
// negative value if couldn't read it or not set
int get_cmp_value(size_t location)
{
	if (genips)
	{
		return ips_get_cmp_value(location);
	}
	else
	{
		// TODO optimize
		int b;
		size_t prevseek = ftell(outputfile);
		fseek(outputfile, location, SEEK_SET);
		if (fread(&b,1,1,outputfile) < 1)
		{
			b = -1;
		}
		fseek(outputfile, prevseek, SEEK_SET);
		return b;
	}
}

// flushes output buffer
void flush_output(int force)
{
	if(outcount>=BUFFSIZE || force || (genips && outcount >= 0xffff)) {
		
		// flush ips output
		if (genips) {
			flush_output_ips();
		}
		
		// flush binary output
		if (outputfile)
		{
			if(fwrite(outputbuff,1,outcount,outputfile)<outcount || fflush(outputfile))
				errmsg=CantWrite;
		}
			
		// clear buffer
		outcount=0;
	}
}

// directly adds bytes to output buffer.
void output_buffer(byte* p, size_t count)
{
	for (size_t i = 0; i < count; ++i)
	{
		// compare
		if (comparefiller)
		{
			int b = get_cmp_value(filepos + i);
			if (b != defaultfiller && b >= 0)
			{
				errmsg = CompFailed;
				int loc = filepos + outcount;
				sprintf(CompFailed + 26, "%0*x was 0x%x.", 6, loc, b);
				return;
			}
		}
		
		// write to outputbuffer
		outputbuff[outcount++]=*p;
		
		p++;
		
		flush_output(0);
		
		if (errmsg)
		{
			return;
		}
	}
}

// checks if we need to start a new file for outputting to.
void output_file()
{
	// pass number for previous pass.
	// note that the first pass is pass=1.
	static int oldpass=0;
	
	if (nooutput) return;
	
	// if we're generating a CDL file.
	if (gencdl) {
		if(oldpass != pass) {
			if(cdlfile) {
				fclose(cdlfile);
			}
			cdlfile = fopen(cdlfilename, "wb+");
		}
	}
	
	// when starting a new pass, reopen file and possibly insert iNES header.
	if(oldpass!=pass) {
		oldpass=pass;
		
		if (genips)
		{
			ips_clear();
			ips_outpos = 0;
		}
		
		// binary output.
		if(outputfile) fclose(outputfile);
		outputfile=fopen(outputfilename,"wb+");
		assert(filepos == 0);
		assert(filesize == 0);
		outcount=0;
		if(!outputfile) {
			errmsg=CantCreateFile;
			return;
		}

		// (insert iNES header if needed)
		if (ines_include) {
			byte ineshdr[HEADERSIZE] = {'N','E','S',0x1A,
								(byte)inesprg_num,
								(byte)ineschr_num,
								(byte)(inesmap_num << 4) | inesmir_num,
								(byte)(inesmap_num & 0xF0) | (use_nes2 << 3) | (nes2vs_num),
								(byte)(inesmap_num >> 8) | (nes2sub_num << 4),
								(byte)(inesprg_num >> 8) | ((ineschr_num >> 8) << 4),
								(byte)(nes2bram_num << 4) | nes2prg_num,
								(byte)(nes2chrbram_num << 4) | nes2chr_num,
								(byte)nes2tv_num,
								0,0,0};
			// extensions
			for (unsigned i = 0; i < HEADERSIZE; ++i)
			{
				ineshdr[i] &= ~ines_extension_mask[i];
				ineshdr[i] |= ines_extension_mask[i] & ines_extension[i];
			}
			
			// write header
			output_buffer(ineshdr, HEADERSIZE);
			filepos = sizeof(ineshdr);
			filesize = sizeof(ineshdr);
		}
	}
}

// writes `size` bytes from `p` to output file.
// cdlflag is used when generating cdlfiles.
// It should be one of the cdl types (NONE, DATA, or CODE).
void output(byte *p,int size, int cdlflag) {
	
	// ensure we have a file that we're outputting to.
	output_file();
	
	// update cdl file
	if(gencdl && !nooutput) {
		if(cdlfile && (!ines_include || filepos >= HEADERSIZE)) {
			int repeat = size;
			while(repeat--) {
				if(addr < 0x10000) {
					//PRG, mark as either code or data
					byte flag = (byte)cdlflag;
					fwrite((void*)&flag, 1, 1, cdlfile);
				} else {
					//CHR data
					fwrite("\x0", 1, 1, cdlfile);
				}
			}
		}
	}
	
	// advance the memory address.
	addr+=size;
	
	if (nooutput)
		return;
	
	if(!outputfile && !genips) return;
	
	// write data.
	output_buffer(p, size);
	
	while(size--) {
		if(listfile && listcount<LISTMAX)
			listbuff[listcount]=*p;
		listcount++;
		p++;
		
		// update filepos and filesize
		filepos++;
		if (filepos > filesize) filesize = filepos;
	}
}

/* Outputs integer as little-endian. See readme.txt for proper usage. */
static void output_le( int n, int size, int cdlflag )
{
	byte b [2];
	b [0] = n;
	b [1] = n >> 8;
	output( b, size, cdlflag);
}

// seeks to the given location for outputting.
void output_seek(int pos)
{
	byte padbyte = defaultfiller;
	int prevaddr = addr;
	
	if (nooutput)
	{
		errmsg = CantSeekEnum;
		return;
	}
		
	// ensure output file exists.
	output_file();
	
	flush_output(1);
	
	// seek in cdlfile.
	if (gencdl)
	{
		if (cdlfile)
		{
			int cdlpos = (pos < filesize)
				? pos
				: filesize;
			// cdl file is headerless
			if (ines_include) cdlpos -= HEADERSIZE;
			// clamp
			if (cdlpos < 0) cdlpos = 0;
			if (fflush(cdlfile))
			{
				errmsg = CantWrite;
				return;
			}
			fseek(cdlfile, cdlpos, SEEK_SET);
			if (ftell(cdlfile) != cdlpos)
			{
				errmsg = CantSeek;
				return;
			}
		}
	}
	
	// FIXME: we don't actually need a file in order to set the next output position.
	// Additionally, there is no need to pad until a write actually occurs.
	
	if (outputfile)
	{
		// seek in outputfile.
		if (pos > filesize)
		{
			// past end of file -- pad with zeros
			filepos = filesize;
			ips_outpos = filepos;
			fseek(outputfile, filepos, SEEK_SET);
			while (filepos < pos)
			{
				output(&padbyte, 1, NONE);
			}
		}
		else if (pos < filesize)
		{
			// within file bounds -- seek there.
			filepos = pos;
			fseek(outputfile, filepos, SEEK_SET);
		}
	
		if (filepos != ftell(outputfile))
		{
			errmsg = CantSeek;
		}
	} 
	
	filepos = pos;
	ips_outpos = pos;
	
	// ensures the above calls to output (for padding) won't modify addr.
	addr = prevaddr;
}

//end listing when src=0
char srcbuff[LINEMAX];
void listline(char *src,char *comment) {
	static int oldpass=0;
	int i;
	if(!listfilename)
		return;
	if(oldpass!=pass) {//new pass = new listfile
		oldpass=pass;
		if(listfile) fclose(listfile);
		listfile=fopen(listfilename,"w");
		if(!listfile) {
			listfilename=0;//stop trying
			// todo - if user wants a listing, this SHOULD be an error, otherwise
			// he might still have old listing and think it's the current one.
			// For example, he might have had it open in a text editor, preventing its
			// creation here.
			fputs("Can't create list file.", stderr);//not critical, just give a warning
			return;
		}
	} else {//finish previous line
		for(i=0;i<listcount && i<LISTMAX;i++)
			fprintf(listfile," %02X",(int)listbuff[i]);
		for(;i<LISTMAX;i++)
			fprintf(listfile,"   ");
		fputs(listcount>LISTMAX?".. ":"   ",listfile);
		fputs(srcbuff,listfile);
		if(listerr) {
			fprintf(listfile,"*** %s\n",listerr);
			listerr=0;
		}
	}
	listcount=0;
	if(src) {
		if(addr<0)
			fprintf(listfile,"	 ");
		else
			fprintf(listfile,"%05X",(int)addr);
		strcpy(srcbuff,src);//make a copy of the original source line
		if(comment) {
			strcat(srcbuff, comment);
			if(genmesenlabels && filepos > 0 && addr < 0x10000 && addr >= 0) {
				//save this comment - needed for export
				addcomment(comment);
			}
		}
	} else {
		fclose(listfile);
		message("%s written.\n",listfilename);
	}
}
//------------------------------------------------------
//directive(label *id, char **next)
//
//  id=reserved word
//  **next=source line (ptr gets moved past directive on exit)
//------------------------------------------------------
void equ(label *id, char **next) {
	char str[LINEMAX];
	char *s=*next;
	if(!labelhere)
		errmsg=NeedName;//EQU without a name
	else {
		if((*labelhere).type==LABEL) {//new EQU.. good
			reverse(str,s+strspn(s,whitesp));	   //eat whitesp off both ends
			reverse(s,str+strspn(str,whitesp));
			if(*s) {
				(*labelhere).line=my_strdup(s);
				(*labelhere).type=EQUATE;
			} else {
				errmsg=IncompleteExp;
			}
		} else if((*labelhere).type!=EQUATE) {
			errmsg=LabelDefined;
		}
		*s=0;//end line
	}
}

void equal(label *id,char **next) {
	if(!labelhere)			  //labelhere=index+1
		errmsg=NeedName;		//(=) without a name
	else {
		(*labelhere).type=VALUE;
		dependant=0;
		(*labelhere).value=eval(next,WHOLEEXP);
		(*labelhere).line=ptr_from_bool(!dependant);
	}
}

void base(label *id, char **next) {	
	int val;
	dependant=0;
	val=eval(next,WHOLEEXP);
	if(!dependant && !errmsg)
		addr=val;
	else
		addr=NOORIGIN;//undefine origin
}

void seekabs(label *id, char **next) {	
	int val;
	dependant=0;
	val=eval(next,WHOLEEXP);
	if(!dependant && !errmsg)
	{
		output_seek(val);
	}
}

void seekrel(label *id, char **next) {	
	int val;
	dependant=0;
	val=eval(next,WHOLEEXP);
	if(!dependant && !errmsg)
	{
		output_seek(filepos + val);
	}
}

void skiprel(label *id, char **next) {	
	int val;
	dependant=0;
	val=eval(next,WHOLEEXP);
	if(!dependant && !errmsg)
	{
		output_seek(filepos + val);
		addr += val;
	}
	else
	{
		// undefine origin -- we don't know where we are anymore.
		addr=NOORIGIN;
	}
}

//nothing to do (empty line)
void nothing(label *id, char **next) {
}

void include(label *id,char **next) {
	char *np;
	FILE *f;

	np=*next;
	reverse(tmpstr,np+strspn(np,whitesp2));	 //eat whitesp off both ends
	reverse(np,tmpstr+strspn(tmpstr,whitesp2));
	f=fopen(np,"r+");   //read as text, the + makes recursion not possible
	if(!f) {
		errmsg=CantOpen;
		error=1;
	} else {
		processfile(f,np);
		fclose(f);
		errmsg=0;//let main() know file was ok
	}
	*next=np+strlen(np);//need to play safe because this could be the main srcfile
}

void incbin(label *id,char **next) {
	int filesize, seekpos, bytesleft, i;
	FILE *f=0;

	do {
	//file open:
		getfilename(tmpstr,next);
		if(!(f=fopen(tmpstr,"rb"))) {
			errmsg=CantOpen;
			break;
		}
		fseek(f,0,SEEK_END);
		filesize=ftell(f);
	//file seek:
		seekpos=0;
		if(eatchar(next,','))
			seekpos=eval(next,WHOLEEXP);
		if(!errmsg && !dependant) if(seekpos<0 || seekpos>filesize)
			errmsg=SeekOutOfRange;
		if(errmsg) break;
		fseek(f,seekpos,SEEK_SET);
	//get size:
		if(eatchar(next,',')) {
			bytesleft=eval(next,WHOLEEXP);
			if(!errmsg && !dependant) if(bytesleft<0 || bytesleft>(filesize-seekpos))
				errmsg=BadIncbinSize;
			if(errmsg) break;
		} else {
			bytesleft=filesize-seekpos;
		}
	//read file:
		while(bytesleft) {
			if(bytesleft>BUFFSIZE) i=BUFFSIZE;
			else i=bytesleft;
			fread(inputbuff,1,i,f);
			output(inputbuff,i,DATA);
			bytesleft-=i;
		}
	} while(0);
	if(f) fclose(f);
}

void incnes(label *id, char **next) {
	char filename[WORDMAX];
	char buf[WORDMAX + 2];
	int filesize, seekpos, bytesleft, i;
	int cdlbytesleft, cdli, cdlstart, cdlflag;
	char cdlbuf[STACKBUFFSIZE];
	FILE *f=0;
	FILE *cdl=0;
	
	// get string-wrapped filename.
	buf[0] = '"';
	getfilename(filename, next);
	strcpy(buf + 1, filename);
	strcpy(buf + strlen(buf), "\"");
	
	char* s = buf;
	incines(id, &s);

	// include binary
	do {
	//file open:
		if(!(f=fopen(filename,"rb"))) {
			errmsg=CantOpen;
			break;
		}
		fseek(f,0,SEEK_END);
		filesize=ftell(f);
		if (filesize < HEADERSIZE)
		{
			errmsg = SeekOutOfRange;
			break;
		}
	//check for .cdl file (optional)
		if (gencdl)
		{
			s = replace_ext(filename, ".cdl");
			if ((cdl = fopen(s, "rb")))
			{
				fseek(cdl, 0, SEEK_END);
				cdlbytesleft = ftell(cdl);
				fseek(cdl, 0, SEEK_SET);
				if (cdlbytesleft == 0)
				{
					fclose(cdl);
					cdl = 0;
				}
			}
			free(s);
		}
		
	//file seek:
		seekpos=HEADERSIZE;
		fseek(f,HEADERSIZE,SEEK_SET);
	//get size:
		bytesleft=filesize-seekpos;
	//read file:
		while(bytesleft) {
			// clamp to buffer
			i = bytesleft;
			if (i>BUFFSIZE) i=BUFFSIZE;
			if (cdl && i > cdlbytesleft) i = cdlbytesleft;
			if (cdl && i > STACKBUFFSIZE) i = STACKBUFFSIZE;
			fread(inputbuff,1,i,f);
			if (cdl)
			{
				fread(cdlbuf, 1, i, cdl);
				cdlbytesleft -= i;
				if (!cdlbytesleft)
				{
					fclose(cdl);
					cdl = 0;
				}
				
				// output in sections of identical cdl.
				
				cdli = 0;
				while(cdli < i)
				{
					cdlstart = cdli;
					cdlflag = cdlbuf[cdli++];
					for (; cdli < i && cdlbuf[cdli] == cdlflag; ++cdli);
					output(inputbuff + cdlstart, cdli - cdlstart, cdlflag);
				}
			}
			else
			{
				output(inputbuff,i,NONE);
			}
			bytesleft-=i;
		}
	} while(0);
	if(f) fclose(f);
	if (cdl) fclose(cdl);
}

void clearpatch(label *id, char **next)
{	
	if (genips)
	{
		flush_output(1);
		
		// mark sections as non-output.
		// we don't remove them because we may wish to
		// read from them (e.g. compare)
		for (ips_hunk* hunk = ips_hunk_head; hunk != 0; hunk = hunk->next)
		{
			hunk->suppress = 1;
		}
	}
}

void hex(label *id,char **next) {
	char buff[LINEMAX];
	char *src;
	int dst;
	char c1,c2;
	getword(buff,next,0);
	if(!*buff) errmsg=MissingOperand;
	else do {
		src=buff;
		dst=0;
		do {
			c1=hexify(*src);
			src++;
			if(*src) {
				c2=hexify(*src);
				src++;
			} else {//deal with odd number of chars
				c2=c1;
				c1=0;
			}
			buff[dst++]=(c1<<4)+c2;
		} while(*src);
		output((byte*)buff,dst,DATA);
		getword(buff,next,0);
	} while(*buff);
}

void dw(label *id, char **next) {
	int val;
	do {
		val=eval(next,WHOLEEXP);
		if(!errmsg) {
			if(val>65535 || val<-65536)
				errmsg=OutOfRange;
			else
				output_le(val,2,DATA);
		}
	} while(!errmsg && eatchar(next,','));
}

void dl(label *id, char **next) {
	byte val;
	do {
		val=eval(next,WHOLEEXP) & 0xff;
		if(!errmsg)
			output(&val,1,DATA);
	} while(!errmsg && eatchar(next,','));
}

void dh(label *id, char **next) {
	byte val;
	do {
		val=eval(next,WHOLEEXP)>>8;
		if(!errmsg)
			output(&val,1,DATA);
	} while(!errmsg && eatchar(next,','));
}

void db(label *id,char **next) {
	int val,val2;
	byte *s,*start;
	char c,quote;

	do {
		*next+=strspn(*next,whitesp);	   //eatwhitespace
		quote=**next;
		if(quote=='"' || quote=='\'') { //string
			s=start=(byte*)*next+1;
			do {
				c=*s;
				s++;
				if(!c) errmsg=IncompleteExp;
				if(c=='\\') s++;
			} while(!errmsg && c!=quote);
			if(errmsg) continue;
			s--;	//point to the "
			*s='0';
			*next=(char*)s;
			val2=eval(next,WHOLEEXP);
			if(errmsg) continue;
			while(start!=s) {
				if(*start=='\\')
					start++;
				val=*start+val2;
				start++;
				output_le(val,1,DATA);
			}
		} else {
			val=eval(next,WHOLEEXP);
			if(!errmsg) {
				if(val>255 || val<-128)
					errmsg=OutOfRange;
				else
					output_le(val,1,DATA);
			}
		}
	} while(!errmsg && eatchar(next,','));
}

void dsw(label *id,char **next) {
	int count,val=defaultfiller;
	dependant=0;
	count=eval(next,WHOLEEXP);
	if(dependant || (count<0 && needanotherpass))//unknown count! don't do anything
		count=0;
	if(eatchar(next,','))
		val=eval(next,WHOLEEXP);
	if(!errmsg && !dependant) if(val>65535 || val<-32768 || count<0)
		errmsg=OutOfRange;
	if(errmsg) return;
	while(count--)
		 output_le(val,2,DATA);
}

void filler(int count,char **next) {
	int val=defaultfiller;
	if(dependant || (count<0 && needanotherpass)) //unknown count! don't do anything
		count=0;
	if(eatchar(next,','))
		val=eval(next,WHOLEEXP);
	if(!errmsg && !dependant) if(val>255 || val<-128 || count<0 || count>0x100000)
		errmsg=OutOfRange;
	if(errmsg) return;
	while(count--)//!#@$
		 output_le(val,1,NONE);
}

void dsb(label *id,char **next) {
	int count;
	dependant=0;
	count=eval(next,WHOLEEXP);
	filler(count,next);
}

void align(label *id,char **next) {
	int count;
	dependant=0;
	count=eval(next,WHOLEEXP);
	if(count>=0) {
		if((unsigned int)addr%count) count-=(unsigned int)addr%count;
		else count=0;
	} else count=0;
	filler(count,next);
}

void pad(label *id, char **next) {
	int count;
	if(addr<0) {
		errmsg=undefinedPC;
	} else {
		dependant=0;
		count=eval(next,WHOLEEXP)-addr;
		filler(count,next);
	}
}

void org(label *id, char **next) {
	if(addr<0) base(id,next);   //this is the first ORG; PC hasn't been set yet
	else pad(id,next);
}

void opcode(label *id, char **next) {
	char *s,*s2;
	int type,val = 0;
	byte *op;
	int oldstate=needanotherpass;
	int forceRel = 0;
    
	int uns;
	if (!allowunstable) {
		for(uns=0;uns<4;uns++) {
			if (!strcmp((*id).name, unstablelist[uns])) {
				fatal_error("Unstable instruction \"%s\" used without calling UNSTABLE.",(*id).name);
			}
		}
	}

	if (!allowhunstable) {
		if (!strcmp((*id).name, "XAA")) {
			fatal_error("Highly unstable instruction \"%s\" used without calling HUNSTABLE.",(*id).name);
		}
	}
		
	for(op=(byte*)(*id).line;*op!=0xff;op+=2) {//loop through all addressing modes for this instruction
		needanotherpass=oldstate;
		strcpy(tmpstr,*next);
		dependant=0;
		errmsg=0;
		type=op[1];
		s=tmpstr;
		if(type!=IMP && type!=ACC) {//get operand
			if(!eatchar(&s,ophead[type])) continue;
			val=eval(&s,WHOLEEXP);
			if(type==REL) {
				if(!dependant) {
					val-=addr+2;
					if(val>127 || val<-128) {
						needanotherpass=1;//give labels time to sort themselves out..
						if(lastchance)
						{
							errmsg="Branch out of range.";
							forceRel = 1;
						}
					}
				}
			} else {
				if(opsize[type]==1) {
					if(!dependant) {
						if(val>255 || val<-128)
							errmsg=OutOfRange;
					} else {
						if(type!=IMM)
							continue;//default to non-ZP instruction
					}
				} else {//opsize[type]==2
					if((val<0 || val>0xffff) && !dependant)
						errmsg=OutOfRange;
				}
			}
			if(errmsg && !dependant && !forceRel) continue;
		}

		my_strupr(s);
		s2=optail[type];
		while(*s2) {		//opcode tail should match input:
			if(!eatchar(&s,*s2))
				break;
			s2++;
		}
		s+=strspn(s,whitesp);
		if(*s || *s2) continue;

		if(addr>0xffff)
			errmsg="PC out of range.";
		output(op,1,CODE);
		output_le(val,opsize[type],CODE);
		*next+=s-tmpstr;
		return;
	}
	if(!errmsg)
		errmsg=Illegal;
}

void _if(label *id,char **next) {
	int val;
	if(iflevel>=IFNESTS-1)
		errmsg=IfNestLimit;
	else
		iflevel++;
	dependant=0;
	val=eval(next,WHOLEEXP);
	if(dependant || errmsg) {//don't process yet
		ifdone[iflevel]=1;
		skipline[iflevel]=1;
	} else {
		skipline[iflevel]=!val || skipline[iflevel-1];
		ifdone[iflevel]=!skipline[iflevel];
	}
}

void ifdef(label *id,char **next) {
	char s[WORDMAX];
	if(iflevel>=IFNESTS-1)
		errmsg=IfNestLimit;
	else
		iflevel++;
	getlabel(s,next);
	skipline[iflevel]=!(ptrdiff_t)findlabel(s) || skipline[iflevel-1];
	ifdone[iflevel]=!skipline[iflevel];
}

void ifndef(label *id,char **next) {
	char s[WORDMAX];
	if(iflevel>=IFNESTS-1)
		errmsg=IfNestLimit;
	else
		iflevel++;
	getlabel(s,next);
	skipline[iflevel]=(ptrdiff_t)findlabel(s) || skipline[iflevel-1];
	ifdone[iflevel]=!skipline[iflevel];
}

void elseif(label *id,char **next) {
	int val;
	if(iflevel) {
		dependant=0;
		val=eval(next,WHOLEEXP);
		if(!ifdone[iflevel]) {//no previous true statements
			if(dependant || errmsg) {//don't process yet
				ifdone[iflevel]=1;
				skipline[iflevel]=1;
			} else {
				skipline[iflevel]=!val || skipline[iflevel-1];
				ifdone[iflevel]=!skipline[iflevel];
			}
		} else {
			skipline[iflevel]=1;
		}
	} else
		errmsg="ELSEIF without IF.";
}

void _else(label *id,char **next) {
	if(iflevel)
		skipline[iflevel]=ifdone[iflevel] || skipline[iflevel-1];
	else
		errmsg="ELSE without IF.";
}

void endif(label *id,char **next) {
	if(iflevel) --iflevel;
	else errmsg="ENDIF without IF.";
}

void endm(label *id, char **next) {//ENDM is handled during macro definition (see processline)
	errmsg=ExtraENDM;
}
void endr(label *id, char **next) {//ENDR is handled during macro definition (see processline)
	errmsg=ExtraENDR;
}

void macro(label *id, char **next) {
	char *src;
	char word[WORDMAX];
	int params;
	
	labelhere=0;
	if(getlabel(word,next))
		addlabel(word,0);
	else
		errmsg=NeedName;

	makemacro=true_ptr;//flag for processline to skip to ENDM
	if(errmsg) {//no valid macro name
		return;
	} else if((*labelhere).type==LABEL) {//new macro
		(*labelhere).type=MACRO;
		(*labelhere).line=0;
		makemacro=&(*labelhere).line;
										//build param list
		params=0;
		src=*next;
		while(getlabel(word,&src)) {//don't affect **next directly, make sure it's a good name first
			*next=src;
			*makemacro=my_malloc(strlen(word)+sizeof(char*)+1);
			makemacro=(char**)*makemacro;
			strcpy((char*)&makemacro[1],word);
			++params;
			eatchar(&src,',');
		}
		errmsg=0;//remove getlabel's errmsg
		(*labelhere).value=params;//set param count
		*makemacro=0;
	} else if((*labelhere).type!=MACRO) {
		errmsg=LabelDefined;
	} else {//macro was defined on a previous pass.. skip past params
		**next=0;
	}
}

//errline=source file line number
//errsrc=source file name
void expandmacro(label *id,char **next,int errline,char *errsrc) {
	//char argname[8];
	char macroerr[WORDMAX*2];//this should be enough, i hope..
	char **line;
	int linecount=0;
	int oldscope;
	int arg, args;
	char c,c2,*s,*s2,*s3;
	
	if((*id).used) {
		errmsg=RecurseMACRO;
		return;
	}

	oldscope=scope;//watch those nested macros..
	scope=nextscope++;
	insidemacro++;
	(*id).used=1;
	sprintf(macroerr,"%s(%i):%s",errsrc,errline,(*id).name);
	line=(char**)((*id).line);

	//define macro params
	s=*next;
	args=(*id).value;   //(named args)
	arg=0;
	do {
		s+=strspn(s,whitesp);//eatwhitespace	s=param start
		s2=s; //s2=param end
		s3=strpbrk(s2,",'\""); //stop at param end or string definition
		if(!s3) s3=strchr(s2,0);
		c=*s3;
		if(c=='"' || c=='\'') {//go to end of string
			s3++;
			do {
				c2=*s3;
				s3++;
				if(c2=='\\') s3++;
			} while(c2 && c2!=c);
			if(!c2) s3--;//oops..too far
			c=*s3;
		}
		s2=s3;
		*s2=0;		  
		if(*s) {//arg not empty
		//  sprintf(argname,"\\%i",arg);		//make indexed arg
		//  addlabel(argname,1);
		//  equ(0,&s);
			if(arg<args) {			  //make named arg
				addlabel((char*)&line[1],1);
				equ(0,&s);
				line=(char**)*line; //next arg name
			}
			arg++;
		}
		*s2=c;
		s=s2;
	} while(eatchar(&s,','));
	*next=s;

	//make "\?" arg
	//{..}

	while(arg++ < args) //skip any unused arg names
		line=(char**)*line;

	while(line) {
		linecount++;
		processline((char*)&line[1],macroerr,linecount);		
		line=(char**)*line;
	}
	errmsg=0;
	scope=oldscope;
	insidemacro--;
	(*id).used=0;
}

int rept_loops;
char *repttext;//rept chain begins here
void rept(label *id, char **next) {
	dependant=0;
	rept_loops=eval(next,WHOLEEXP);
	if(dependant || errmsg || rept_loops<0)
		rept_loops=0;
	makerept=&repttext;
	repttext=0;
	reptcount++;//tell processline to start storing up rept lines
}

void expandrept(int errline,char *errsrc) {
	char macroerr[WORDMAX*2];//source to show in listing (this should be enough, i hope?)
	char **start,**line;
	int linecount;
	int i,oldscope;

	start=(char**)repttext;//first rept data
	oldscope=scope;
	insidemacro++;
	for(i=rept_loops;i;--i) {
		linecount=0;
		scope=nextscope++;
		sprintf(macroerr,"%s(%i):REPT",errsrc,errline);
		line=start;
		while(line) {
			linecount++;
			processline((char*)&line[1],macroerr,linecount);
			line=(char**)*line;
		}
	}
	while(start) {//delete everything
		line=(char**)*start;
		free(start);
		start=line;
	}
	errmsg=0;
	scope=oldscope;
	insidemacro--;
}

int enum_saveaddr;
void _enum(label *id, char **next) {
	int val=0;
	dependant=0;
	val=eval(next,WHOLEEXP);
//  if(!dependant && !errmsg) {
//  }
	if(!nooutput)
		enum_saveaddr=addr;
	addr=val;
	nooutput=1;
}

void ende(label *id, char **next) {
	if(nooutput) {
		addr=enum_saveaddr;
		nooutput=0;
	} else {
		errmsg=ExtraENDE;
	}
}

// [freem addition]
void ignorenl(label *id, char **next) {
	nonl=1;
}

// [freem addition]
void endinl(label *id, char **next) {
	if(nonl){
		nonl=0;
	} else {
		errmsg=ExtraENDINL;
	}
}

void fillval(label *id,char **next) {
	dependant=0;
	defaultfiller=eval(next,WHOLEEXP);
}

void compfill(label *id,char **next) {
	comparefiller=1;
}

void endcompfill(label *id,char **next) {
	comparefiller=0;
}

void make_error(label *id,char **next) {
	char *s=*next;
	reverse(tmpstr,s+strspn(s,whitesp2));	   //eat whitesp, quotes off both ends
	reverse(s,tmpstr+strspn(tmpstr,whitesp2));
	errmsg=s;
	error=1;
	*next=s+strlen(s);
}

void unstable(label *id, char **next) {
	allowunstable++;
}

void hunstable(label *id, char **next) {
	allowunstable++;
	allowhunstable++;
}

//[nicklausw] ines stuff

void inesprg(label *id, char **next) {
	inesprg_num=eval(next, WHOLEEXP);

	if(inesprg_num < 0 || inesprg_num > 0xFF)
		errmsg=OutOfRange;
	
	ines_include = 1;
}

void ineschr(label *id, char **next) {
	ineschr_num=eval(next, WHOLEEXP);

	if(ineschr_num < 0 || ineschr_num > 0xFF)
		errmsg=OutOfRange;
	
	ines_include = 1;
}

void inesmir(label *id, char **next) {
	inesmir_num=eval(next, WHOLEEXP);

	//force 4 bits
	if(inesmir_num > 16 || inesmir_num < 0)
		errmsg=OutOfRange;
	
	ines_include = 1;
}

void inesmap(label *id, char **next) {
	inesmap_num=eval(next, WHOLEEXP);

	//ines 2.0 allows for some big numbers...
	if(inesmap_num > 0xfff || inesmap_num < 0)
		errmsg=OutOfRange;
	
	ines_include = 1;
	if (inesmap_num > 0xff)
	{
		ines_extension_mask[8] &= ~0x0f;
	}
}

void incines(label *id,char **next) {
	int filesize;
	FILE *f=0;
	
	char header[ HEADERSIZE ];
	int parse = 0;

	do {
	//file open:
		getfilename(tmpstr,next);
		if(!(f=fopen(tmpstr,"rb"))) {
			errmsg=CantOpen;
			break;
		}
		fseek(f,0,SEEK_END);
		filesize=ftell(f);
		if (filesize < sizeof(header)) {
			errmsg = InvalidHeader;
			break;
		}
	//file seek:
		fseek(f, 0,SEEK_SET);
	// file read:
		fread(header, sizeof(header), 1, f);
		parse = 1;
	} while(0);
	if (f) fclose(f);
	
	// parse the header that was just read.
	if (parse) {
		ines_include = 1;
		
		// validate header
		if (header[0] != 'N' || header[1] != 'E' || header[2] != 'S' || header[3] != 0x1A) {
			errmsg = InvalidHeader;
			return;
		}
		
		// ines data
		inesprg_num = header[4];
		inesprg_num |= (header[9] & 0x0f) << 8;
		
		ineschr_num = header[5];
		ineschr_num |= (header[9] & 0xf0) << 4;
		
		inesmir_num = (header[6] & 0x0f);
		
		inesmap_num = 0;
		inesmap_num |= (header[6] & 0xf0) >> 4;
		inesmap_num |= (header[7] & 0xf0);
		inesmap_num |= (header[8] & 0x0f) << 4;		
		
		// nes2
		use_nes2 = (header[7] & 0x0c) == 0x08;
		if (use_nes2)
		{
			nes2vs_num = header[7] & 0x01;
			nes2sub_num = (header[8] & 0xf0) >> 4;
			nes2bram_num = (header[10] & 0xf0) >> 4;
			nes2prg_num = (header[10] & 0x0f);
			nes2chrbram_num = (header[11] & 0xf0) >> 4;
			nes2chr_num = (header[11] & 0x0f);
			nes2tv_num = header[12] & 0x03;
		}
		
		// there are some bits in the header that we don't
		// understand, so to ensure these parts of the header are preserved,
		// we store them as unrecognized extension data.
		byte mask[] = {
			0   , 0   , 0   , 0   ,
			0   , 0   , 0   , 0x06,
			0   , 0   , 0   , 0   ,
			0xff, 0xff, 0xff, 0xff   
		};
		if (!use_nes2)
		{
			// iNES format only strictly defines up to byte 7.
			memset(mask + 0x8, 0xff, HEADERSIZE - 0x8);
		}
		memcpy(ines_extension, header, HEADERSIZE);
		memcpy(ines_extension_mask, mask, HEADERSIZE);
	}
}

void nes2chrram(label *id, char **next) {
	nes2chr_num=eval(next, WHOLEEXP);

	if (nes2chr_num < 0 || nes2chr_num > 16)
		errmsg=OutOfRange;
	
	ines_include = 1; use_nes2 = 1;
	ines_extension_mask[7] &= ~0x0c;
	ines_extension_mask[11] &= ~0x0f;
}

void nes2prgram(label *id, char **next) {
	nes2prg_num=eval(next, WHOLEEXP);

	if (nes2prg_num < 0 || nes2prg_num > 16)
		errmsg=OutOfRange;
	
	ines_include = 1; use_nes2 = 1;
	ines_extension_mask[7] &= ~0x0c;
	ines_extension_mask[10] &= ~0x0f;
}

void nes2sub(label *id, char **next) {
	nes2sub_num=eval(next, WHOLEEXP);

	if (nes2sub_num < 0 || nes2sub_num > 16)
		errmsg=OutOfRange;
	
	ines_include = 1; use_nes2 = 1;
	ines_extension_mask[7] &= ~0x0c;
	ines_extension_mask[8] &= ~0xf0;
}

void nes2tv(label *id, char **next) {
	nes2tv_num=eval(next, WHOLEEXP);

	// possible presets...
	if(nes2tv_num == 'N') nes2tv_num = 0;
	if (nes2tv_num == 'P') nes2tv_num = 1;

	// might just change to 'N', 'P' but eh...
	if (nes2tv_num == 'B') nes2tv_num = 2;

	if(nes2tv_num > 2 || nes2tv_num < 0)
		errmsg=OutOfRange;
	
	ines_include = 1; use_nes2 = 1;
	ines_extension_mask[7] &= ~0x0c;
	ines_extension_mask[12] &= ~0x03;
}

void nes2vs(label *id, char **next) {
	nes2vs_num = 1;
	ines_include = 1; use_nes2 = 1;
	ines_extension_mask[7] &= ~0x0d;
}

void nes2bram(label *id, char **next) { 
	nes2bram_num=eval(next, WHOLEEXP);

	if (nes2bram_num < 0 || nes2bram_num > 16)
		errmsg=OutOfRange;
	
	ines_include = 1; use_nes2 = 1;
	ines_extension_mask[7] &= ~0x0c;
	ines_extension_mask[10] &= ~0xf0;
}

void nes2chrbram(label *id, char **next) {
	nes2chrbram_num=eval(next, WHOLEEXP);

	if (nes2chrbram_num < 0 || nes2chrbram_num > 16)
		errmsg=OutOfRange;
	
	ines_include = 1; use_nes2 = 1;
	ines_extension_mask[7] &= ~0x0c;
	ines_extension_mask[11] &= ~0xf0;
}