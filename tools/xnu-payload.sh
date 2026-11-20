#!/bin/bash
# Build the XNUP payload for lancelot: build-payload.bin, the kernel and (optionally) a small root
# image that rides in the boot image as a RAM disk. mtkloader packs it in front of Android's kernel
# (mtkloader/build_flashed.py), and the run is tools/xnu-smoke.sh (or tools/xnu-cycle.sh).
#
# The kernel is the one the build tree just made; a kernel-only payload is enough to get XNU running
# and stopping at IOFindBSDRoot. With userland/root.apfs present it travels as the RAM disk and, when
# the loader is built with -DXNU_ROOT_EMMC=0, becomes /dev/md0 -- the root, no `xnusys` needed.
#
#   XL_RAMDISK   the image to ride in the payload. Default: userland/root.apfs when it exists, else
#                none (kernel only). Set it empty for a kernel-only payload on purpose.
#   XL_CMDLINE   payload cmdline (default: empty; what matters are the loader's own boot-args)
#   XNU_KERNEL   the kernel to pack (default: the build tree's mach.development.bcm2837)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
K=${XNU_KERNEL:-$ROOT/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837}
[ -f "$K" ] || K="$ROOT/prebuilt/mach.development.bcm2837"
[ -f "$K" ] || { echo "xnu-payload.sh: no kernel at $K (build it first: xnu-build.sh)"; exit 1; }
RD=${XL_RAMDISK-$ROOT/userland/root.apfs}
command -v python3 >/dev/null || { echo "xnu-payload.sh: python3 is not in PATH"; exit 1; }
# The image is only as new as the root it packs, and the driver in it is the one userland/build.sh
# copied out of what tools/mk-mesa.sh built (lib/libOSMesa.8.dylib). Building Mesa on its own and
# then packing leaves the previous driver in the image without a word -- a run then measures the old
# one (18.11.2026: 8 minutes, "redwood draw" without the fields the new build prints).
MESA=$ROOT/xnu-deps/build/mesa/out/libOSMesa.8.dylib
if [ -n "$RD" ] && [ -f "$RD" ] && [ -f "$MESA" ] && [ "$MESA" -nt "$RD" ]; then
	echo "xnu-payload.sh: $MESA is newer than $RD" >&2
	echo "xnu-payload.sh: run userland/build.sh first, or the image carries the previous driver" >&2
fi
ARGS=("kernel=$K")
if [ -n "$RD" ] && [ -f "$RD" ]; then
	ARGS+=("ramdisk=$RD")
	echo "xnu-payload.sh: kernel + ramdisk ($(wc -c < "$RD") bytes)"
else
	[ -n "$RD" ] && echo "xnu-payload.sh: no $RD yet: kernel-only payload (XNU will stop at IOFindBSDRoot)"
fi
[ -n "${XL_CMDLINE:-}" ] && ARGS+=("--cmdline=$XL_CMDLINE")
python3 "$ROOT/xnuloader/mkpayload.py" -o "$ROOT/build-payload.bin" "${ARGS[@]}"
ls -l "$ROOT/build-payload.bin"
