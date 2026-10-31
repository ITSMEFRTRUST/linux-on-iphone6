# long Wikipedia-like page for perf (object count / build time): python3 mklong.py > long.html
import random
random.seed(7)
W = "the of and a to in is was for on that with as by at from his her an were are which this be or has had it not but one also first new".split()
def para(n):
    out = []
    for i in range(n):
        w = random.choice(W)
        r = random.random()
        if r < 0.06: w = '<a href="/wiki/%s">%s %s</a>' % (w, w, random.choice(W))
        elif r < 0.08: w = '<b>%s</b>' % w
        elif r < 0.09: w = '<i>%s</i>' % w
        out.append(w)
    s = " ".join(out)
    return s[0].upper() + s[1:] + "."
print('<!doctype html><html><head><meta charset="utf-8"><title>Long article</title></head><body><main>')
print('<h1>History of the long article</h1>')
for sec in range(40):
    print('<h2>Section %d</h2>' % (sec + 1))
    for p in range(6):
        print('<p>%s</p>' % para(random.randint(40, 120)))
    if sec % 3 == 0:
        print('<ul>' + ''.join('<li>%s</li>' % para(12) for _ in range(4)) + '</ul>')
    if sec % 5 == 0:
        print('<blockquote><p>%s</p></blockquote>' % para(30))
print('</main></body></html>')
