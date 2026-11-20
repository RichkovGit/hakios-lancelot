#!/bin/bash
# Reboot the Xiaomi Redmi 9 (lancelot) into Darwin/XNU once: mtkloader starts Android unless it
# finds this request (kept across the reboot by ramoops' pmsg zone); the boot after XNU is Android
# again. The string is the loader's, unchanged from the reference (mtkloader.c, boot_choice()).
set -e
S="${HAKIOS_SERIAL:?set HAKIOS_SERIAL to your phone adb serial}"
# On this host adb is a Windows program; under WSL it is reached as adb.exe (ADB overrides).
ADB=${ADB:-adb}
command -v "$ADB" >/dev/null 2>&1 || { command -v adb.exe >/dev/null 2>&1 && ADB=adb.exe; }
# lancelot: `su` is not on PATH in adb shell; Magisk's is /debug_ramdisk/su (docs/NEXT-STEPS.md,
# docs/PHONE-STUCK-BROM.md). tools/xnu-cycle.sh exports HAKIOS_SU after probing; when this script
# is run on its own, it is found here.
if [ -z "${HAKIOS_SU:-}" ]; then
	for c in /debug_ramdisk/su su /system/bin/su; do
		if "$ADB" -s "$S" shell "$c -c id" 2>/dev/null | grep -q 'uid=0'; then HAKIOS_SU="$c"; break; fi
	done
fi
[ -n "${HAKIOS_SU:-}" ] || { echo "boot-xnu.sh: no working su on the phone (set HAKIOS_SU)"; exit 1; }
"$ADB" -s "$S" shell "$HAKIOS_SU -c 'printf DARWIN-REDWOOD:BOOT-XNU > /dev/pmsg0'"
"$ADB" -s "$S" reboot
