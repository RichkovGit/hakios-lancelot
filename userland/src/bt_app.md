# Bluetooth A2: client identity

Run from `userland`. `BT_A2=package ./build.sh` only cross-builds
`obj/bt_test.app` and `obj/bt_test_a2_cli`; it neither builds an image nor
registers or launches anything.

**TCC fix (2026-11-22):** the GUI app is now signed with the Bluetooth TCC-bypass
entitlement `com.apple.private.tcc.allow = kTCCServiceBluetoothAlways`
(`src/bt_test.entitlements`) via `ldid -S` (build.sh signs `$BT_APP/bt_test` after
linking; ldid at `$THEOS/toolchain/linux/iphone/bin/ldid`). With this private
entitlement the app can use CoreBluetooth **without an Allow prompt** (TCC is
bypassed, not prompted). The app also keeps `NSBluetoothAlwaysUsageDescription`.

**VERIFIED (2026-11-22, autotest btapp 5/5):** with the entitled app in xnusys and BT=1
(BlueTool inject answers the boot script), `auto_bt=1` launches the app from SpringBoard;
bluetoothd logs `Registered application - bt_test`, a `bt_test-central` session, and
`tccRequired : 0` (the bypass works, no Allow prompt). The app scans without interaction.

To apply on the phone: rebuild the system image with the entitled app
(`BT_A2=1 ./build.sh`, needs the ld64 cross-linker; the app binary can be re-signed
in place with `ldid -Ssrc/bt_test.entitlements obj/bt_test.app/bt_test`), then
`tools/system-flash.sh` writes `system.apfs` to `xnusys` (boot both new images; a
payload-only update leaves the old `/Applications`). Verify via bluetoothd scan
logs + the app scanning without a prompt (the bypass means no Allow tap needed).

## Settings Bluetooth pane (status)

