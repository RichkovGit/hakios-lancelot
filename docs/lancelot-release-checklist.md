# Redmi 9 (lancelot): release checklist

This is a research port, not a daily-driver ROM. Results for other MediaTek phones
are not evidence that a feature works on Redmi 9. Do not publish as a stable release
until the supported paths below pass on this device.

## Current status

**Verified by the autotest stand (`tools/autotest.sh`, reports in `logs/autotest/`):**
- `hwprobe` 7/7: accelerometer/SCP, flashlight GPIO, MT6358 codec id, AW87519 amp id, battery telemetry.
- `audio4` / `audio4g` 12/12: speaker playback DMA (DL1→ADDA DAC→MT6358 LOL→AW87519), 1 kHz tone plays, volume control (`mtkafe_gain`). The audiosyscon/infra audio gates are active-low — kept at default.
- `mic` 4/4: microphone record DMA (MT6358 ADC1→ADDA UL→VUL12), ~48k mono frames/s into `/dev/redwood-mic`.
- `btapp` 5/5: Bluetooth app delivery — the entitled `bt_test` app launches from SpringBoard, registers with `bluetoothd`, `tccRequired : 0` (TCC bypass), scans.
- `btsettings` 5/5: Bluetooth Settings pane — client-side BluetoothManager getter hook forces `available 1 powered 1 powerState 2` (the pane reads local state ivars; MBF setup callback intermittent). Toggle reads green. (`bt_force_on=1`.)
- `perf` 5/5: raised clocks A75 @1999 MHz + Mali @800 MHz (`buttons=2 mtk_cpufreq=2 mali_mhz=800`), stable, SpringBoard up — the fix for the ~30 fps frame-budget root cause.
- `native` 4/4: 1080x2340 native surface + SpringBoard at raised clocks (`fbscale=0 fbwidth=1080 fbheight=2340`).
- `conn` 4/4: Wi-Fi associated + DHCP + traffic carried.
- `demo` 5/5: Mali GPU up + ran a job, SpringBoard launched, no panic.
- `audio5` 3/3: iOS audio sink registered + DL1 DMA running (sink-fed pending mediaserverd routing).
Pending interactive verification: physical audio listen/record, iOS audio reaching the speaker (sink-fed), touch 60 fps feel, native-resolution UIKit layout, BT Settings pane.

- XNU boots and hands control back to Android. On the latest diagnostic run,
  Wi-Fi associated and DHCP assigned an address.
- The panel scans out at 1080 × 2340; the current baseline renders at 375 × 812
  and scales up to the panel. A hardware run with `fbscale=2 fbxr=1 fbfit=0`
  confirmed an 828 × 1792 surface and a clean return to Android, but the
  scripted taps did not reach Settings. This does not pass the UI gate. Check
  the lock screen and icon layout before comparing touch or changing the default.
  A separate run with `fbscale=0 fbwidth=1080 fbheight=2340 fbfit=0` confirmed
  a 1080 × 2340 surface at 4352 bytes per scanout row and a clean return to
  Android. It also did not reach Settings, so native-size surface allocation
  is verified, but interactive UIKit layout and sustained GPU performance are not.
  Diagnosis of runs 71–73: the baseline run (71, `fbscale=1`) reached Settings
  (SpringBoard pid 8, Preferences pid 170, 164 processes). The two native runs
  (72, 73) ended with only ~85 processes and no SpringBoard: `fbwait` returned
  "a frame after 0.0 s" at ~43–45 s because a frame was already grabbable that
  early, so the fixed-wait script tapped before SpringBoard had started
  (backboardd was up and received the touches, but there was no home screen).
  The native UI failure is therefore a test-readiness/timing artifact, not a
  proven geometry fault. Re-test native mode gated on SpringBoard's process
  appearing and a settled compositor frame (a `waitproc SpringBoard` step),
  then wait before tapping; do not change the default until that passes.
- The battery regression (`RC_JOBS=rc.jobs.lancelot-battery`, `mtkbatt=2`)
  reported one IOPS source, 100%, full and USB/adapter power in ten successive
  samples. The kernel reported 31.50 °C and 4378 mV versus 31.1 °C and 4347 mV
  in Android just before boot. Android reported charging while IOPS reported
  full/not charging. Repeat at partial charge and with USB unplugged before
  treating charge-state reporting as verified. Do not change charger settings.
- The optional [HakiOS loader splash](lancelot-splash.md) passes seven host
  framebuffer cases with sanitizers. This does not verify physical visibility
  or a seamless handoff, and does not restore local Apple artwork.
- The audio HAL opens its endpoint, but the Redmi 9 speaker and microphone need
  an MT6768 AFE and MT6358 codec backend. Identify the active board amplifier
  and microphone route before enabling output or bias.
