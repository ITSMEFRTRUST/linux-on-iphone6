/* i2c - generic register poking on iPhone 6 Linux. freestanding, raw syscalls.
 * usage: i2c BUS ADDR cmds...   (BUS dec, ADDR hex)
 * cmds: r RR  read reg   w RR VV  write reg   d SS EE  dump SS..EE
 *       R RRRR / W RRRR VV  16-bit register address (PMU)
 *       x  scan bus (1-byte read at every addr)   s MS  sleep */
typedef long i64; typedef unsigned char u8;
static i64 sc(i64 n,i64 a,i64 b,i64 c){register i64 x8 asm("x8")=n,x0 asm("x0")=a,x1 asm("x1")=b,x2 asm("x2")=c;
  asm volatile("svc 0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2):"memory");return x0;}
static int hex(const char*s){int v=0;if(s[0]=='0'&&s[1]=='x')s+=2;for(;*s;s++){int d=*s>='a'?*s-'a'+10:*s>='A'?*s-'A'+10:*s-'0';v=v*16+d;}return v;}
static int dec(const char*s){int v=0;while(*s>='0'&&*s<='9')v=v*10+*s++-'0';return v;}
static void out(const char*s){int n=0;while(s[n])n++;sc(64,1,(i64)s,n);}
static void hx(int v){char b[3]={"0123456789abcdef"[(v>>4)&15],"0123456789abcdef"[v&15],0};out(b);}
static int fd=-1;
static int adr(int a){return sc(29,fd,0x0706,a)<0?-1:0;}
static int rd(int r){u8 b=r;if(sc(64,fd,(i64)&b,1)!=1)return -1;if(sc(63,fd,(i64)&b,1)!=1)return -1;return b;}
static int wr(int r,int v){u8 b[2]={r,v};return sc(64,fd,(i64)b,2)==2?0:-1;}
static int rd16(int r){u8 b[2]={r>>8,r};if(sc(64,fd,(i64)b,2)!=2)return -1;if(sc(63,fd,(i64)b,1)!=1)return -1;return b[0];}
static int wr16(int r,int v){u8 b[3]={r>>8,r,v};return sc(64,fd,(i64)b,3)==3?0:-1;}
static void slp(int ms){struct{i64 s,ns;}t={ms/1000,(ms%1000)*1000000L};sc(101,(i64)&t,0,0);}
void _start_c(i64*sp){int argc=sp[0];char**a=(char**)(sp+1);
  if(argc<3){out("usage: i2c BUS ADDR [r RR|w RR VV|d SS EE|x|s MS]...\n");sc(93,1,0,0);}
  char dev[]="/dev/i2c-0";dev[9]='0'+dec(a[1]);
  fd=sc(56,-100,(i64)dev,2);if(fd<0){out("open failed\n");sc(93,1,0,0);}
  int ad=hex(a[2]);if(adr(ad)<0){out("I2C_SLAVE_FORCE failed\n");sc(93,1,0,0);}
  for(int i=3;i<argc;i++){char c=a[i][0];
    if(c=='r'){int r=hex(a[++i]),v=rd(r);out("r ");hx(r);out("=");if(v<0)out("FAIL");else hx(v);out("\n");}
    else if(c=='R'){int r=hex(a[++i]),v=rd16(r);out("R ");hx(r>>8);hx(r);out("=");if(v<0)out("FAIL");else hx(v);out("\n");}
    else if(c=='W'){int r=hex(a[i+1]),v=hex(a[i+2]);i+=2;out("W ");hx(r>>8);hx(r);out("=");hx(v);out(wr16(r,v)?" FAIL\n":"\n");}
    else if(c=='w'){int r=hex(a[i+1]),v=hex(a[i+2]);i+=2;out("w ");hx(r);out("=");hx(v);out(wr(r,v)?" FAIL\n":"\n");}
    else if(c=='d'){int s=hex(a[i+1]),e=hex(a[i+2]);i+=2;
      for(int r=s;r<=e;r++){if(r==s||(r&15)==0){if(r!=s)out("\n");hx(r);out(":");}
        int v=rd(r);out(" ");if(v<0)out("--");else hx(v);}out("\n");}
    else if(c=='x'){for(int x=3;x<0x78;x++){u8 b;adr(x);if(sc(63,fd,(i64)&b,1)==1){hx(x);out(" ");}}out("\n");adr(ad);}
    else if(c=='s'){slp(dec(a[++i]));}}
  sc(93,0,0,0);}
asm(".globl _start\n_start: mov x0, sp\n bl _start_c\n");
