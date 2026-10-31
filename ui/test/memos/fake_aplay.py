#!/usr/bin/env python3
"""Stand-in for /usr/bin/aplay in test/memos/run.sh (no ALSA touched).
Recording (args have -D): 0.5 s of 2-ch S16_LE at once (ch1 = 1000, ch2 = -2000), then 0.5 s more each
0.1 s until SIGINT, then exit 0 (MEMOS_REC_XRUN=1: two overrun lines on stderr). Playback: reads stdin until EOF, appends the byte count + args to $MEMOS_LOG."""
import os, signal, struct, sys, time

log = os.environ.get("MEMOS_LOG", "/dev/null")
args = sys.argv[1:]
if "-D" in args:
	stop = False
	def on_int(*_):
		global stop
		stop = True
	signal.signal(signal.SIGINT, on_int)
	with open(log, "a") as f:
		f.write("arecord " + " ".join(args) + "\n")
	if os.environ.get("MEMOS_REC_FAIL"):
		sys.stderr.write("arecord: main:850: audio open error: Device or resource busy\n")
		sys.exit(1)
	frame = struct.pack("<hh", 1000, -2000)
	out = sys.stdout.buffer
	out.write(frame * 24000)
	out.flush()
	if os.environ.get("MEMOS_REC_XRUN"):	# what aplay prints after an ALSA capture overrun
		for _ in range(2):
			sys.stderr.write("overrun!!! (at least 12.345 ms long)\n")
		sys.stderr.flush()
	while not stop:
		time.sleep(0.1)
		if not stop:
			out.write(frame * 4800)
			out.flush()
	sys.exit(0)
n = 0
while True:
	b = sys.stdin.buffer.read(4096)
	if not b:
		break
	n += len(b)
with open(log, "a") as f:
	f.write("aplay %s bytes=%d\n" % (" ".join(args), n))
