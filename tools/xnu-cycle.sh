#!/bin/bash
# One XNU run on the Xiaomi Redmi 9 (lancelot). This is the A-only cycle: the phone has a single
# boot partition (no boot_a/boot_b, no current-slot), and its LK has no `fastboot boot`, so the
# image is always written into `boot` and Android brings the log back afterwards.
#
#   build mtkloader (extra args go to the compiler, e.g. -DXNU_CHECKPOINT=17)
#   -> pack mtkloader in front of Android's own kernel plus the XNU payload (mtkloader/build_flashed.py)
#   -> flash `boot`
#   -> boot Android -> ask it for one XNU boot (tools/boot-xnu.sh)
#   -> the watchdog returns the phone to Android -> read XNU's log from pstore.
#
# It is the build tree's tools/xnu-cycle.sh with three changes for this phone: `boot` instead of
# `boot_a` (one slot), the rooted boot image given by path (ANDROID_BOOT), and its own `su`
# discovery (Magisk's is /debug_ramdisk/su, not on adb shell's PATH).
set -e
S="${HAKIOS_SERIAL:?set HAKIOS_SERIAL to your phone adb serial}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${XNU_LOG_OUT:-$ROOT/logs/xnu-last.log}
# The phone's own rooted boot image: mtkloader is chained in front of the Android kernel already in
# it, and it is also the way back (fastboot flash boot it). Never in the repository.
ANDROID_BOOT=${ANDROID_BOOT:-$ROOT/backup/boot-magisk-patched.img}
# On this host adb/fastboot are Windows programs; under WSL they are reached as adb.exe/fastboot.exe
# (ADB/FASTBOOT override). fastboot.exe must be given the image as a Windows path.
ADB=${ADB:-adb}
FASTBOOT=${FASTBOOT:-fastboot}
command -v "$ADB" >/dev/null 2>&1 || { command -v adb.exe >/dev/null 2>&1 && ADB=adb.exe; }
command -v "$FASTBOOT" >/dev/null 2>&1 || { command -v fastboot.exe >/dev/null 2>&1 && FASTBOOT=fastboot.exe; }
# lancelot: `su` is not on PATH in adb shell; Magisk's is /debug_ramdisk/su. It can only be found
# once the phone is back in Android (the run may well start in fastboot, where there is no adb at
# all), so the search is deferred to just before boot-xnu.sh. HAKIOS_SU overrides it.
SU=${HAKIOS_SU:-}
find_su() {
	[ -n "$SU" ] && return 0
	for c in /debug_ramdisk/su su /system/bin/su; do
		if "$ADB" -s "$S" shell "$c -c id" 2>/dev/null | grep -q 'uid=0'; then SU="$c"; break; fi
	done
	[ -n "$SU" ]
}
export ADB
# adb.exe under WSL ends its lines with CR: strip it, or "device" never equals "device".
state() { "$ADB" -s $S get-state 2>/dev/null | tr -d '\r'; }
# test settings: XNU picked by default, watchdog and watcher on, XNU comes back by itself
SECS=${XNU_SECS:-45}
XNU_ARGS=${XNU_ARGS:-cpus=1}
CORES=${XNU_CORES:-$(expr "$XNU_ARGS" : '.*cpus=\([0-9]*\)' || echo 1)}
[ -n "$CORES" ] || CORES=1
WATCH=${XNU_WATCH:-$((SECS + 240))}
# No MENU_DEFAULT_XNU: the image boots Android, and XNU is asked for once (tools/boot-xnu.sh), so
# a kernel that hangs costs a power cycle and not a rescue through BROM.
TEST_FLAGS=(-DXNU_WDT_SECONDS=0 -DXNU_CORES=$CORES -DXNU_WATCH_SECONDS=$WATCH
	"-DXNU_RETURN_AFTER=\"$SECS\"" "-DXNU_EXTRA_ARGS=\"$XNU_ARGS epoch=$(date +%s)\"")
command -v python3 >/dev/null || { echo "xnu-cycle.sh: python3 is not in PATH"; exit 1; }
[ -f "$ROOT/build-payload.bin" ] || { echo "xnu-cycle.sh: no build-payload.bin: make one with xnuloader/mkpayload.py"; exit 1; }
[ -f "$ANDROID_BOOT" ] || { echo "xnu-cycle.sh: no $ANDROID_BOOT (set ANDROID_BOOT to your phone's rooted boot image)"; exit 1; }
mkdir -p "$(dirname "$OUT")"
"$ROOT/mtkloader/build.sh" "${TEST_FLAGS[@]}" "$@" >/dev/null
python3 "$ROOT/mtkloader/build_flashed.py" "$ANDROID_BOOT" >/dev/null
IMG="$ROOT/xnu-flashed-slot_a.img"
# fastboot.exe (Windows) needs a Windows path; fastboot (Linux) takes the file as it is.
IMG_ARG="$IMG"
case "$FASTBOOT" in *.exe) command -v wslpath >/dev/null 2>&1 && IMG_ARG=$(wslpath -w "$IMG") ;; esac
[ "$(state)" = device ] && "$ADB" -s $S reboot bootloader
for i in $(seq 1 120); do "$FASTBOOT" devices | grep -q $S && break; sleep 1; done
# (fastboot -s waits for ever for a phone that is not there: a run would hang for hours)
"$FASTBOOT" devices | grep -q $S || { echo "the phone did not come up in fastboot"; exit 1; }
# A-only: the whole image goes into `boot`. There is no second slot and no set_active to move to,
# which is why the rooted boot image above must be kept -- it is the only way back.
"$FASTBOOT" -s $S flash boot "$IMG_ARG" >/dev/null 2>&1
"$FASTBOOT" -s $S reboot >/dev/null 2>&1
echo "waiting for Android, to ask it for one XNU boot..."
for i in $(seq 1 100); do [ "$(state)" = device ] && break; sleep 3; done
sleep 4
find_su || { echo "xnu-cycle.sh: no working su on the phone (set HAKIOS_SU)"; exit 1; }
export HAKIOS_SU="$SU"
"$ROOT/tools/boot-xnu.sh"
echo "XNU is running; Android comes back by itself"
WAIT_ITERS=$(( (SECS + 300) / 3 ))
for i in $(seq 1 $WAIT_ITERS); do [ "$(state)" = device ] && break; sleep 3; done
sleep 3
for i in $(seq 1 10); do "$ADB" -s $S shell "$SU -c 'cat /sys/fs/pstore/console-ramoops-0'" > "$OUT" 2>/dev/null && [ -s "$OUT" ] && break; sleep 2; done
# what the kernel kept for the host (kern.hakios.host_keep / kern.redwood.host_keep), which mtkloader
# put into the pmsg zone: appended to the log, when it is this run's and not Android's own
"$ADB" -s $S shell "$SU -c 'cat /sys/fs/pstore/pmsg-ramoops-0'" > "$OUT.keep" 2>/dev/null || true
if head -c 64 "$OUT.keep" | grep -q 'kept for the host by the kernel'; then
	{ echo; cat "$OUT.keep"; } >> "$OUT"
	echo "kept for the host: $(head -1 "$OUT.keep" | sed 's/^=== kept for the host by the kernel: //; s/ ===$//')"
fi
rm -f "$OUT.keep"
echo "log: $OUT ($(wc -c < "$OUT") bytes)"
