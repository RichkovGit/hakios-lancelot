# HakiOS autotest stand (lancelot)

A repeatable, report-only hardware test harness. One boot per profile: build the
payload, flash `boot`, let XNU run and return to Android, read the pstore log, and
parse it into PASS / FAIL / UNKNOWN against fixed markers. A failing probe is
recorded, never fatal, so a long unattended bring-up keeps going.

## Run
```sh
# under WSL, from work/hakios; the phone must be the lancelot (serial checked)
HAKIOS_SERIAL=<your-serial> ADB=adb.exe FASTBOOT=fastboot.exe \
  ANDROID_BOOT=<path-to>/backup/boot-magisk-patched.img \
  ./tools/autotest.sh hwprobe
```
Output: `logs/autotest/<ts>-<profile>/` with `xnu.log`, `args.txt`,
`report.json`, `report.md`.

## Files
- `tools/autotest.sh` — orchestrator (preflight -> payload -> smoke -> parse).
- `tools/autotest-profile.py` — prints a profile's boot-args / secs / cores.
- `tools/autotest-parse.py` — regex markers -> PASS/FAIL/UNKNOWN + report.
- `tools/autotest-tests.json` — profiles and their checks (edit to add tests).

## Profiles
- `hwprobe` — one read-only-ish boot covering accelerometer (SCP step 7), flashlight
  GPIO probe, MT6358 codec id, AW87519 amp id, battery telemetry. 7 checks.
- `audio3` — audio clocks bring-up (APLL1 powered, audiosyscon gates, AFE tuner).
- `audio4` — full playback: DL1 ring + ADDA DAC + MT6358 LOL analog + AW87519 kspk
  profile + a 1 kHz tone at -40 dB LOL; checks the DL1 DMA cursor advances.
- `conn` — Wi-Fi + Bluetooth (180 s session): CONNSYS powered, en0 created, Wi-Fi
  traffic carried (sent>0), bluetoothd opens the BT device and stays stable.

## How a check works
Each test has `pass` regexes (all must appear) and `fail` regexes (any appears ->
FAIL). No fail and all pass present -> PASS; otherwise UNKNOWN (not observed).
Markers are the drivers' `redwood_keep(...)` / `printf(...)` lines, which mtkloader
saves into pstore (console-ramoops + the host_keep pmsg zone).

## Adding a test
Add a profile or test object to `tools/autotest-tests.json`. Use a marker the driver
already prints, or add a `redwood_keep("...")` line and rebuild the kernel
(`./xnu-build.sh -j8`). Keep markers stable so reports stay comparable across runs.

## Safety
Preflight refuses any device that is not `lancelot` and requires the rollback boot
image. Only `boot` is written; `xnusys` is untouched by the stand. Audio playback
starts at -40 dB LOL with a low-amplitude tone; nothing loud until physically verified.
