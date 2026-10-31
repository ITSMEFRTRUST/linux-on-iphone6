/* tone - feed the MCA2 TX shim FIFO from the CPU (PIO). freestanding, raw syscalls.
 * Writes the 8-word aperture +0x00..0x1c in order like the SIO fw (mcat hit one address and hung the phone).
 * Never writes more than the shim's free count (+0x18 [8:0]) and never busy-polls MMIO.
 * usage: tone m [N]                 measure: fill FIFO, sample free count every 100 us -> bytes/s
 *        tone p SECS [SHIFT] [DIV]  play 48-sample sine (1 kHz at 48 kHz), amplitude >> SHIFT (default 3),
 *                                   each sample repeated DIV times (pitch / DIV); prints underruns
 *        tone z SECS                play silence (zeros), for clock/underrun tests
 *        tone f FILE|- [SHIFT]      stream raw s16le 48 kHz mono PCM from FILE (or stdin) until EOF, >> SHIFT (default 0)
 * Shim must already be configured + enabled (tools/mcamaster). */
typedef long i64; typedef unsigned int u32; typedef unsigned long u64; typedef short i16;
static i64 sc6(i64 n,i64 a,i64 b,i64 c,i64 d,i64 e,i64 f){register i64 x8 asm("x8")=n,x0 asm("x0")=a,x1 asm("x1")=b,
  x2 asm("x2")=c,x3 asm("x3")=d,x4 asm("x4")=e,x5 asm("x5")=f;
  asm volatile("svc 0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory");return x0;}
#define sc(n,a,b,c) sc6(n,a,b,c,0,0,0)
static int dec(const char*s){int v=0;while(*s>='0'&&*s<='9')v=v*10+*s++-'0';return v;}
static void out(const char*s){int n=0;while(s[n])n++;sc(64,1,(i64)s,n);}
static void hx(u64 v,int w){char b[17];for(int i=0;i<w;i++)b[i]="0123456789abcdef"[(v>>(4*(w-1-i)))&15];b[w]=0;out(b);}
static void dc(u64 v){char b[21];int i=20;b[i]=0;do{b[--i]='0'+v%10;v/=10;}while(v);out(b+i);}
static u64 now(void){struct{i64 s,ns;}t;sc(113,1,(i64)&t,0);return t.s*1000000000UL+t.ns;}
static void nsl(i64 ns){struct{i64 s,ns;}t={0,ns};sc(101,(i64)&t,0,0);}
static int fd;
static volatile u32 *map(u64 a){u64 pg=a&~0xfffUL;
  i64 p=sc6(222,0,0x1000,3,1,fd,pg);
  if(p<0&&p>-4096){out("mmap failed\n");sc(93,1,0,0);} return (volatile u32*)(p+(a-pg));}
static const i16 sine[48]={0,4277,8481,12539,16383,19947,23170,25996,28377,30273,31650,32487,32767,32487,31650,30273,
  28377,25996,23170,19947,16383,12539,8481,4277,0,-4277,-8481,-12539,-16383,-19947,-23170,-25996,-28377,-30273,
  -31650,-32487,-32767,-32487,-31650,-30273,-28377,-25996,-23170,-19947,-16384,-12539,-8481,-4277};
