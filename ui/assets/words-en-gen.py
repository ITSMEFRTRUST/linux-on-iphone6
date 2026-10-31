#!/usr/bin/env python3
"""Regenerate words-en.txt (the keyboard's English word model, most frequent first) from wordfreq
(https://github.com/rspeer/wordfreq, code Apache-2.0, frequency data CC BY-SA 4.0; this list is a derived work of
that data and is under CC BY-SA 4.0 too). usage: pip install wordfreq==3.1.1; words-en-gen.py > words-en.txt"""
import re
import wordfreq

N = 9884
# profanity and proper names left out of suggestions
BLOCK = set("""fuck fucking fucked fucks shit shitty bullshit bitch bitches cunt pussy asshole bastard porn nigga nigger
fag faggot slut whore cock tits boobs motherfucker dildo horny milf cum piss retard retarded hoe thot negro wtf lmao
dick sex sexy anal rape nude naked penis vagina xxx damn crap""".split())
out = []
for w in wordfreq.top_n_list("en", 40000):
	if re.fullmatch(r"[a-z]+", w) and w not in BLOCK and (len(w) > 1 or w in ("a", "i")):
		out.append(w)
	if len(out) == N:
		break
print("\n".join(out))