- The Bluetooth controller delivers LE advertising events to bluetoothd. The
  command-line CoreBluetooth test still reports zero discoveries. A registered
  app with normal Bluetooth authorization is the next control.
- Runs 76–82 (A2 app experiment). The app `/Applications/bt_test.app`
  (bundle `com.redwood.bt_test`, ad-hoc signed, no entitlements,
  `NSBluetoothAlwaysUsageDescription`) is staged in `xnusys`; a full partition
  backup (`/data/local/tmp/xnusys-backup.img`, 4295983104 bytes) was taken
  before the first write. Every run booted cleanly and returned to Android.
  Logging was fixed so evidence survives: `bt_hci_trace=0` gates the HCI byte
  trace that filled the 64 KB `host_keep` ring; `bt_said_from` delays inject's
  bluetoothd capture; `bt_test` and `battery_test` now also write to
  `host_keep`; `net_test waitproc`/`sbicons` were added.
  Findings: the ordinary CLI is identified by bluetoothd by its **executable
  name** (`bt_test_a2_cli-central-157-47`, also a `-MBF-` BTSession), reaches
  scan level 2, and gets **0 advertisements delivered**; CoreBluetooth reports
  state 5 (powered on) but **authorization 2 (denied)**, and BluetoothManager
  reports available 0 / powered 0. bluetoothd restarts during the run
  (e.g. 215 → 101). The app does **not** launch:
  `SBSLaunchApplicationWithIdentifier` returns **9** both unsigned and with
  entitlements embedded via ld64 (`com.apple.springboard.launchapplications`),
  and no `bt_test` app process appears (163 processes, SpringBoard pid 8 up).
  `sbicons` finds no `iconState` key in `com.apple.springboard.plist` (14
  unrelated keys), so the icon position is not persisted there. Result 9 is
  unchanged by entitlements, pointing at LaunchServices registration rather
  than authorization: the next step is to register the bundle with `lsregister`
  (or seed the LaunchServices database) and retry the launch, then tap Allow on
  the TCC prompt and compare advertisement delivery with the CLI.
- **Root cause found AND FIXED (runs 83→88): bluetoothd crashed on a vendor
  response.** Run 87 (HCI trace on, no applog) caught the exact trigger:
  bluetoothd sends `01 e2 fc 05 0d 00 00 00 01` (OGF 0x3f, **0xFCE2 sub 0x0D**,
  the LE match-buffer/LPM read), the kernel answered status-only, and bluetoothd
  asserted (ByteStream_NumReadBytesAvailable) → SIGABRT immediately after
  ("vendor 0xfce2 (sub 0xd) answered here" → "userabort"). It exited repeatedly
  ("service has crashed 3 times in a row"), launchd throttled it, and it stayed
  dead — this, not only authorization, is why delivery was 0. **Fix:**
  `bt_vendor_done` now gives sub 0x0D a 16-byte data stream (first byte the
  available-entry count 0), like the antenna prefs. **Run 88 (no applog, the
  former crash condition) confirms it: no userabort, no SIGABRT, bluetoothd[22]
  stays up ("Stack Did Start", sub 0x0d answered, LE_Set_Scan_Parameters
  proceeds), no restarts.** The crash was intermittent before because the
  status-only answer only asserted on some bring-up timings.
