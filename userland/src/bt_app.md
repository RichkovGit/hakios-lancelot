# Bluetooth A2: client identity

Run from `userland`. `BT_A2=package ./build.sh` only cross-builds
`obj/bt_test.app` and `obj/bt_test_a2_cli`; it neither builds an image nor
registers or launches anything. Both Mach-O files have plain ad-hoc signatures,
without the legacy CLI's Bluetooth TCC entitlement. The app has a standard
`NSBluetoothAlwaysUsageDescription` and requests access through CoreBluetooth.

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
