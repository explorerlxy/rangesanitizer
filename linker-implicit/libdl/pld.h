#ifndef __PLD_H
#define __PLD_H

// addr space range below bit 41 usable for untagged memory
#define UNTAGGED_END    0x20000000000ULL
// for basebounds the safe stack needs to have implicit tag zero
// map it in the middle of the untagged range
#define STACK_END       0x10000000000ULL
#define KERN_ADDRSPACE (1ULL << 47)
#define BB_TAG_SHIFT 41
#define PTR_GET_TAG(x)      ((uintptr_t)(x) >> BB_TAG_SHIFT)
#define PAGE_SIZE 4096
#define ROUND_TO_PAGE(x) ( (((uintptr_t)(x)) + PAGE_SIZE-1)  & (~(PAGE_SIZE-1)) ) 

/* Stack Utilities */
static const rlim_t DEFAULT_STACK_SIZE = 16 * 1024 * 1024; // 16 MB
#define STACK_END       0x10000000000ULL

// https://github.com/gcc-mirror/gcc/blob/master/include/safe-ctype.h#L93
// https://github.com/gcc-mirror/gcc/blob/master/libiberty/safe-ctype.c#L158

enum {
  /* In C99 */
  _sch_isblank  = 0x0001,	/* space \t */
  _sch_iscntrl  = 0x0002,	/* nonprinting characters */
  _sch_isdigit  = 0x0004,	/* 0-9 */
  _sch_islower  = 0x0008,	/* a-z */
  _sch_isprint  = 0x0010,	/* any printing character including ' ' */
  _sch_ispunct  = 0x0020,	/* all punctuation */
  _sch_isspace  = 0x0040,	/* space \t \n \r \f \v */
  _sch_isupper  = 0x0080,	/* A-Z */
  _sch_isxdigit = 0x0100,	/* 0-9A-Fa-f */

  /* Extra categories useful to cpplib.  */
  _sch_isidst	= 0x0200,	/* A-Za-z_ */
  _sch_isvsp    = 0x0400,	/* \n \r */
  _sch_isnvsp   = 0x0800,	/* space \t \f \v \0 */

  /* Combinations of the above.  */
  _sch_isalpha  = _sch_isupper|_sch_islower,	/* A-Za-z */
  _sch_isalnum  = _sch_isalpha|_sch_isdigit,	/* A-Za-z0-9 */
  _sch_isidnum  = _sch_isidst|_sch_isdigit,	/* A-Za-z0-9_ */
  _sch_isgraph  = _sch_isalnum|_sch_ispunct,	/* isprint and not space */
  _sch_iscppsp  = _sch_isvsp|_sch_isnvsp,	/* isspace + \0 */
  _sch_isbasic  = _sch_isprint|_sch_iscppsp     /* basic charset of ISO C
						   (plus ` and @)  */
};

/* Shorthand */
#define bl _sch_isblank
#define cn _sch_iscntrl
#define di _sch_isdigit
#define is _sch_isidst
#define lo _sch_islower
#define nv _sch_isnvsp
#define pn _sch_ispunct
#define pr _sch_isprint
#define sp _sch_isspace
#define up _sch_isupper
#define vs _sch_isvsp
#define xd _sch_isxdigit

/* Masks.  */
#define L  (const unsigned short) (lo|is   |pr)	/* lower case letter */
#define XL (const unsigned short) (lo|is|xd|pr)	/* lowercase hex digit */
#define U  (const unsigned short) (up|is   |pr)	/* upper case letter */
#define XU (const unsigned short) (up|is|xd|pr)	/* uppercase hex digit */
#define D  (const unsigned short) (di   |xd|pr)	/* decimal digit */
#define P  (const unsigned short) (pn      |pr)	/* punctuation */
#define _  (const unsigned short) (pn|is   |pr)	/* underscore */

#define C  (const unsigned short) (         cn)	/* control character */
#define Z  (const unsigned short) (nv      |cn)	/* NUL */
#define M  (const unsigned short) (nv|sp   |cn)	/* cursor movement: \f \v */
#define V  (const unsigned short) (vs|sp   |cn)	/* vertical space: \r \n */
#define T  (const unsigned short) (nv|sp|bl|cn)	/* tab */
#define S  (const unsigned short) (nv|sp|bl|pr)	/* space */

