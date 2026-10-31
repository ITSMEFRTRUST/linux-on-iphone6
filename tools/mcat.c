/* mcat - time how fast the MCA TX FIFO drains, to measure the frame clock. freestanding, raw syscalls.
 * usage: mcat BASE PORT N [REPS]  (hex BASE/PORT, dec N words)
 * Per rep: write N words to PORT, clear IRQ status (BASE+0x40), poll until TX underflow (bit12).
 * Prints ns per word; overflow (bit13) during the fill = PORT is the FIFO and N > free space. */
typedef long i64; typedef unsigned int u32; typedef unsigned long u64;
static i64 sc6(i64 n,i64 a,i64 b,i64 c,i64 d,i64 e,i64 f){register i64 x8 asm("x8")=n,x0 asm("x0")=a,x1 asm("x1")=b,
  x2 asm("x2")=c,x3 asm("x3")=d,x4 asm("x4")=e,x5 asm("x5")=f;
  asm volatile("svc 0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory");return x0;}
#define sc(n,a,b,c) sc6(n,a,b,c,0,0,0)
static u64 hex(const char*s){u64 v=0;if(s[0]=='0'&&s[1]=='x')s+=2;for(;*s;s++){int d=*s>='a'?*s-'a'+10:*s>='A'?*s-'A'+10:*s-'0';v=v*16+d;}return v;}
static int dec(const char*s){int v=0;while(*s>='0'&&*s<='9')v=v*10+*s++-'0';return v;}
static void out(const char*s){int n=0;while(s[n])n++;sc(64,1,(i64)s,n);}
static void hx(u64 v,int w){char b[17];for(int i=0;i<w;i++)b[i]="0123456789abcdef"[(v>>(4*(w-1-i)))&15];b[w]=0;out(b);}
static void dc(u64 v){char b[21];int i=20;b[i]=0;do{b[--i]='0'+v%10;v/=10;}while(v);out(b+i);}
static u64 now(void){struct{i64 s,ns;}t;sc(113,1,(i64)&t,0);return t.s*1000000000UL+t.ns;}
static int fd;
static volatile u32 *map(u64 a){u64 pg=a&~0xfffUL;
  i64 p=sc6(222,0,0x2000,3,1,fd,pg);
  if(p<0&&p>-4096){out("mmap failed\n");sc(93,1,0,0);} return (volatile u32*)(p+(a-pg));}
void _start_c(i64*sp){int argc=sp[0];char**a=(char**)(sp+1);
  if(argc<4){out("usage: mcat BASE PORT N [REPS]\n");sc(93,1,0,0);}
  fd=sc(56,-100,(i64)"/dev/mem",2|0x101000); if(fd<0){out("no /dev/mem\n");sc(93,1,0,0);}
  volatile u32*st=map(hex(a[1])+0x40),*port=map(hex(a[2]));
  int n=dec(a[3]),reps=argc>4?dec(a[4]):3;
  for(int r=0;r<reps;r++){
    *st=0xb0003000;
    for(int k=0;k<n;k++)*port=(k&8)?0xc000c000:0x40004000;
    u32 s0=*st;*st=0xb0003000;u64 t0=now(),t1=t0;u32 s=0;
    while(!((s=*st)&0x1000)){t1=now();if(t1-t0>2000000000UL)break;}
    out("fill status ");hx(s0,8);out("  end status ");hx(s,8);out("  ");dc(t1-t0);out(" ns");
    if(n)out("  ="),dc((t1-t0)/n),out(" ns/word");out("\n");}
  sc(93,0,0,0);}
asm(".globl _start\n_start: mov x0, sp\n bl _start_c\n");
