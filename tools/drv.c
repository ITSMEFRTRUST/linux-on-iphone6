/* drv - live DRV2604 poking on iPhone 6 (i2c1 @0x5a). freestanding, raw syscalls.
 * cmds: p [strength_hex]  open event0 + play 30s FF_RUMBLE (driver raises EN, keeps it up until exit)
 *       w RR VV  write reg   r RR  read reg   d  dump 0x00-0x22   s MS  sleep */
typedef unsigned short u16; typedef short s16; typedef long i64; typedef unsigned char u8;
struct ff_effect { u16 type; s16 id; u16 dir; u16 trig_btn, trig_int; u16 rep_len, rep_delay; u16 pad0;
  u16 strong, weak; char pad[28]; };
struct input_event { i64 sec, usec; u16 type, code; int value; };
static i64 sc(i64 n,i64 a,i64 b,i64 c){register i64 x8 asm("x8")=n,x0 asm("x0")=a,x1 asm("x1")=b,x2 asm("x2")=c;
  asm volatile("svc 0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2):"memory");return x0;}
static int hex(const char*s){int v=0;if(s[0]=='0'&&s[1]=='x')s+=2;for(;*s;s++){int d=*s>='a'?*s-'a'+10:*s>='A'?*s-'A'+10:*s-'0';v=v*16+d;}return v;}
static int dec(const char*s){int v=0;while(*s>='0'&&*s<='9')v=v*10+*s++-'0';return v;}
static void out(const char*s){int n=0;while(s[n])n++;sc(64,1,(i64)s,n);}
static void hx(int v){char b[4]={"0123456789abcdef"[(v>>4)&15],"0123456789abcdef"[v&15],0};out(b);}
static int i2c=-1;
static int rd(int r){u8 b=r;if(sc(64,i2c,(i64)&b,1)!=1)return -1;if(sc(63,i2c,(i64)&b,1)!=1)return -1;return b;}
static int wr(int r,int v){u8 b[2]={r,v};return sc(64,i2c,(i64)b,2)==2?0:-1;}
static void slp(int ms){struct{i64 s,ns;}t={ms/1000,(ms%1000)*1000000L};sc(101,(i64)&t,0,0);}
void _start_c(i64*sp){int argc=sp[0];char**a=(char**)(sp+1);
  i2c=sc(56,-100,(i64)"/dev/i2c-1",2); if(i2c<0){out("no /dev/i2c-1\n");sc(93,1,0,0);}
  if(sc(29,i2c,0x0706,0x5a)<0){out("I2C_SLAVE_FORCE failed\n");sc(93,1,0,0);}
  for(int i=1;i<argc;i++){char c=a[i][0];
    if(c=='p'){int st=(i+1<argc&&a[i+1][0]!='w'&&a[i+1][0]!='r'&&a[i+1][0]!='d'&&a[i+1][0]!='s')?hex(a[++i]):0xffff;
      int fd=sc(56,-100,(i64)"/dev/input/event0",2);struct ff_effect e={0};e.type=0x50;e.id=-1;e.rep_len=30000;e.strong=st;e.weak=st;
      if(fd<0||sc(29,fd,0x40304580UL,(i64)&e)<0){out("ff upload failed\n");continue;}
      struct input_event ev={0,0,0x15,(u16)e.id,1};sc(64,fd,(i64)&ev,sizeof ev);slp(20);out("playing\n");}
    else if(c=='w'){int r=hex(a[i+1]),v=hex(a[i+2]);i+=2;out("w ");hx(r);out("=");hx(v);out(wr(r,v)?" FAIL\n":"\n");}
    else if(c=='r'){int r=hex(a[++i]),v=rd(r);out("r ");hx(r);out("=");if(v<0)out("FAIL");else hx(v);out("\n");}
    else if(c=='d'){for(int r=0;r<=0x22;r++){int v=rd(r);hx(r);out(":");if(v<0)out("--");else hx(v);out((r&7)==7?"\n":" ");}out("\n");}
    else if(c=='s'){slp(dec(a[++i]));}}
  sc(93,0,0,0);}
asm(".globl _start\n_start: mov x0, sp\n bl _start_c\n");
