#!/bin/bash
# Web reader screenshots of the fixtures here: test/web/shoot.sh OUTDIR [B] — serves this dir on 127.0.0.1 and runs
# orchard-snap (B's snap build) on each page, in the user theme (build/theme) and the built-in dark fallback.
set -e
here=$(cd "$(dirname "$0")" && pwd); ui=$(cd "$here/../.." && pwd)
out=${1:?outdir}; B=${2:-build}; mkdir -p "$out"
snap=$ui/$B/snap/orchard-snap
theme=${THEME_DIR:-$(cd "$(dirname "$0")/../.." && pwd)/build/theme}
port=$((20000 + RANDOM % 20000))
python3 -m http.server -b 127.0.0.1 -d "$here" $port >/dev/null 2>&1 & srv=$!
trap 'kill $srv' EXIT
sleep 0.5
data=$(mktemp -d); printf 'start_locked=0\n' > "$data/prefs"
pages=${PAGES:-"article forum login docs long"}
for t in theme dark; do
	for p in $pages; do
		env=(ORCHARD_DATA=$data ORCHARD_WEB_ALLOW_LOOPBACK=1 TZ=EST5EDT ORCHARD_WEB_URL=http://127.0.0.1:$port/$p.html)
		[ $t = theme ] && env+=(ORCHARD_THEME=$theme)
		args=(open Web wait 25000 shot "$out/$t-$p-1.ppm")
		for k in $(seq 2 ${SCREENS:-4}); do	# one screen down (900 px, finger held still before lifting: no fling)
			for y in $(seq 1150 -100 250); do args+=(touch 375 $y); done
			args+=(wait 300 lift wait 6000 shot "$out/$t-$p-$k.ppm")
		done
		env "${env[@]}" "$snap" "${args[@]}" 2>>"$out/log.txt"
		for f in "$out/$t-$p"-*.ppm; do magick "$f" -resize 50% "${f%.ppm}.png" && rm "$f"; done
	done
done
rm -rf "$data"
