#!/bin/bash
# Write userland/system.apfs into the phone's xnusys partition, straight from the host (no
# temporary copy on the phone): that partition is where mtkloader reads XNU's root from.
# Needs Android up with root for the shell. LK's fastboot cannot take a file this large.
# ADB / HAKIOS_SU override the adb and su programs (adb.exe and /debug_ramdisk/su under WSL).
set -e
S="${HAKIOS_SERIAL:?set HAKIOS_SERIAL to your phone adb serial}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:-$ROOT/userland/system.apfs}"
ADB=${ADB:-adb}
SU=${HAKIOS_SU:-su}
command -v "$ADB" >/dev/null 2>&1 || { command -v adb.exe >/dev/null 2>&1 && ADB=adb.exe; }
[ -f "$IMG" ] || { echo "no such image: $IMG"; exit 1; }
# adb.exe (Windows) needs a Windows path; adb (Linux) takes the file as it is.
IMG_ARG="$IMG"
case "$ADB" in *.exe) command -v wslpath >/dev/null 2>&1 && IMG_ARG=$(wslpath -w "$IMG") ;; esac
"$ADB" -s "$S" shell "$SU -c 'ls -l /dev/block/by-name/xnusys'" || {
	echo "xnusys not there: reboot once so the kernel rereads the partition table"; exit 1; }
# Through a file on /data: piping into dd over "adb exec-in su -c" silently drops most of it.
TMP=/data/local/tmp/system.apfs
echo "sending $(du -h "$IMG" | cut -f1) to the phone..."
"$ADB" -s "$S" push "$IMG_ARG" $TMP
echo "writing it to xnusys..."
"$ADB" -s "$S" shell "$SU -c 'dd if=$TMP of=/dev/block/by-name/xnusys bs=4M'"
"$ADB" -s "$S" shell "$SU -c 'sync; rm -f $TMP'"
"$ADB" -s "$S" shell "$SU -c 'sync; dd if=/dev/block/by-name/xnusys bs=512 count=1 2>/dev/null | dd bs=1 skip=32 count=4 2>/dev/null'"
echo " <- an APFS container begins with NXSB at offset 32"
