#!/bin/sh
# cpu_freq.sh - helper for MaixCAM Pro (SG2002) CPU frequency control
#
# usage:
#   ./cpu_freq.sh status              show current PLL / rate / temperature
#   ./cpu_freq.sh set 900000000       set MPLL to 900 MHz
#   ./cpu_freq.sh set 850000000       set MPLL back to 850 MHz
#   ./cpu_freq.sh restore             set 850 MHz and unload the module
#
# environment:
#   MOD   path to mpll_freq.ko (default: /root/cpu_tool/mpll_freq.ko)
set -e

MOD=${MOD:-/root/cpu_tool/mpll_freq.ko}

status() {
    echo "MPLL_CSR : $(devmem 0x03002908 32 2>/dev/null || echo '(devmem missing)')"
    echo "clk_rate : $(cat /sys/kernel/debug/clk/clk_c906_0/clk_rate 2>/dev/null)"
    echo "temp     : $(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null) (milli-degC)"
    if grep -q mpll_freq /proc/modules; then
        echo "module   : $(grep mpll_freq /proc/modules)"
    else
        echo "module   : not loaded"
    fi
}

apply() {
    rate=$1
    if [ -z "$rate" ]; then
        echo "usage: $0 set <hz>   e.g. 900000000" >&2
        exit 1
    fi
    if ! [ -f "$MOD" ]; then
        echo "error: $MOD not found" >&2
        exit 1
    fi
    rmmod mpll_freq 2>/dev/null || true
    insmod "$MOD" target="$rate" delay_ms=1500
    sleep 3
    dmesg | grep mpll_freq | tail -5
    echo
    status
}

case "$1" in
    status)  status ;;
    set)     apply "$2" ;;
    restore) apply 850000000; rmmod mpll_freq 2>/dev/null || true; echo "module unloaded (frequency stays)"; ;;
    *)       echo "usage: $0 {status|set <hz>|restore}" >&2; exit 1 ;;
esac
