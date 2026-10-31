#!/bin/sh
# Phase-2 step 1: read-only display-pipe dump on the phone. Writes nothing to any register.
# Run on the phone:  sh /tmp/disp/stage0.sh   -> /tmp/disp-stage0.txt (pull it with bin/ph "cat ...")
# Each step is a separate process and output is flushed per register, so if a read ever hangs the
# log shows the last good register.
D=/tmp/disp
O=/tmp/disp-stage0.txt
[ -e /dev/mem ] || mknod /dev/mem c 1 1
{
echo "== $(date) $(uname -r)"
echo "== chosen/framebuffer"; for f in /proc/device-tree/chosen/framebuffer*/; do echo "$f"; for p in reg width height stride; do printf "  %s: " $p; od -An -tx1 "$f/$p" | tr -d '\n'; echo; done; printf "  format: "; cat "$f/format"; echo; done
echo "== /proc/iomem (top)"; grep -i -E "system ram|framebuffer|83e9" /proc/iomem
echo "== drm"; ls -l /sys/class/drm/ 2>&1 | head; cat /proc/interrupts | grep -E " 103 |CPU" | head -3
echo "== disp pm";      $D/disp pm      || exit 1
echo "== disp regs 1";  $D/disp regs 1  || exit 1
echo "== disp regs 2";  $D/disp regs 2  || exit 1
echo "== disp count";   $D/disp count 1000
echo "== disp walk";    $D/disp walk
echo "== done"
} 2>&1 | tee $O
