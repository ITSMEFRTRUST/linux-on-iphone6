#!/usr/bin/env python3
"""Orchard's app icons: writes one SVG per app next to this file (120×120, squircle included).
Own designs (no Apple artwork). One system: a superellipse tile with a soft vertical gradient,
a crisp white (or dark, on the light tiles) symbol with round caps, ~6 px strokes at 120 px.
Run it after changing a design; the Makefile renders the SVGs into raw BGRA for assets.S."""
import math, os

D = os.path.dirname(os.path.abspath(__file__))
N = 5.0		# superellipse exponent (≈ iOS continuous corners)


def squircle(s=120, n=N, pts=240):
	r = s / 2
	out = []
	for i in range(pts):
		t = 2 * math.pi * i / pts
		c, si = math.cos(t), math.sin(t)
		x = r + r * math.copysign(abs(c) ** (2 / n), c)
		y = r + r * math.copysign(abs(si) ** (2 / n), si)
		out.append(f"{x:.2f},{y:.2f}")
	return "M" + " L".join(out) + " Z"


TILE = squircle()


def icon(name, top, bottom, body, extra_defs=""):
	svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="120" height="120" viewBox="0 0 120 120">
<defs>
<linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{top}"/><stop offset="1" stop-color="{bottom}"/></linearGradient>
<linearGradient id="sheen" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity="0.16"/><stop offset="0.5" stop-color="#fff" stop-opacity="0"/></linearGradient>
<clipPath id="tile"><path d="{TILE}"/></clipPath>
{extra_defs}
</defs>
<g clip-path="url(#tile)">
<rect width="120" height="120" fill="url(#bg)"/>
{body}
<rect width="120" height="120" fill="url(#sheen)"/>
</g>
</svg>
"""
	with open(os.path.join(D, name + ".svg"), "w") as f:
		f.write(svg)


def polar(cx, cy, r, deg):
	a = math.radians(deg - 90)
	return cx + r * math.cos(a), cy + r * math.sin(a)


# ---- Clock: white face on black, 10:09, orange second hand
ticks = []
for i in range(60):
	big = i % 5 == 0
	x1, y1 = polar(60, 60, 44, i * 6)
	x2, y2 = polar(60, 60, 44 - (7 if big else 3.5), i * 6)
	ticks.append(f'<line x1="{x1:.2f}" y1="{y1:.2f}" x2="{x2:.2f}" y2="{y2:.2f}" stroke="#1c1c1e" '
		     f'stroke-width="{2.6 if big else 1}" stroke-linecap="round"/>')
hx, hy = polar(60, 60, 24, 10 * 30 + 9 * 0.5)
mx, my = polar(60, 60, 37, 9 * 6)
sx, sy = polar(60, 60, 40, 180 + 36)
tx, ty = polar(60, 60, 10, 36)
icon("clock", "#2c2c2e", "#000000", f"""
<circle cx="60" cy="60" r="49" fill="#f9f9f9"/>
{''.join(ticks)}
<line x1="60" y1="60" x2="{hx:.2f}" y2="{hy:.2f}" stroke="#1c1c1e" stroke-width="5.5" stroke-linecap="round"/>
<line x1="60" y1="60" x2="{mx:.2f}" y2="{my:.2f}" stroke="#1c1c1e" stroke-width="3.6" stroke-linecap="round"/>
<line x1="{tx:.2f}" y1="{ty:.2f}" x2="{sx:.2f}" y2="{sy:.2f}" stroke="#ff9500" stroke-width="1.8" stroke-linecap="round"/>
<circle cx="60" cy="60" r="3.6" fill="#ff9500"/><circle cx="60" cy="60" r="1.4" fill="#f9f9f9"/>""")


# ---- Notes: yellow header, ruled page
lines = "".join(f'<line x1="20" y1="{y}" x2="100" y2="{y}" stroke="#d6d6db" stroke-width="2.2" stroke-linecap="round"/>'
		for y in (56, 72, 88, 104))
icon("notes", "#ffffff", "#f1f1f4", f"""
<rect width="120" height="38" fill="url(#ny)"/>
<rect y="37" width="120" height="2" fill="#000" opacity="0.06"/>
{lines}""", '<linearGradient id="ny" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffe066"/>'
	    '<stop offset="1" stop-color="#ffc800"/></linearGradient>')

# ---- Calculator: graphite, three grey keys + an orange "="
def key(cx, cy, fill, sym):
	return f'<circle cx="{cx}" cy="{cy}" r="19" fill="{fill}"/>' + sym
w = 'stroke="#fff" stroke-width="4.6" stroke-linecap="round"'
icon("calculator", "#48484a", "#1c1c1e",
     key(38, 38, "#7c7c80", f'<line x1="30" y1="38" x2="46" y2="38" {w}/><line x1="38" y1="30" x2="38" y2="46" {w}/>') +
     key(82, 38, "#7c7c80", f'<line x1="74" y1="38" x2="90" y2="38" {w}/>') +
     key(38, 82, "#7c7c80", f'<line x1="32" y1="76" x2="44" y2="88" {w}/><line x1="44" y1="76" x2="32" y2="88" {w}/>') +
     key(82, 82, "#ff9f0a", f'<line x1="74" y1="78" x2="90" y2="78" {w}/><line x1="74" y1="86" x2="90" y2="86" {w}/>'))

# ---- Web: blue, a white globe (outline, one meridian, equator + two parallels)
pt = lambda p: f"{p[0]:.2f},{p[1]:.2f}"
icon("web", "#5ad8ff", "#1559f5", """
<g fill="none" stroke="#fff" stroke-linecap="round" stroke-linejoin="round">
<circle cx="60" cy="60" r="36" stroke-width="6.5"/>
<ellipse cx="60" cy="60" rx="15" ry="36" stroke-width="5"/>
<path d="M25 60 H95" stroke-width="5"/>
<path d="M31 41 Q60 49 89 41 M31 79 Q60 71 89 79" stroke-width="4.2" opacity="0.9"/>
</g>""")

# ---- Weather: sky gradient, sun behind a cloud
icon("weather", "#5ac8fa", "#1a73e8", """
<circle cx="47" cy="46" r="19" fill="url(#sun)"/>
<g fill="#fff">
<circle cx="52" cy="76" r="15"/><circle cx="72" cy="66" r="19"/><circle cx="90" cy="78" r="12"/>
<rect x="37" y="74" width="65" height="20" rx="10"/>
</g>""", '<linearGradient id="sun" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffe14d"/>'
	 '<stop offset="1" stop-color="#ffb300"/></linearGradient>')

# ---- Terminal: near-black, white chevron + green cursor
icon("terminal", "#2c2c2e", "#0b0b0c", """
<polyline points="30,46 46,60 30,74" fill="none" stroke="#fff" stroke-width="7" stroke-linecap="round" stroke-linejoin="round"/>
<line x1="56" y1="76" x2="84" y2="76" stroke="#30d158" stroke-width="7" stroke-linecap="round"/>""")

# ---- Music: pink, white beamed eighth notes
icon("music", "#ff6a8a", "#e0245e", """
<g fill="#fff">
<ellipse cx="40" cy="84" rx="12.5" ry="9.5" transform="rotate(-22 40 84)"/>
<ellipse cx="82" cy="76" rx="12.5" ry="9.5" transform="rotate(-22 82 76)"/>
<rect x="46.5" y="34" width="6" height="51"/>
<rect x="88.5" y="26" width="6" height="51"/>
<polygon points="46.5,34 94.5,24 94.5,36 46.5,46"/>
</g>""")

# ---- Files: blue, white folder (back tab softer)
icon("files", "#5eb3ff", "#1a6dff", """
<path d="M24 38 Q24 30 32 30 L50 30 Q53 30 55 32.5 L59 37 L88 37 Q96 37 96 45 L96 52 L24 52 Z" fill="#fff" opacity="0.72"/>
<rect x="24" y="44" width="72" height="48" rx="8" fill="#fff"/>""")

# ---- Settings: grey, light gear with a dark hub
teeth, R1, R2, T = [], 43, 33, 10
for i in range(T):
	c = i * 360 / T
	for deg, r in ((c - 13, R2), (c - 8, R1), (c + 8, R1), (c + 13, R2)):
		teeth.append(pt(polar(60, 60, r, deg)))
icon("settings", "#a1a1a6", "#636366", f"""
<polygon points="{' '.join(teeth)}" fill="url(#gear)"/>
<circle cx="60" cy="60" r="22" fill="#56565b"/>
<circle cx="60" cy="60" r="13" fill="url(#gear)"/>
<circle cx="60" cy="60" r="6" fill="#56565b"/>""",
     '<linearGradient id="gear" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fbfbfd"/>'
     '<stop offset="1" stop-color="#d8d8de"/></linearGradient>')

# ---- Camera: light grey, near-black camera body, glass lens, a yellow ready light
icon("camera", "#e5e5ea", "#a1a1a6", """
<path d="M45 33.5 Q47 28 53 28 L67 28 Q73 28 75 33.5 L76.5 37 L87 37 Q97 37 97 47 L97 81 Q97 91 87 91 L33 91 Q23 91 23 81 L23 47 Q23 37 33 37 L43.5 37 Z" fill="#1c1c1e"/>
<circle cx="60" cy="63" r="20" fill="url(#ring)"/>
<circle cx="60" cy="63" r="14.5" fill="#1c1c1e"/>
<circle cx="60" cy="63" r="9" fill="url(#glass)"/>
<circle cx="55.5" cy="58.5" r="3" fill="#fff" opacity="0.55"/>
<circle cx="86" cy="47" r="3.5" fill="#ffd60a"/>""",
     '<linearGradient id="ring" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f2f2f7"/>'
     '<stop offset="1" stop-color="#aeaeb2"/></linearGradient>'
     '<radialGradient id="glass" cx="0.4" cy="0.35" r="0.8"><stop offset="0" stop-color="#3a4d6b"/>'
     '<stop offset="1" stop-color="#0b0f17"/></radialGradient>')

# ---- Voice Memos: near-black, a white waveform of round-capped bars, a red playhead
heights = [10, 22, 36, 18, 48, 30, 56, 26, 40, 16, 30, 12]
bars = []
for i, h in enumerate(heights):
	x = 22 + i * 7 + (4 if i >= 6 else 0)	# a gap for the playhead
	bars.append(f'<line x1="{x}" y1="{60 - h / 2:.1f}" x2="{x}" y2="{60 + h / 2:.1f}" stroke="#fff" '
		    f'stroke-width="4.2" stroke-linecap="round" opacity="{0.55 if i >= 6 else 1}"/>')
icon("voicememos", "#2c2c2e", "#0b0b0c", "".join(bars) + """
<line x1="62" y1="26" x2="62" y2="94" stroke="#ff453a" stroke-width="3" stroke-linecap="round"/>
<circle cx="62" cy="26" r="5" fill="#ff453a"/>""")