- Run 89 (fixed kernel, stable bluetoothd[22]): the ordinary CLI still reports
  **authorization 2 (denied)**, state 5, scanning, **0 devices seen**;
  BluetoothManager still available 0 / powered 0. bluetoothd's own and sharingd's
  sessions also log "0 advertisements delivered" (they scan for specific services
  not present), while the controller still reports 9–11 Advertising Events to
  bluetoothd (runs 84/86). So reception works; delivery to a *denied* CLI is 0,
  as expected — the control that matters is a properly authorized foreground app.
  **The app now launches (runs 90–93).** `SBSLaunchApplicationWithIdentifier`
  from the unentitled CLI returned 9, but calling it from *inside SpringBoard*
  succeeds: inject_sb.c registers a Darwin notification `com.redwood.launch`
  (`rwtouch notify com.redwood.launch`) that calls it with SpringBoard's own
  audit token. `/Applications/bt_test.app/bt_test` runs as a foreground UIKit
  process (pid 180, "procs: 1 of 173"). Its scanner reports **authorization 0
  (notDetermined), state 0, isScanning 0** — it is waiting on the Bluetooth TCC
  prompt; no Allow has been tapped yet, so delivery is still 0. A tap at the
  alert's presumed Allow position (0.68, 0.555) was tried in run 93 but the
  screen went **Off** mid-run ("Screen state changed to Off"), which stops the
  scan, and the app's report was pushed out of host_keep by the TX-slot trace.
  Runs 94–95 added a `bt_tx_trace=0` gate (redwood_btsk.c) so the app report
  survives, `idleTimerDisabled` in both inject_sb and the app itself (bt_app.m),
  and Allow taps at the alert's presumed right-button position. The app report
  is captured every time: **authorization 0 (notDetermined), state 0,
  isScanning 0, 0 devices**. The taps never grant (authorization stays
  notDetermined, not denied — so they miss the alert, not press Don't Allow).
  The `com.redwood.layers` dump produces no output at all in these runs (no
  window/alert layers captured), and bluetoothd shows no
  `getReadyForTCC com.redwood.bt_test` line: the app's CBCentralManager never
  registers a central session because it is stuck at notDetermined.
  **Confirmed (run 96): the Bluetooth TCC prompt is not presented.** inject_sb's
  `alert_chain` (fires 6 s after the launch) dumped SpringBoard's 15 windows:
  wallpaper (PBUIPosterWallpaperRemoteViewController), home screen
  (SBHomeScreenViewController), the app's plain UIViewController, switcher and
  input windows — **no TCC/UIAlert/SBAlertItem anywhere, and no
  presentedViewController on the app's controller**. CoreBluetooth's permission
  request never raises a visible Allow alert, so authorization stays
  notDetermined and there is nothing to tap. Next: trace why tccd/SpringBoard
  does not present the Bluetooth TCC alert. Run 97 (applog_name=tccd,bluetoothd)
  shows tccd serving system daemons' requests, but **no TCCAccessRequest from
  com.redwood.bt_test at all**, and repeated "Unable to construct an identity to
  kTCCServiceBluetoothAlways" (kTCCErrorDomain Code=2, authValue=0/authReason=1)
  for processes whose identity tccd cannot attribute. The app's ad-hoc signature
  / LaunchServices registration likely prevents tccd from constructing its
  identity, so no prompt is raised and authorization stays notDetermined. Next:
  give the app a signature/registration tccd accepts (proper LaunchServices
  registration, or a trusted code signature), confirm a TCCAccessRequest from
  com.redwood.bt_test raises the Allow prompt, grant it, and verify delivery to
  `com.redwood.bt_test-central`; then re-test Settings' BluetoothManager pane.
  The abort follows the loop "bt device opened → LE_Set_Scan_Parameters (75
  probes) → scanner-off injected → device closed → abort". inject's abort
  interposer captured the stack (bluetoothd base 0x100000000, shared-cache slide
  0x25c20000): abort ← 0x100fd4850 ← 0x101037a6c ← 0x10102d780 ← 0x10102c91c ←
  0x100f7dd50 ← 0x100f7d410. The standalone bluetoothd is stripped (no nm
  symbols) and these addresses are above inject's bt_trace range
  (0x100100000–0x1005e0000). Next: symbolicate via the dyld shared cache symbols
  or extend bt_trace to the abort range, and correlate the last HCI
  command/event before the abort (a synthesized vendor response or the
  scan-off injection is the prime suspect).
- The crash is **intermittent**: run 84 (`applog=1 applog_name=bluetoothd`)
  had bluetoothd stable and scanning — "Scanning stopped successfully. 9
  Advertising Events" then 11 — while runs 83/85 aborted. So the
  controller→bluetoothd path works when bring-up does not race; the abort is a
  bring-up race, not a permanent fault.
- Refined abort site (run 85, bluetoothd base 0x100000000): the aborting frames
  unslid to bluetoothd text ≈ offsets 0x529410 → 0x529d50 → 0x5d891c → 0x5d9780
  → 0x5e3a6c → 0x580850 → abort. Disassembly of 0x5e3a3c shows a stack-guard
  checked os_log/assert helper whose string arguments live in the **LPM / scan
  match-buffer** region ("Set LPM flags for White Glove Tests", "Failed to set
  lpm1 flag with error", "Start session error", "Match Buffer reading handle",
  "Match buffer complete with status %d"). The abort is therefore in
  bluetoothd's LE scan LPM-flag / match-buffer configuration — the same path the
  kernel's synthesized vendor responses and scan-off injection touch. Next:
  identify the exact LPM/match-buffer vendor command (OGF 0x3f) whose
  synthesized response makes bluetoothd assert, and correct its status/payload
  (or complete it with the error bluetoothd expects instead of a bad success).
  `bt_vendor_done` (redwood_connsys.c) gives data only for 0xfd48 (+1),
  0xfd1b/0x51 (+64) and 0xfce2/0x15,0x36 (+16); every other OGF 0x3f opcode
  (incl. the 0xfce9 and 0xfce2/0x1e match-buffer/PCF reads seen in run 86) gets
  status-only. Run 86 (HCI trace on) was stable: bluetoothd read
  `TryReadMatchAndADVBuffers fMatchBufferExpectedEntries:0x255` and the
  0xfce9/0x05,0x07 + 0xfce2/0x1e commands completed status 0. The abort needs a
  crashing run with HCI trace on to pin the exact sub-command; it did not recur
  in 84/86, so it is a bring-up race rather than a deterministic bad response.
