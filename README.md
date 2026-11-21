# HakiOS · Xiaomi Redmi 9 (lancelot)

English · [Русский](README.ru.md)

**Apple's open-source Darwin kernel (XNU, xnu-8796.121.2 — the iOS 16.5 kernel) on the Xiaomi Redmi 9**, the way Android is Linux on
phones: a boot loader of its own (`mtkloader`), drivers of its own, and an unmodified iOS 16.5 user space on top
(launchd → backboardd → SpringBoard → apps).

Device: **Xiaomi Redmi 9 global (lancelot) — MediaTek Helio G80 (MT6769T, MT6768 family), Mali-G52 GPU, MT6358 PMIC,
FocalTech FT8719P touch, CONNSYS MT6631 Wi-Fi/BT.** This port derives from the Infinix Hot 20 (X6826B, MT6768) `mtk`
baseline; the Infinix journals in `docs/` describe that phone and do **not** all apply to lancelot.

The phone loses nothing. `mtkloader` installs into `boot` ahead of the Android kernel; it starts XNU once, and after the
session the phone returns to Android by itself. The iOS system image lives in its own `xnusys` partition, read from eMMC.
**A research bench, not a ROM / daily driver.**

**Status & checklist:** [docs/lancelot-release-checklist.md](docs/lancelot-release-checklist.md) ·
**Install:** [docs/from-a-clone.md](docs/from-a-clone.md), [docs/mtk-porting.md](docs/mtk-porting.md).

## What works on lancelot (verified on the device)

- **Boot & return** — XNU boots, SMP 4 cores, the session ends back in Android without a panic; rollback to stock is one `boot` flash.
- **Graphics** — in-kernel Mali driver (`redwood_mali.c`), Mesa **panfrost** (OpenGL ES) and a **Metal** driver
  (`redwoodmetal.m`): SpringBoard, home screen, gestures, Control Center; crisp at 2× (`fbscale=2`).
- **Settings** opens with the Metal driver (`metal=1`).
- **Accelerometer / SCP** — MT6768 sensor hub (`redwood_scp.c`, `scp_step=7`), real x/y/z at 50 Hz.
- **Screen rotation** — accelerometer → orientation → backboardd, end-to-end.
- **Flashlight** — LED191 on two GPIOs (EN152/SEL153), `redwood_mtktorch.c` (`mtktorch=3`) + the iOS ABI.
- **Battery** — percentage, voltage, current, temperature via the MT6358 fuel gauge (`redwood_mtkbatt.c`, `mtkbatt=2`); matches Android.
- **Touch** — FocalTech FT8719P (`redwood_fts.c`), reports with zero bad/timeout/CRC errors.
- **Bluetooth** — the controller answers, `bluetoothd` is stable (a scan-time crash was fixed), LE scan receives advertisers.
- **Boot splash** — an original HakiOS mark (`mtkloader/splash.h`), no Apple assets.

## Known bugs / not yet (honest)

- **Touch feels sticky** — render is ~30 fps (not 60); the touch chip is fine, the lag is frame-rate / HID delivery.
- **Settings crashes without Metal** — opens only with `metal=1`.
- **Jagged image at 1×** — 375×812 upscaled to 1080×2340; crisp only at 2× / native.
- **No internet in the demo config** — the kernel begins joining but `wifid` refuses association / DHCP; Wi-Fi traffic did flow in separate runs.
- **Bluetooth does not deliver to apps / Settings** — TCC does not construct the app's identity, so the Allow prompt never raises.
- **Sound (speaker) — DMA running.** The full DL1→ADDA DAC→MT6358 LOL→AW87519→speaker chain is up and verified by autotest `audio4` (12/12): APLL1 clocks, codec, LOL (−40 dB), amp profile, DL1 memif, IRQ0, DL1→ADDA_DL route, **DMA reads the ring** (CUR advances, ADDA receives data). A 1 kHz tone plays (`mtkafe=4`), volume is adjustable (`mtkafe_gain`, `audio4g` 12/12); the iOS audio sink is registered (`mtkafe=5`, HAL Speaker plugin confirmed). **Microphone: record DMA working** (`mtkmic=1`, `mic` 4/4): MT6358 ADC1→ADDA UL→VUL12→mono `/dev/redwood-mic` ring, ~48k frames/s. Physical listen/record and in-app audio await interactive verification. Details: [docs/lancelot-audio-backend.md](docs/lancelot-audio-backend.md).
- Screen may sleep (auto-lock); native 1080×2340 + UIKit scale not yet reconciled with touch.
- Camera, vibration, GPS — queued (see [docs/lancelot-peripherals.md](docs/lancelot-peripherals.md)).
- Apple online services (activation, Apple ID, iCloud, App Store) — **not supported, not a goal** (no Secure Enclave).

## What you bring, and what is never here

Nothing of Apple's and no vendor firmware is in this repository. The Darwin user space you lay out yourself from an IPSW you fetch
yourself (`ios165/fetch.sh`); the MediaTek firmware (CONNSYS, SCP) comes off your own phone's Android. Nothing is downloaded for
you, decrypted or circumvented. No iOS file, no phone firmware, no iOS screenshot, no device serials or radio identifiers are
tracked. See [docs/lancelot-release-audit.md](docs/lancelot-release-audit.md).

## Building

Kernel: external XNU source + `xnu/redwood.patch` (`xnu-build.sh`). User space: `userland/build.sh` (`GL=1 METAL=1` for graphics).
System image: `tools/system-flash.sh` → `xnusys`. Loader + session: `tools/xnu-payload.sh` + `tools/xnu-smoke.sh`.
Every command in order: [docs/from-a-clone.md](docs/from-a-clone.md).

> **Important:** the iOS system image (`system.apfs` for `xnusys`) is **not distributed** in this repository — it is Apple's
> proprietary userland, built yourself from your own IPSW (`ios165/fetch.sh`). Without it XNU boots but launchd cannot find
> `libSystem.B.dylib`/`launchd` (the "missing libSystem.B.dylib" error). There is no ready "boot.img with iOS": `boot.img`
> carries only the loader + kernel; the iOS root lives in `xnusys` and is proprietary.

## Autotests

Automated stand: one boot → probes → parse pstore → PASS/FAIL report.
`HAKIOS_SERIAL=<serial> ./tools/autotest.sh <profile>` (under WSL). Profiles: `hwprobe`
(accelerometer, flashlight, codec, amp, battery — 7 checks), `audio3`/`audio4` (sound).
Reports in `logs/autotest/<ts>-<profile>/report.{json,md}`. See [docs/lancelot-autotest.md](docs/lancelot-autotest.md).

## License and legal

The XNU patch and files derived from it are under the Apple Public Source License 2.0; everything else is MIT, as the file headers
say. None of this is legal advice.

HakiOS is an independent research project, **not affiliated with or endorsed by Apple**; "Darwin", "iPhone", "XNU", "iOS" and
"Apple" are named only to describe what the code works with. If you believe a file here infringes anyone's rights, open an issue.

## Origin

Based on the HakiOS `mtk` baseline (Infinix Hot 20 / X6826B). Upstream:
[hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel). This repository is the Redmi 9 (lancelot) port.
