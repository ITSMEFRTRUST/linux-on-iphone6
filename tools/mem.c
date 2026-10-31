/* mem - 32-bit physical memory access via /dev/mem. freestanding, raw syscalls.
 * usage: mem [r ADDR [N]] [w ADDR VAL] [s MS]...   (hex ADDR/VAL, N words, dec MS) */
typedef long i64; typedef unsigned int u32; typedef unsigned long u64;
static i64 sc6(i64 n,i64 a,i64 b,i64 c,i64 d,i64 e,i64 f){register i64 x8 asm("x8")=n,x0 asm("x0")=a,x1 asm("x1")=b,
  x2 asm("x2")=c,x3 asm("x3")=d,x4 asm("x4")=e,x5 asm("x5")=f;
  asm volatile("svc 0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory");return x0;}
#define sc(n,a,b,c) sc6(n,a,b,c,0,0,0)
static u64 hex(const char*s){u64 v=0;if(s[0]=='0'&&s[1]=='x')s+=2;for(;*s;s++){int d=*s>='a'?*s-'a'+10:*s>='A'?*s-'A'+10:*s-'0';v=v*16+d;}return v;}
static int dec(const char*s){int v=0;while(*s>='0'&&*s<='9')v=v*10+*s++-'0';return v;}
static void out(const char*s){int n=0;while(s[n])n++;sc(64,1,(i64)s,n);}
static void hx(u64 v,int w){char b[17];for(int i=0;i<w;i++)b[i]="0123456789abcdef"[(v>>(4*(w-1-i)))&15];b[w]=0;out(b);}
static void slp(int ms){struct{i64 s,ns;}t={ms/1000,(ms%1000)*1000000L};sc(101,(i64)&t,0,0);}
static int fd;
static volatile u32 *map(u64 a){u64 pg=a&~0xfffUL;
  i64 p=sc6(222,0,0x2000,3,1,fd,pg); /* PROT_READ|WRITE, MAP_SHARED */
  if(p<0&&p>-4096){out("mmap failed\n");sc(93,1,0,0);} return (volatile u32*)(p+(a-pg));}
void _start_c(i64*sp){int argc=sp[0];char**a=(char**)(sp+1);
  fd=sc(56,-100,(i64)"/dev/mem",2|0x101000); if(fd<0){out("no /dev/mem\n");sc(93,1,0,0);} /* O_RDWR|O_SYNC */
  for(int i=1;i<argc;i++){char c=a[i][0];
    if(c=='r'){u64 ad=hex(a[++i]);int n=(i+1<argc&&a[i+1][0]>='0'&&a[i+1][0]<='9'&&a[i+1][1]!='x')?dec(a[++i]):1;
      for(int k=0;k<n;k++){u64 x=ad+4*k;if(k%4==0){if(k)out("\n");hx(x,9);out(":");}
        volatile u32*p=map(x);out(" ");hx(*p,8);sc(215,(i64)((u64)p&~0xfffUL),0x2000,0);}out("\n");}
    else if(c=='w'){u64 ad=hex(a[i+1]);u32 v=hex(a[i+2]);i+=2;volatile u32*p=map(ad);*p=v;
      out("w ");hx(ad,9);out(" = ");hx(v,8);out(" -> ");hx(*p,8);out("\n");sc(215,(i64)((u64)p&~0xfffUL),0x2000,0);}
    else if(c=='s'){slp(dec(a[++i]));}}
  sc(93,0,0,0);}
asm(".globl _start\n_start: mov x0, sp\n bl _start_c\n");
