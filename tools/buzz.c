/* buzz [ms] [strength 0-65535] - rumble /dev/input/event0 via FF_RUMBLE. freestanding, raw syscalls */
typedef unsigned short u16; typedef short s16; typedef unsigned int u32; typedef long i64;
struct ff_effect { u16 type; s16 id; u16 dir; u16 trig_btn, trig_int; u16 rep_len, rep_delay; u16 pad0;
  u16 strong, weak; char pad[28]; };
struct input_event { i64 sec, usec; u16 type, code; int value; };
static i64 sc(i64 n,i64 a,i64 b,i64 c){register i64 x8 asm("x8")=n,x0 asm("x0")=a,x1 asm("x1")=b,x2 asm("x2")=c;
  asm volatile("svc 0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2):"memory");return x0;}
static int num(const char*s){int v=0;while(*s>='0'&&*s<='9')v=v*10+*s++-'0';return v;}
#define EVIOCSFF 0x40304580UL
void _start_c(i64 *sp){ int argc=sp[0]; char**argv=(char**)(sp+1);
  int ms=argc>1?num(argv[1]):500, st=argc>2?num(argv[2]):0xC000;
  int fd=sc(56,-100,(i64)"/dev/input/event0",2);  /* openat O_RDWR */
  struct ff_effect e={0}; e.type=0x50; e.id=-1; e.rep_len=ms; e.strong=st; e.weak=st;
  i64 r=sc(29,fd,EVIOCSFF,(i64)&e);
  struct input_event ev={0,0,0x15,(u16)e.id,1}; sc(64,fd,(i64)&ev,sizeof ev);
  struct { i64 s, ns; } ts={ms/1000+1,(ms%1000)*1000000}; sc(101,(i64)&ts,0,0);
  sc(93, (fd<0)?2:(r<0?3:0),0,0); }
asm(".globl _start\n_start: mov x0, sp\n bl _start_c\n");
