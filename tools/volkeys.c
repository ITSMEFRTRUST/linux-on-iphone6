/* volkeys - iPhone volume buttons -> 'Speaker DAC' volume via amixer. freestanding, raw syscalls.
 * usage: volkeys [EVDEV] [START]   (default /dev/input/event1 = gpio-keys, START 164 = -20 dB)
 * DAC scale: 0.5 dB/step, 204 = 0 dB; capped there to spare the speaker. 4 steps (2 dB) per press/repeat.
 * Ring/silent switch (KEY_MUTE): silent = DAC 0, back = restore. */
typedef long i64; typedef unsigned short u16; typedef int i32;
static i64 sc6(i64 n,i64 a,i64 b,i64 c,i64 d,i64 e,i64 f){register i64 x8 asm("x8")=n,x0 asm("x0")=a,x1 asm("x1")=b,
  x2 asm("x2")=c,x3 asm("x3")=d,x4 asm("x4")=e,x5 asm("x5")=f;
  asm volatile("svc 0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory");return x0;}
#define sc(n,a,b,c) sc6(n,a,b,c,0,0,0)
static int dec(const char*s){int v=0;while(*s>='0'&&*s<='9')v=v*10+*s++-'0';return v;}
static void out(const char*s){int n=0;while(s[n])n++;sc(64,1,(i64)s,n);}
static char *num(int v,char*b){int i=11;b[i]=0;do{b[--i]='0'+v%10;v/=10;}while(v);return b+i;}
struct ev{i64 sec,usec;u16 type,code;i32 value;};
#define KEY_MUTE 113
#define KEY_VOLDOWN 114
#define KEY_VOLUP 115
#define MAXVOL 204
static void setvol(int v){char b[12];char*n=num(v,b);
  char*argv[]={"amixer","-q","sset","Speaker DAC",n,0};char*envp[]={0};
  i64 pid=sc6(220,17/*SIGCHLD*/,0,0,0,0,0);           /* clone() = fork */
  if(pid==0){sc(221,(i64)"/usr/bin/amixer",(i64)argv,(i64)envp);sc(93,127,0,0);}
  if(pid>0)sc6(260,pid,0,0,0,0,0);                     /* wait4 */
  out("vol ");out(n);out("\n");}
void _start_c(i64*sp){int argc=sp[0];char**a=(char**)(sp+1);
  const char*dev=argc>1?a[1]:"/dev/input/event1";int vol=argc>2?dec(a[2]):164,muted=0;
  int fd=sc(56,-100,(i64)dev,0);if(fd<0){out("can't open evdev\n");sc(93,1,0,0);}
  setvol(vol);
  for(;;){struct ev e;if(sc(63,fd,(i64)&e,sizeof e)!=sizeof e)sc(93,1,0,0);
    if(e.type!=1)continue;                              /* EV_KEY */
    if(e.code==KEY_MUTE){muted=e.value!=0;setvol(muted?0:vol);continue;}
    if(e.value==0)continue;                             /* release */
    if(e.code==KEY_VOLUP)vol+=4;else if(e.code==KEY_VOLDOWN)vol-=4;else continue;
    if(vol>MAXVOL)vol=MAXVOL;if(vol<0)vol=0;
    if(!muted)setvol(vol);}}
asm(".globl _start\n_start: mov x0, sp\n bl _start_c\n");