static volatile u32 *shim,*port,*st;
#define FREE() (shim[6]&0x1ff)
void _start_c(i64*sp){int argc=sp[0];char**a=(char**)(sp+1);
  if(argc<2){out("usage: tone m [N] | p SECS [SHIFT] [DIV] | z SECS | f FILE|- [SHIFT]\n");sc(93,1,0,0);}
  fd=sc(56,-100,(i64)"/dev/mem",2|0x101000); if(fd<0){out("no /dev/mem\n");sc(93,1,0,0);}
  shim=map(0x20a0a9000);port=map(0x20a0aa000);st=map(0x20a0a8040);
  if(shim[4]!=1){out("shim not enabled (+0x10 != 1), run mcamaster first\n");sc(93,1,0,0);}
  char m=a[1][0];
  if(m=='m'){int n=argc>2?dec(a[2]):200;
    u32 f=FREE();for(u32 k=0;k<f/4;k++)port[k&7]=0;
    u64 t0=now();u32 f0=FREE(),fl=f0;out("free after fill ");dc(f0);out("\n");
    for(int i=0;i<n;i++){nsl(100000);u32 x=FREE();if(x!=fl||i==n-1){dc((now()-t0)/1000);out(" us free ");dc(x);out("\n");fl=x;}
      if(x>=0xcc)break;}
    u64 dt=now()-t0;u32 f1=FREE();out("drained ");dc(f1-f0);out(" B in ");dc(dt/1000);out(" us");
    if(f1>f0&&dt)out(" = "),dc((u64)(f1-f0)*1000000000UL/dt),out(" B/s (expect 96000 for 48k mono 16-bit)");
    out("\nstatus 0x40 = ");hx(*st,8);out("\n");sc(93,0,0,0);}
  if(m=='f'){if(argc<3){out("tone f FILE|- [SHIFT]\n");sc(93,1,0,0);}
    int in=(a[2][0]=='-'&&!a[2][1])?0:sc(56,-100,(i64)a[2],0); if(in<0){out("can't open file\n");sc(93,1,0,0);}
    int sh=argc>3?dec(a[3]):0; static i16 buf[16384]; i64 have=0,pos=0; int eof=0;
    struct{int prio;}p={50};sc(119,0,1,(i64)&p);
    u64 t0=now(),words=0;u32 under=0;
    *st=0xb0003000;
    for(;;){ /* keep >= 4 KiB of samples buffered; read() from tmpfs is quick */
      if(!eof&&have-pos<2048){i64 k=0;for(i64 j=pos;j<have;j++)buf[k++]=buf[j];have=k;pos=0;
        i64 n=sc(63,in,(i64)(buf+have),(16384-have)*2); if(n<=0)eof=1; else have+=n/2;}
      if(eof&&have-pos<2)break;
      u32 f=FREE(); if(f>=0xcc&&words)under++;
      for(u32 k=0;k<f/4&&have-pos>=2;k++){
        u32 s0=(u32)(i16)(buf[pos]>>sh)&0xffff,s1=(u32)(i16)(buf[pos+1]>>sh)&0xffff;pos+=2;
        port[words&7]=s0|s1<<16;words++;}
      nsl(250000);}
    for(int z=0;z<16;z++){u32 f=FREE();for(u32 k=0;k<f/4;k++)port[(words++)&7]=0;nsl(500000);} /* tail of silence, no pop */
    u64 dt=now()-t0;out("played ");dc(words*2);out(" samples in ");dc(dt/1000000);out(" ms = ");
    dc(dt?words*4*1000000000UL/dt:0);out(" B/s, underruns ");dc(under);out(", status 0x40 = ");hx(*st,8);out("\n");
    sc(93,0,0,0);}
  int secs=argc>2?dec(a[2]):2,sh=argc>3?dec(a[3]):3,dv=argc>4?dec(a[4]):1;if(dv<1)dv=1;
  struct{int prio;}p={50};sc(119,0,1,(i64)&p); /* SCHED_FIFO 50; ignore failure */
  u64 end=now()+(u64)secs*1000000000UL,words=0;u32 under=0,i=0,r=0;
  *st=0xb0003000;
  while(now()<end){u32 f=FREE();
    if(f>=0xcc&&words)under++;
    for(u32 k=0;k<f/4;k++){u32 s0=0,s1=0;
      if(m=='p'){s0=(u32)(i16)(sine[i]>>sh)&0xffff;if(++r>=dv){r=0;if(++i==48)i=0;}
                 s1=(u32)(i16)(sine[i]>>sh)&0xffff;if(++r>=dv){r=0;if(++i==48)i=0;}}
      port[words&7]=s0|s1<<16;words++;}
    nsl(250000);}
  out("words ");dc(words);out(" = ");dc(words*4/secs);out(" B/s, underruns ");dc(under);
  out(", status 0x40 = ");hx(*st,8);out("\n");
  sc(93,0,0,0);}
asm(".globl _start\n_start: mov x0, sp\n bl _start_c\n");
