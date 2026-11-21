#!/bin/bash
# Automated hardware test stand for HakiOS on Redmi 9 (lancelot).
#   HAKIOS_SERIAL=<serial> ./tools/autotest.sh <profile>
# Profiles in tools/autotest-tests.json: one boot per profile (build payload ->
# flash boot -> XNU runs and returns -> Android back -> read pstore -> parse into
# logs/autotest/<ts>-<profile>/report.{json,md}). Report-only: a failing probe is
# recorded, not fatal, so a long unattended bring-up keeps going.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
S="${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the phone adb serial}"
PROFILE="${1:-hwprobe}"
ADB="${ADB:-adb.exe}"; FASTBOOT="${FASTBOOT:-fastboot.exe}"
ANDROID_BOOT="${ANDROID_BOOT:-$ROOT/../../backup/boot-magisk-patched.img}"
command -v python3 >/dev/null 2>&1 || { echo "autotest: python3 not in PATH"; exit 1; }
{ IFS= read -r ARGS; IFS= read -r SECS; IFS= read -r CORES; } < <(python3 "$ROOT/tools/autotest-profile.py" "$PROFILE")
[ -n "$ARGS" ] || { echo "autotest: unknown profile $PROFILE"; exit 1; }
dev="$("$ADB" -s "$S" shell getprop ro.product.device 2>/dev/null | tr -d '\r')"
[ "$dev" = lancelot ] || { echo "autotest: device $dev is not lancelot (serial $S)"; exit 1; }
[ -s "$ANDROID_BOOT" ] || { echo "autotest: no rollback boot image $ANDROID_BOOT"; exit 1; }
TS="$(date +%Y%m%d-%H%M%S)"
OUTDIR="$ROOT/logs/autotest/$TS-$PROFILE"
mkdir -p "$OUTDIR"
export HAKIOS_SERIAL="$S" ANDROID_BOOT ADB FASTBOOT
export XNU_CORES="$CORES" XNU_SECS="$SECS" XNU_ARGS="$ARGS"
export XNU_LOG_OUT="$OUTDIR/xnu.log"
echo "autotest: profile=$PROFILE serial=$S secs=$SECS"
echo "autotest: args=$ARGS"
printf '%s\n' "$ARGS" > "$OUTDIR/args.txt"
./tools/xnu-payload.sh
./tools/xnu-smoke.sh
python3 "$ROOT/tools/autotest-parse.py" "$XNU_LOG_OUT" "$PROFILE" "$OUTDIR"
echo "autotest: done -> $OUTDIR"
