#!/usr/bin/env python3
"""A8 NCO register values, computed the way iOS 12 AppleS5L8960XNCO does (fn 0x...833a5c0).
usage: nco.py PARENT_HZ RATE_HZ [CHANNEL]  -> prints `mem w` commands for tools/mem
PARENT_HZ may be a fraction: iOS computes with 2400000000/7 (342.857 MHz) exactly, i.e. both rates x7.
INC1/INC2 then match iOS bit for bit; an integer parent gives the same DIV and the same INC1/(INC1-INC2)."""
from fractions import Fraction
import sys

NCO_BASE, STRIDE = 0x20E011000, 0x1000


def regs(parent, rate):
    two_p = 2 * parent
    div, rem = divmod(two_p, rate)
    steps = (div >> 2) - 3
    if steps == 0:
        coarse = 0x1FFC
    else:
        s = 0x7FF
        for _ in range(steps):
            s = (s << 1) ^ 0xA01 if s & 0x400 else s << 1
        coarse = s << 2
    return {
        0x4: coarse | (div & 3),             # DIV
        0x8: rem,                            # INC1
        0xC: (rem - rate) & 0xFFFFFFFF,      # INC2
        0x10: ((rem ^ 0x80000000) - rate) & 0xFFFFFFFF,  # ACCINIT
        0x0: 0x80001000,                     # CTRL: enable | bit12 (as iOS)
    }


if __name__ == "__main__":
    parent, rate = Fraction(sys.argv[1]), Fraction(sys.argv[2])
    scale = parent.denominator * rate.denominator
    parent, rate = int(parent * scale), int(rate * scale)
    ch = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    base = NCO_BASE + ch * STRIDE
    print("w %x 0" % base, end=" ")  # disable before reprogramming
    print(" ".join("w %x %x" % (base + off, val) for off, val in regs(parent, rate).items()))