const unsigned short _sch_istable_[256] =
{
  Z,  C,  C,  C,   C,  C,  C,  C,   /* NUL SOH STX ETX  EOT ENQ ACK BEL */
  C,  T,  V,  M,   M,  V,  C,  C,   /* BS  HT  LF  VT   FF  CR  SO  SI  */
  C,  C,  C,  C,   C,  C,  C,  C,   /* DLE DC1 DC2 DC3  DC4 NAK SYN ETB */
  C,  C,  C,  C,   C,  C,  C,  C,   /* CAN EM  SUB ESC  FS  GS  RS  US  */
  S,  P,  P,  P,   P,  P,  P,  P,   /* SP  !   "   #    $   %   &   '   */
  P,  P,  P,  P,   P,  P,  P,  P,   /* (   )   *   +    ,   -   .   /   */
  D,  D,  D,  D,   D,  D,  D,  D,   /* 0   1   2   3    4   5   6   7   */
  D,  D,  P,  P,   P,  P,  P,  P,   /* 8   9   :   ;    <   =   >   ?   */
  P, XU, XU, XU,  XU, XU, XU,  U,   /* @   A   B   C    D   E   F   G   */
  U,  U,  U,  U,   U,  U,  U,  U,   /* H   I   J   K    L   M   N   O   */
  U,  U,  U,  U,   U,  U,  U,  U,   /* P   Q   R   S    T   U   V   W   */
  U,  U,  U,  P,   P,  P,  P,  _,   /* X   Y   Z   [    \   ]   ^   _   */
  P, XL, XL, XL,  XL, XL, XL,  L,   /* `   a   b   c    d   e   f   g   */
  L,  L,  L,  L,   L,  L,  L,  L,   /* h   i   j   k    l   m   n   o   */
  L,  L,  L,  L,   L,  L,  L,  L,   /* p   q   r   s    t   u   v   w   */
  L,  L,  L,  P,   P,  P,  P,  C,   /* x   y   z   {    |   }   ~   DEL */

  /* high half of unsigned char is locale-specific, so all tests are
     false in "C" locale */
  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,

  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
};

#define _sch_test(c, bit) (_sch_istable_[(c) & 0xff] & (unsigned short)(bit))

#define ISALPHA(c)  _sch_test(c, _sch_isalpha)
#define ISALNUM(c)  _sch_test(c, _sch_isalnum)
#define ISBLANK(c)  _sch_test(c, _sch_isblank)
#define ISCNTRL(c)  _sch_test(c, _sch_iscntrl)
#define ISDIGIT(c)  _sch_test(c, _sch_isdigit)
#define ISGRAPH(c)  _sch_test(c, _sch_isgraph)
#define ISLOWER(c)  _sch_test(c, _sch_islower)
#define ISPRINT(c)  _sch_test(c, _sch_isprint)
#define ISPUNCT(c)  _sch_test(c, _sch_ispunct)
#define ISSPACE(c)  _sch_test(c, _sch_isspace)
#define ISUPPER(c)  _sch_test(c, _sch_isupper)
#define ISXDIGIT(c) _sch_test(c, _sch_isxdigit)

#define ISIDNUM(c)	_sch_test(c, _sch_isidnum)
#define ISIDST(c)	_sch_test(c, _sch_isidst)
#define IS_ISOBASIC(c)	_sch_test(c, _sch_isbasic)
#define IS_VSPACE(c)	_sch_test(c, _sch_isvsp)
#define IS_NVSPACE(c)	_sch_test(c, _sch_isnvsp)
#define IS_SPACE_OR_NUL(c)	_sch_test(c, _sch_iscppsp)

/* /proc/pid/maps support. */
struct mapinfo
{
    uintptr_t start, end;
};

// gcc strtoll
#define ULLONG_MAX (~(ullong_type)0) /* 0xFFFFFFFFFFFFFFFF */
#define LLONG_MAX ((llong_type)(ULLONG_MAX >> 1)) /* 0x7FFFFFFFFFFFFFFF */
#define LLONG_MIN (~LLONG_MAX) /* 0x8000000000000000 */

typedef unsigned long long ullong_type;
typedef long long llong_type;

#endif