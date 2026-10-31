/* spi - raw full-duplex SPI transfers via spidev on iPhone 6 Linux. freestanding, raw syscalls.
 * usage: spi DEV HZ MODE HEX [+N]...   one transfer per HEX arg (tx bytes, "-" = none),
 *        +N pads that transfer with N 0xff bytes; prints the rx bytes of each transfer.
 *        a single "c" arg after a transfer deasserts CS after it (cs_change); by default CS stays low for the whole message. */
typedef long i64; typedef unsigned long u64; typedef unsigned int u32;
typedef unsigned short u16; typedef unsigned char u8;
static i64 sc(i64 n,i64 a,i64 b,i64 c){register i64 x8 asm("x8")=n,x0 asm("x0")=a,x1 asm("x1")=b,x2 asm("x2")=c;
  asm volatile("svc 0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2):"memory");return x0;}
static int hv(char c){return c>='a'?c-'a'+10:c>='A'?c-'A'+10:c-'0';}
static int dec(const char*s){int v=0;while(*s>='0'&&*s<='9')v=v*10+*s++-'0';return v;}
static void out(const char*s){int n=0;while(s[n])n++;sc(64,1,(i64)s,n);}
static void hx(int v){char b[3]={"0123456789abcdef"[(v>>4)&15],"0123456789abcdef"[v&15],0};out(b);}
struct xfer{u64 tx,rx;u32 len,hz;u16 delay;u8 bpw,cs_change,txn,rxn,wdelay,pad;};
static u8 tb[16][4096],rb[16][4096];
void _start_c(i64*sp){int argc=sp[0];char**a=(char**)(sp+1);
  if(argc<5){out("usage: spi DEV HZ MODE HEX[+N] [c] ...\n");sc(93,1,0,0);}
  int fd=sc(56,-100,(i64)a[1],2);if(fd<0){out("open failed\n");sc(93,1,0,0);}
  u8 mode=dec(a[3]);if(sc(29,fd,0x40016b01,(i64)&mode)<0){out("set mode failed\n");sc(93,1,0,0);}
  struct xfer x[16];int n=0;
  for(int i=4;i<argc&&n<16;i++){char*s=a[i];
    if(s[0]=='c'&&!s[1]){if(n)x[n-1].cs_change=1;continue;}
    u32 len=0;if(s[0]=='-')s++;
    while(*s&&*s!='+'){tb[n][len++]=hv(s[0])<<4|hv(s[1]);s+=2;}
    if(*s=='+'){int k=dec(s+1);while(k--&&len<4096)tb[n][len++]=0xff;}
    x[n]=(struct xfer){(u64)tb[n],(u64)rb[n],len,dec(a[2]),0,8,0,0,0,0,0};n++;}
  /* SPI_IOC_MESSAGE(n) = _IOW('k', 0, char[32*n]) */
  i64 r=sc(29,fd,0x40006b00|((i64)(32*n)<<16),(i64)x);
  if(r<0){out("transfer failed\n");sc(93,1,0,0);}
  for(int t=0;t<n;t++){for(u32 j=0;j<x[t].len;j++){hx(rb[t][j]);out((j&31)==31?"\n":" ");}out("\n");}
  sc(93,0,0,0);}
asm(".globl _start\n_start: mov x0, sp\n bl _start_c\n");