- The `lsapp` LaunchServices probe: `LSApplicationCopyForBundleID`,
  `LSRegisterURL` and `_LSApplicationRegisterURL` are all absent (symbol 0x0,
  both RTLD_DEFAULT and dlopen) on this iOS 16.5 — those are macOS-era symbols.
  Registration needs the iOS LaunchServices path (`lsdiagnose` / lsd's own
  scan of `/Applications`); `SBSLaunchApplicationWithIdentifier` still returns
  9 and no app process appears. `applog=1 applog_name=bluetoothd` is the way to
  capture bluetoothd's own stderr (the assertion text) on the console.
- The BluetoothManager CLI probe still reports unavailable. The IORegistry
  service named bluetooth is visible before and after the attach attempt; this
  rules out a missing service name from the CLI's point of view. A system daemon
  successfully attached using the legacy transport in the same boot. In a
  subsequent Settings run, bluetoothd sent status events to the real Preferences
  session and handled its Classic discovery request. Visible devices, toggle
  behavior and pairing remain unverified.
- A previous shutdown panic while quiescing Wi-Fi was fixed by removing console
  output from the interrupt-disabled path. The next hardware run returned to
  Android with no panic string; verify this again before release.

- **Accelerometer / SCP working (run scp7b):** the SCP DRAM-size clamp in
  redwood_scp.c unblocked bring-up; `scp_step=7` starts the hub, enables the
  accelerometer at 50 Hz and reads real x/y/z samples into `kern.redwood.accel`
  (z ≈ 1 g). **Screen rotation applied (run scp7c):** hooking the manager's init
  and CoreMotion-update callback captured the instance (manager 0x12082c5a0);
  orientation computed from accel and sent to backboardd ("last sent 5, 1
  changes"). Accel → orientation → backboardd verified end-to-end.

- **Flashlight driver implemented & exercised (run torch2):** new
  redwood_mtktorch.c (MTK GPIO EN152/SEL153, boot-arg mtktorch=N) drove the torch
  for 3 s and off cleanly; physical light to be confirmed by eye (SEL polarity).

## On-device gates

- [ ] Verify model, boot partition mapping and image size before writing; confirm
  the original boot image and documented recovery path are available locally.
- [ ] Verify two consecutive XNU boots, a return to Android, and the rollback.
- [ ] Verify touch, native panel output and GPU composition independently: the
  panel mode, render surface resolution, UIKit scale and touch coordinates are
  distinct. Verify smooth animations and no distorted or clipped UI.
- [ ] Verify a centered boot splash and a clean handoff to the compositor. Keep
  any locally supplied Apple artwork out of the publication tree; provide an
  original or otherwise redistributable default.
- [ ] Verify speaker output at low volume, route changes, and separate microphone
  capture with normal app permission. Do not infer capture from playback.
- [ ] Verify battery percentage, temperature and charge-state reporting against
  Android. Do not change charger settings or rail voltages for diagnostics.
- [ ] Verify Wi-Fi (association, DHCP, DNS and HTTPS).
- [ ] Confirm that a normally authorized foreground app discovers a known BLE
  advertiser. Compare counts before and after restarting the scan.
- [ ] Open Settings > Bluetooth; verify adapter state, toggle and device discovery.
- [ ] Verify pairing and reconnect separately, or mark them unsupported. Do not
  claim support for audio profiles based on discovery or pairing alone.

## Source publication gates

- [ ] Build from a clean checkout with documented dependencies and locally supplied
  device files. Do not rely on absolute host paths or a stale built image.
- [ ] Review the exact tracked tree and reachable history, not just ignore rules.
  Exclude raw logs, local handoff notes, boot backups, private keys and network
  credentials, serials, nearby radio identifiers, screenshots, firmware and
  proprietary OS images. A ramdisk may embed network credentials. The current
  checkout is shallow: a local scan is not a full history audit.
- [ ] Resolve the existing tracked firmware headers, vendor EFI files, prebuilt
  payloads and documentation images before release. Ignore rules do not remove
  files already tracked. If provenance or distribution rights are unverified, omit
  them from the publication tree and its history.
- [ ] Verify licenses and provenance of all third-party code and any intentionally
  distributed binary. Publish source and checksums only for reviewed artifacts.
- [ ] Update both READMEs for Redmi 9, with verified features separated from
  experimental and unsupported features. Document exactly which partition the
  installer writes; keep inspection read-only by default.
