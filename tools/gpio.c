/* gpio - drive/read gpiochip0 lines via the v2 chardev ABI. freestanding, raw syscalls.
 * usage: gpio [g N]... [N=V]... [s MS]   g = read line as input, N=V = drive output,
 *        s = keep the requested outputs held for MS ms before exiting (lines are released on exit) */
typedef long i64; typedef unsigned int u32; typedef unsigned long u64;
static i64 sc(i64 n,i64 a,i64 b,i64 c){register i64 x8 asm("x8")=n,x0 asm("x0")=a,x1 asm("x1")=b,x2 asm("x2")=c;
  asm volatile("svc 0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2):"memory");return x0;}
static int dec(const char*s){int v=0;while(*s>='0'&&*s<='9')v=v*10+*s++-'0';return v;}
static void out(const char*s){int n=0;while(s[n])n++;sc(64,1,(i64)s,n);}
static void num(int v){char b[12];int i=11;b[i]=0;do{b[--i]='0'+v%10;v/=10;}while(v);out(b+i);}
static void slp(int ms){struct{i64 s,ns;}t={ms/1000,(ms%1000)*1000000L};sc(101,(i64)&t,0,0);}
struct attr { u32 id, pad; u64 val; u64 mask; };
struct req { u32 offsets[64]; char consumer[32]; u64 flags; u32 num_attrs, pad0[5]; struct attr attrs[10];
  u32 num_lines, ev_buf, pad1[5]; int fd; };
struct vals { u64 bits, mask; };
#define GET_LINE   0xC250B407UL
#define GET_VALUES 0xC010B40EUL
#define FLAG_INPUT  (1UL<<2)
#define FLAG_OUTPUT (1UL<<3)
#define ATTR_OUTPUT_VALUES 2
static int chip;
static int request(int line, u64 flags, int val){
  static struct req r; for(unsigned i=0;i<sizeof r;i++)((char*)&r)[i]=0;
  r.offsets[0]=line; r.num_lines=1; r.flags=flags; r.consumer[0]='c';
  if(flags&FLAG_OUTPUT){r.num_attrs=1;r.attrs[0].id=ATTR_OUTPUT_VALUES;r.attrs[0].val=val;r.attrs[0].mask=1;}
  return sc(29,chip,GET_LINE,(i64)&r)<0?-1:r.fd;}
void _start_c(i64*sp){int argc=sp[0];char**a=(char**)(sp+1);
  chip=sc(56,-100,(i64)"/dev/gpiochip0",2); if(chip<0){out("no gpiochip0\n");sc(93,1,0,0);}
  for(int i=1;i<argc;i++){
    if(a[i][0]=='g'){int n=dec(a[++i]),fd=request(n,FLAG_INPUT,0);struct vals v={0,1};
      out("gpio ");num(n);out(" = ");
      if(fd<0||sc(29,fd,GET_VALUES,(i64)&v)<0)out("busy/fail\n");else{num(v.bits&1);out("\n");}
      if(fd>=0)sc(57,fd,0,0);}
    else if(a[i][0]=='s'){slp(dec(a[++i]));}
    else{int n=dec(a[i]);const char*p=a[i];while(*p&&*p!='=')p++;int v=*p?dec(p+1):1;
      out("gpio ");num(n);out(request(n,FLAG_OUTPUT,v)<0?" set FAIL\n":v?" -> 1\n":" -> 0\n");}}
  sc(93,0,0,0);}
asm(".globl _start\n_start: mov x0, sp\n bl _start_c\n");
