#!/usr/bin/env python3
# helper: read VAs from kc.macho (segments parsed from load commands)
import struct,sys
D=open(sys.argv[1] if len(sys.argv) > 1 else 're/kc.macho','rb').read()	# your own kernelcache, extracted from your IPSW
ncmds=struct.unpack_from('<I',D,16)[0]; off=32; SEGS=[]
for i in range(ncmds):
    cmd,sz=struct.unpack_from('<II',D,off)
    if cmd==0x19:
        name=D[off+8:off+24].rstrip(b'\0').decode(); va,vs,fo,fs=struct.unpack_from('<QQQQ',D,off+24)
        nsect=struct.unpack_from('<I',D,off+64)[0]
        for s in range(nsect):
            so=off+72+s*80; sn=D[so:so+16].rstrip(b'\0').decode(); a,z,o=struct.unpack_from('<QQI',D,so+32)
            SEGS.append((name+'.'+sn,a,z,o))
    off+=sz
def fo(va):
    for n,a,z,o in SEGS:
        if a<=va<a+z and o: return o+va-a
    raise KeyError(hex(va))
def u64(va): return struct.unpack_from('<Q',D,fo(va))[0]
def u32(va): return struct.unpack_from('<I',D,fo(va))[0]
def ptr(va):
    r=u64(va); return ((r&0xffffffffff)|0xffffff0000000000) if r else 0
def cstr(va):
    p=fo(va); return D[p:D.index(b'\0',p)].decode(errors='replace')
if __name__=='__main__':
    if sys.argv[1]=='segs':
        for s in SEGS: print(s[0],hex(s[1]),hex(s[2]),hex(s[3]))
