#!/bin/bash
# One XNU run on lancelot that needs no trigger: the loader is built with -DMENU_DEFAULT_XNU=1, so the
# first boot after the flash goes straight into XNU (no Android before it). XNU runs, returns to
# mtkloader, which saves its log into ramoops and resets; the next boot puts Android up with that log
# waiting in pstore.
#
# Why not tools/xnu-cycle.sh: its one-shot request is a line written to /dev/pmsg0 under Android, and
# on this phone that write never reaches ramoops (rc=0, no pmsg-ramoops-0 record, and the loader still
# prints "boot: Android" on the next boot). Until that trigger works, this is the run that actually
# starts XNU. Both are the same build/pack/flash; only the choice of XNU differs. See
# docs/HAKIOS-BUILD.md, "Первый запуск на lancelot".
#
#   build mtkloader with -DMENU_DEFAULT_XNU=1 (extra args go to the compiler, e.g. -DXNU_CHECKPOINT=17)
#   -> pack it in front of Android's own kernel plus the XNU payload (mtkloader/build_flashed.py)
#   -> flash `boot` (A-only: one slot, no `fastboot boot` on this LK)
#   -> reboot: XNU first, then Android comes back with the log
#   -> read the log from pstore.
#
# With no trigger, *every* cold boot runs XNU once before Android. The way back to plain Android is
# the same `boot`: `fastboot flash boot "$ANDROID_BOOT"`.
set -e
S="${HAKIOS_SERIAL:?set HAKIOS_SERIAL to your phone adb serial}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${XNU_LOG_OUT:-$ROOT/logs/xnu-smoke.log}
# The phone's own rooted boot image: mtkloader is chained in front of the Android kernel already in
# it, and it is also the way back. Never in the repository, so it usually has to be pointed at.
ANDROID_BOOT=${ANDROID_BOOT:-$ROOT/backup/boot-magisk-patched.img}
# On this host adb/fastboot are Windows programs; under WSL they are reached as adb.exe/fastboot.exe
# (ADB/FASTBOOT override). fastboot.exe must be given the image as a Windows path.
ADB=${ADB:-adb}
FASTBOOT=${FASTBOOT:-fastboot}
command -v "$ADB" >/dev/null 2>&1 || { command -v adb.exe >/dev/null 2>&1 && ADB=adb.exe; }
command -v "$FASTBOOT" >/dev/null 2>&1 || { command -v fastboot.exe >/dev/null 2>&1 && FASTBOOT=fastboot.exe; }
# adb.exe under WSL ends its lines with CR: strip it, or "device" never equals "device".
state() { "$ADB" -s $S get-state 2>/dev/null | tr -d '\r'; }
# XNU settings: watchdog off, one watcher core, the kernel comes back by itself after SECS seconds.
SECS=${XNU_SECS:-40}
CORES=${XNU_CORES:-1}
WATCH=${XNU_WATCH:-$((SECS + 240))}
XNU_ARGS=${XNU_ARGS:-cpus=$CORES}
# Where XNU's root is: 1 (default) the system image on the eMMC (rd=md0 emmc_root=1, needs the xnusys
# partition); 0 the payload's own RAM disk as /dev/md0 (rd=md0), which is the way to boot a root
# before xnusys exists -- build the payload with tools/xnu-payload.sh (ramdisk=userland/root.apfs).
ROOT_EMMC=${XNU_ROOT_EMMC:-1}
command -v python3 >/dev/null || { echo "xnu-smoke.sh: python3 is not in PATH"; exit 1; }
[ -f "$ROOT/build-payload.bin" ] || { echo "xnu-smoke.sh: no build-payload.bin: make one with xnuloader/mkpayload.py"; exit 1; }
[ -f "$ANDROID_BOOT" ] || { echo "xnu-smoke.sh: no $ANDROID_BOOT (set ANDROID_BOOT to your phone's rooted boot image)"; exit 1; }
mkdir -p "$(dirname "$OUT")"
"$ROOT/mtkloader/build.sh" -DMENU_DEFAULT_XNU=1 -DXNU_ROOT_EMMC=$ROOT_EMMC -DXNU_WDT_SECONDS=0 -DXNU_CORES=$CORES \
	-DXNU_WATCH_SECONDS=$WATCH "-DXNU_RETURN_AFTER=\"$SECS\"" "-DXNU_EXTRA_ARGS=\"$XNU_ARGS epoch=$(date +%s)\"" \
	"$@" >/dev/null
python3 "$ROOT/mtkloader/build_flashed.py" "$ANDROID_BOOT" >/dev/null
IMG="$ROOT/xnu-flashed-slot_a.img"
# fastboot.exe (Windows) needs a Windows path; fastboot (Linux) takes the file as it is.
IMG_ARG="$IMG"
case "$FASTBOOT" in *.exe) command -v wslpath >/dev/null 2>&1 && IMG_ARG=$(wslpath -w "$IMG") ;; esac
[ "$(state)" = device ] && "$ADB" -s $S reboot bootloader
for i in $(seq 1 120); do "$FASTBOOT" devices | grep -q $S && break; sleep 1; done
"$FASTBOOT" devices | grep -q $S || { echo "the phone did not come up in fastboot"; exit 1; }
"$FASTBOOT" -s $S flash boot "$IMG_ARG" >/dev/null
echo "boot flashed; rebooting straight into XNU (no Android first)"
# A USB-level "usb_read failed" from reboot does not mean the phone stayed in fastboot: it has
# already gone. Only a phone that is still there needs the failure to be reported.
"$FASTBOOT" -s $S reboot >/dev/null 2>&1 || true
sleep 3
"$FASTBOOT" devices | grep -q $S && { echo "the phone is still in fastboot after reboot"; exit 1; }
echo "waiting for Android to come back (XNU runs first, then mtkloader resets)..."
for i in $(seq 1 $(( (SECS + 420) / 3 ))); do [ "$(state)" = device ] && break; sleep 3; done
sleep 6
# lancelot: `su` is not on PATH in adb shell; Magisk's is /debug_ramdisk/su (HAKIOS_SU overrides).
SU=${HAKIOS_SU:-}
if [ -z "$SU" ]; then
	for c in /debug_ramdisk/su su /system/bin/su; do
		if "$ADB" -s "$S" shell "$c -c id" 2>/dev/null | grep -q 'uid=0'; then SU="$c"; break; fi
	done
fi
[ -n "$SU" ] || { echo "xnu-smoke.sh: no working su on the phone (set HAKIOS_SU)"; exit 1; }
for i in $(seq 1 10); do "$ADB" -s $S shell "$SU -c 'cat /sys/fs/pstore/console-ramoops-0'" > "$OUT" 2>/dev/null && [ -s "$OUT" ] && break; sleep 2; done
"$ADB" -s $S shell "$SU -c 'ls -l /sys/fs/pstore/'" 2>/dev/null | tr -d '\r' || true
# what the kernel kept for the host (kern.redwood.host_keep, bsd/dev/redwood_mali.c), which mtkloader
# put into the pmsg zone: appended to the log, when it is this run's and not Android's own
"$ADB" -s $S shell "$SU -c 'cat /sys/fs/pstore/pmsg-ramoops-0'" > "$OUT.keep" 2>/dev/null || true
if head -c 64 "$OUT.keep" | grep -q 'kept for the host by the kernel'; then
	{ echo; cat "$OUT.keep"; } >> "$OUT"
	echo "kept for the host: $(head -1 "$OUT.keep" | sed 's/^=== kept for the host by the kernel: //; s/ ===$//')"
fi
rm -f "$OUT.keep"
echo "log: $OUT ($(wc -c < "$OUT") bytes)"
