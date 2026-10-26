#!/bin/sh
# build.sh - build mpll_freq.ko against a Linux 5.10.4 kernel tree
#
#   ./build.sh /path/to/linux-5.10.4 [riscv64-linux-gnu-]
#
# The kernel tree must be configured for the target device:
#   - 5.10.4 with EXTRAVERSION = -tag-   (so vermagic is "5.10.4-tag- ...")
#   - .config imported from the device:  zcat /proc/config.gz > .config
#   - modules_prepare done once:
#       make ARCH=riscv CROSS_COMPILE=... olddefconfig modules_prepare
#
# Tested toolchain: Ubuntu 22.04 gcc-riscv64-linux-gnu 11.4.0
# (the vendor kernel was built with Xuantie GCC 10.2; both work, vermagic does
#  not encode the compiler version)
set -e

KDIR=${1:-}
CROSS=${2:-riscv64-linux-gnu-}
HERE=$(cd "$(dirname "$0")" && pwd)

if [ -z "$KDIR" ]; then
    echo "usage: $0 <kernel-tree> [cross-prefix]" >&2
    exit 1
fi
if [ ! -f "$KDIR/Makefile" ]; then
    echo "error: $KDIR does not look like a kernel tree" >&2
    exit 1
fi

make -C "$KDIR" M="$HERE/src" ARCH=riscv CROSS_COMPILE="$CROSS" modules

mkdir -p "$HERE/prebuilt"
cp "$HERE/src/mpll_freq.ko" "$HERE/prebuilt/mpll_freq.ko"

echo
echo "built: $HERE/prebuilt/mpll_freq.ko"
echo "vermagic:"
grep -a -o 'vermagic=[^ ]*' "$HERE/prebuilt/mpll_freq.ko" || true
echo
echo "expected on device: 5.10.4-tag- preempt mod_unload riscv64"
echo "upload: scp prebuilt/mpll_freq.ko root@<device>:/root/cpu_tool/"
