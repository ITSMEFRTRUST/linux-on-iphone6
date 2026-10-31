#!/bin/sh
# pa_cpu.sh SECONDS - per-process and per-thread CPU ticks (utime+stime, 1/100 s) over SECONDS, plus wakeup-ish counters
T=${1:-30}
snap() { for t in /proc/[0-9]*/task/[0-9]*; do [ -r $t/stat ] || continue; s=$(cat $t/stat 2>/dev/null) || continue; r=${s#*) }; set -- $r; echo "${t#/proc/} $(( ${12} + ${13} )) $(cat $t/comm 2>/dev/null) $(grep -E '^(voluntary|nonvoluntary)_ctxt' $t/status 2>/dev/null | awk '{s+=$2} END{print s}')"; done; }
snap > /tmp/pa_p1; sleep $T; snap > /tmp/pa_p2
awk 'NR==FNR{a[$1]=$2; c[$1]=$4; next} ($1 in a){d=$2-a[$1]; w=$4-c[$1]; if (d>0||w>3) printf "%5d ticks %6d ctxsw  %s %s\n", d, w, $1, $3}' /tmp/pa_p1 /tmp/pa_p2 | sort -k3 -n -r | head -30
