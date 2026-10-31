import re,sys
L=open(sys.argv[1]).read().splitlines()
targets={sys.argv[2]:'R',sys.argv[3]:'W'}
# function starts: line after a 'ret' or unconditional b at function end; approximate via 'sub sp' / 'stp x29' prologues
fn=None; lastfnstr={}
for i,l in enumerate(L):
  if re.search(r'\s(sub\s+sp, sp|stp\s+x\d+, x\d+, \[sp, #-)',l) and i>0 and re.search(r'\sret$|\sb\s+0x',L[i-1]): fn=l.split(':')[0].strip()
  m=re.search(r'; "(_[A-Za-z]\w+|[a-z]\w+Command|setMCLKEnabled|setIISControllerActive_Forced|_getMCLKClkCon|_getFIFODepth|_setIISConfig_v4)"',l)
  if m and fn: lastfnstr[fn]=m.group(1)
  m=re.search(r'bl\s+0x([0-9a-f]+)',l)
  if m and m.group(1) in targets:
    w1=w2='?'
    for j in range(i-1,max(i-25,0),-1):
      if w1=='?':
        mm=re.search(r'(mov|orr)\s+w1, (wzr, )?#(0x[0-9a-f]+|-?\d+)',L[j])
        if mm: w1=mm.group(3)
        elif re.search(r'\s(mov|ldr|add|ldur)\w*\s+[wx]1,',L[j]): w1='reg:'+L[j].split('\t')[-1].strip()
      if w2=='?':
        mm=re.search(r'(mov|orr)\s+w2, (wzr, )?#(0x[0-9a-f]+|-?\d+)',L[j])
        if mm: w2=mm.group(3)
        elif re.search(r'\s(mov|ldr|add|orr|and|bfi|bfxil|ldur)\w*\s+[wx]2,',L[j]): w2='<'+L[j].split('\t')[-1].strip()+'>'
    print(targets[m.group(1)], l.split(':')[0].strip(), 'fn',fn, 'idx',w1, ('val '+w2) if targets[m.group(1)]=='W' else '')
print('fnnames:',lastfnstr)
