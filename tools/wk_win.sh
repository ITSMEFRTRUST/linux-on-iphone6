#!/bin/sh
# wk_win.sh SECS TAG - wakeup window: per-CPU /proc/interrupts + /proc/softirqs deltas + top threads by ctx switches
T=${1:-10}; TAG=${2:-win}
th() { for t in /proc/[0-9]*/task/[0-9]*; do s=$(grep -E '^(voluntary|nonvoluntary)_ctxt' $t/status 2>/dev/null | awk '{s+=$2} END{print s}'); [ -n "$s" ] && echo "${t#/proc/} $s $(cat $t/comm 2>/dev/null | tr ' ' _)"; done; }
th > /tmp/wk_t1; cat /proc/interrupts > /tmp/wk_i1; cat /proc/softirqs > /tmp/wk_s1; t0=$(cut -d' ' -f1 /proc/uptime)
sleep $T
cat /proc/interrupts > /tmp/wk_i2; cat /proc/softirqs > /tmp/wk_s2; t1=$(cut -d' ' -f1 /proc/uptime); th > /tmp/wk_t2
echo "== $TAG dt=$(awk "BEGIN{print $t1-$t0}") lim=$(cat /sys/class/power_supply/sn2400-charger/input_current_limit) st=$(cat /sys/class/power_supply/battery/status)"
awk 'NR==FNR{if($1~/:$/){a[$1]=$2;b[$1]=$3}; next} $1~/:$/{d0=$2-a[$1]; d1=$3-b[$1]; if(d0+d1>0){ $2="";$3=""; printf "  irq %-6s %6d %6d %s\n",$1,d0,d1,$0}}' /tmp/wk_i1 /tmp/wk_i2 | sed 's/  */ /g'
awk 'NR==FNR{a[$1]=$2;b[$1]=$3; next} $1~/:$/{d0=$2-a[$1]; d1=$3-b[$1]; if(d0+d1>0) printf "  sirq %-8s %6d %6d\n",$1,d0,d1}' /tmp/wk_s1 /tmp/wk_s2
awk 'NR==FNR{a[$1]=$2; next} ($1 in a){w=$2-a[$1]; if(w>2) printf "  %6d cs %s %s\n", w, $1, $3}' /tmp/wk_t1 /tmp/wk_t2 | sort -k1 -n -r | head -20