The pane's plumbing exists and was brought to a working state on the Infinix baseline
(docs/bluetooth-x6826b.md, runs 735-762): green toggle, "Other Devices" list, classic
inquiry, correct device name. The original blocker was MobileBluetooth looking up an
IORegistry service "marconi-bt"/"bluetooth" that did not exist -> BTSessionAttachWithQueue
error 9, BluetoothManager.init nil, grey toggle. Fixed by the kernel creating the
"bluetooth" IOService (iokit/Kernel/RedwoodBluetooth.cpp RedwoodBluetoothNode, called from
redwood_connsys.c connsys_bt_start once the controller answers 6 HCI commands; logs
"RedwoodBluetooth: the service bluetooth is in the registry"). Device name comes from
SCDynamicStoreCopyComputerName, set by net_test device_name (SCPreferencesSetComputerName).
On lancelot VERIFIED in an interactive session (2026-11-22, logs/lancelot-interactive.log):
the IORegistry "bluetooth" service EXISTS (bt_test: "IORegistry bluetooth: found=1", so
RedwoodBluetoothNode succeeded), CoreBluetooth works (authorization 3, central manager
state 5 powered on, scanning). Touch flawless (35743 reports, 0 errors). Settings opened.
**PANE BLOCKER FOUND:** bt_test logs "BluetoothManager available 0 powered 0 powerState 0"
-- the PRIVATE BluetoothManager framework (which the Settings pane uses, distinct from
CoreBluetooth/CBCentralManager) reports unavailable/not-powered, so the Settings toggle is
grey. CoreBluetooth's path works but BluetoothManager's (BTSessionAttachWithQueue /
MobileBluetooth) does not see the controller as available. PRECISE BLOCKER (bt-manager-fix research + interactive log): the Infinix IOService gate
PASSES (bm sharedInstance non-nil, bluetooth found=1, MG bluetooth=1, all 4 mach services
resolve). The break is DOWNSTREAM: BluetoothManager's MBF (MobileBluetooth Framework, legacy/
classic) session+power handshake with bluetoothd never completes. bluetoothd log shows ONLY
com.apple.mediaserverd-MBF SessionAttach succeeds; NO Preferences-MBF / bt_test-MBF attach
arrives, and the advertising stack reads "OFF" (classic off, LE on). CoreBluetooth works via
a separate LE path (CBCentralManager state 5). BluetoothManager.powered reads the classic
stack power -> OFF -> powered 0 -> grey toggle. BluetoothCapability=0 is a red herring;
marconi-bt alias low-value (bluetooth fallback already found).
FIX IMPLEMENTED (bt_force_on=1, inject.c bt_force_power_on, three single-instruction patches):
1. PowerManager getter 0x1003a5f1c: ldr w0,[x19,#0x70] -> mov w0,#1. VERIFIED: bluetoothd
   logs "PowerManager power state is 1".
2. MBF XPC access level 0x100220ce0: ldr w8,[x0,#0x50] -> mov w8,#1. VERIFIED BREAKTHROUGH:
   the "insufficient entitlements to send MBFramework XPC messages" reject is gone, and
   bt_test-MBF kCBMsgIdSessionAttach now COMPLETES ("Attached session for bt_test-MBF-...",
   device queries flow). This was the main MBF handshake blocker.
3. GetModulePower store 0x10035e670: str w8,[x19] -> mov w8,#1 (changed 0xffffffff -> 0).
4. GetModulePower handler reply 0x1002d6e0c: ldr w2,[sp,#12] -> mov w2,#1, forcing
   kCBMsgArgBTPowerMode = 1 (ON) in the XPC reply (the handler 0x1002d6d2c sets BTPowerMode
   from the table-mapped [sp,#12], 0=off).
STATUS: the MBF handshake blocker is FIXED (patch 2: bt_test-MBF SessionAttach completed in
run 050808, device queries flowed). PowerManager forced ON (patch 1). But BluetoothManager
still reads available 0 / powered 0 in subsequent runs, and the bt_test-MBF attach is
intermittent (did not re-attach in runs 051518/052136). REMAINING: (a) the BluetoothManager
available/powered read may use a cached value or a different query than GetModulePower --
hook MobileBluetooth client-side (BTSessionAttachWithQueue +0x89d88c, the available/powered
getters) to see exactly what it reads; (b) the attach timing/re-attach; (c) confirm the ON
encoding BluetoothManager expects for BTPowerMode/powerState. The patches are gated behind
bt_force_on=1 (off by default). This is deep bluetoothd/BluetoothManager RE; the core MBF
access blocker is solved, the powered-state read remains. Remaining gaps from
the baseline: pair confirmation (User_Confirmation_Request needs a tap on both sides),
link-key storage, profiles A2DP/HFP/HID + audio, LE GATT connect.

## Device test (coordinate first)

1. Set `BT_A2=1` for the build. This stages the app as
   `/Applications/bt_test.app` in `system.apfs`, where the existing LaunchServices
   scan discovers it, and puts the ordinary CLI at `/redwood/bin/bt_test` in
   `root.apfs`, instead of the historical BT client. Boot with *both* new
   images; updating just the payload on a resident eMMC system leaves the old
   `/Applications`. Keep the default SpringBoard/lsd/runningboardd setup; retain
   the known BT=1 daemon bring-up, HCI/kernel settings and advertiser setup
   from the current baseline. Ensure the stock `com.apple.tccd` job is present
   (add it through `MORE_JOBS` if needed); do not change TCC policy or grants.
2. Use `RC_JOBS=src/bt_app.rc` to run `/redwood/bin/bt_test 20` and keep the
   system up for the app arm (`rc.jobs.bt1` ends the boot immediately after the
   CLI). Keep the device unlocked during both scan windows and a known BLE
   advertiser continuously on. After the CLI finishes, open **Redwood BLE Test** from
   the SpringBoard icon, or request `/redwood/bin/battery_test launch com.redwood.bt_test`
   via the project's existing SpringBoardServices launcher. Do not exec the app
   bundle's executable as a substitute. Verify the visible app or the
   `bt_a2: UIKit active` line; a successful launch request alone is not proof.
3. If asked, choose Allow promptly and record that choice and prompt timing.
   Keep the app foreground. The unchanged scanner runs once per app process
   with the same argument 20 and no service filter in both arms (the scanner
   also has its existing six-second diagnostic wait). If the prompt consumed
   significant time, repeat with a fresh app process after the user has granted
   access. The app displays the report, writes it to `NSTemporaryDirectory()/bt-a2.log`
   and emits it to unified logging. Give enough extra boot time for both runs.
4. Preserve bluetoothd logs for both windows, including early registration:
   enable its existing OSLOG setting and collect continuously if possible,
   since the console ring wraps. Correlate each scanner PID with bluetoothd's
   session label, `[Server.App] Registering central session`, `getReadyForTCC`,
   `canScanNow`, scan level, advertisements delivered and client discoveries.
   Record CoreBluetooth state/authorization and bluetoothd PID changes. Do not
   interpret the local NSBundle ID as evidence of bluetoothd identity.

If bluetoothd changes from `UNKNOWN-…` to the bundle ID and registers the app
session, that supports the launch-context identity hypothesis; it is not by
itself evidence that identity explains missing advertisements. If both sessions
are scanning and delivery remains zero after real app identification, continue
below app identity. A denied/no-prompt CLI, differing authorization or daemon
restart confounds delivery comparisons; still report any usable identity lines.
If registration or real launch fails, report packaging-only/launch failure,
not a negative A2 result.
