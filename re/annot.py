#!/usr/bin/env python3
# annotate llvm-objdump output of kc.macho: resolve adrp+add / adrp+ldr to cstrings
import re,sys,struct
d=open(sys.argv[1],'rb').read()
CS=(0xfffffff007207a10,0x24fa94,0x203a10); OSL=(0xfffffff0074574a4,0x343e2,0x4534a4)
def s_at(va):
  for a,z,o in (CS,OSL):
    if a<=va<a+z:
      p=o+va-a; e=d.index(b'\0',p); return d[p:e].decode(errors='replace')[:90]
pages={}
for line in sys.stdin:
  line=line.rstrip('\n'); m=re.search(r'adrp\s+(x\d+), 0x([0-9a-f]+)',line)
  if m: pages[m.group(1)]=int(m.group(2),16)
  m=re.search(r'add\s+(x\d+), (x\d+), #0x([0-9a-f]+)',line)
  if m and m.group(2) in pages:
    va=pages[m.group(2)]+int(m.group(3),16); s=s_at(va)
    if s is not None: line+=f'   ; "{s}"'
    else: line+=f'   ; &{va:#x}'
  print(line)
