#!/bin/bash
# Favicon screenshots: test/web/icons.sh OUTDIR [B] [SNAP] — serves each site under icons/ on its own port of
# 127.0.0.1 (a port is a site of its own for web_icon.c), visits each page once with orchard-snap (B's build, or SNAP) so web_icon.c looks
# its icon up, then shoots the start page (Favorites grid + Recently Visited) in the user theme and the dark fallback.
set -e
here=$(cd "$(dirname "$0")" && pwd); ui=$(cd "$here/../.." && pwd)
out=${1:?outdir}; B=${2:-build}; mkdir -p "$out"
snap=${3:-$ui/$B/snap/orchard-snap}
theme=${THEME_DIR:-$(cd "$(dirname "$0")/../.." && pwd)/build/theme}
port=$((20000 + RANDOM % 20000))
sites="touch png ico old none"
srv=; i=0
for s in $sites; do
	python3 -m http.server -b 127.0.0.1 -d "$here/icons/$s" $((port + i)) >/dev/null 2>&1 & srv="$srv $!"
	i=$((i + 1))
done
trap 'kill $srv' EXIT
sleep 0.7
data=$(mktemp -d); printf 'start_locked=0\n' > "$data/prefs"; mkdir -p "$data/web"
: > "$data/web/favorites.txt"; i=0
for s in $sites; do printf 'http://127.0.0.1:%d/\t%s\n' $((port + i)) "$s" >> "$data/web/favorites.txt"; i=$((i + 1)); done
export ORCHARD_DATA=$data ORCHARD_WEB_ALLOW_LOOPBACK=1 TZ=EST5EDT
i=0
for s in $sites; do
	[ -n "$NOVISIT" ] && break
	ORCHARD_WEB_URL=http://127.0.0.1:$((port + i))/ "$snap" open Web wait 8000 2>>"$out/log.txt"
	i=$((i + 1))
done
for t in theme dark; do
	env=(); [ $t = theme ] && env=(ORCHARD_THEME=$theme)
	env "${env[@]}" "$snap" open Web wait 1500 shot "$out/$t-start.ppm" drag 375 1100 375 500 wait 1500 shot "$out/$t-start-2.ppm" 2>>"$out/log.txt"
	for f in "$out/$t"-start*.ppm; do magick "$f" -resize ${SCALE:-50}% "${f%.ppm}.png" && rm "$f"; done
done
ls -la "$data/web/icons" >> "$out/log.txt" 2>&1 || true
rm -rf "$data"
