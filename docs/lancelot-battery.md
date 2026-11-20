# Redmi 9 (lancelot): battery & telemetry

Read-only analysis (subagent power-telemetry). V/I/T/SOC/VBUS telemetry already
works and matches Android; the charge-status nuance and missing health fields are
documented here. No charger/rail writes were made or are proposed.

## Status enums differ
- Driver/IOPS use Linux POWER_SUPPLY_STATUS: 1 charging, 2 discharging,
  3 not charging, 4 full (redwood_input.h).
- Android dumpsys uses BatteryManager: status 2 = CHARGING. So "Android status 2"
  ≠ "driver status 2".

## Why 100% status disagrees with Android
- SOC-only "full" heuristic fires before the current check:
  redwood_mtkbatt.c (~line 408): `else if (percent_hundredths >= 9950) status=4`
  overrides charging/not-charging regardless of current sign.
- Second trigger in RedwoodBattery.cpp (~line 126):
  `FullyCharged = external && (status==4 || percent==100)` — the `|| percent==100`
  lights FullyCharged at 100% even if status is fixed.
- At 100% + USB the battery may net-discharge (e.g. −96 mA on a weak port); XNU
  says full/not-charging, Android says charging (Android uses RT9471 charger
  state, which the driver does not read at all).
- Correct scheme (recommended, untested): drop the SOC-only full triggers;
  charger && current>+threshold → charging(1); charger && current≤0 → not
  charging(3); full(4) only on charge termination (sustained ≈0 at high SOC, or
  RT9471 done). Exact Android match needs read-only RT9471 state (I2C5) — a new
  read-only function; never write charger registers.

## Telemetry already published (read-only pwrap, matches Android)
- SOC% (RTC_AL_MTH seed + FGADC_CAR delta), cell voltage (BATADC ch0),
  signed battery current (FGADC_CUR_CON0), cell temperature (BAT_TEMP ch3 +
  VBIF28 ch11 NTC), charger presence (CHR_CON0 bit5 CHRDET), VBUS (VCDT ch2).
- IOKit Temperature is in hundredths °C (3150 = 31.50); sysctl in whole °C.
  Early Temperature=2500 was the 25 °C fallback before the VBIF channel fix.
- AdapterDetails.Current=500 mA is a fixed assumption (USB_CURRENT_UA), not a
  measurement; Watts = measured VBUS × 500 mA. FamilyCode=USBUnknown (usb_type=0).

## Missing on the MTK path (not obtainable from MT6358 FGADC directly)
- charge_uah/full_uah/design_uah/cycles are not filled by mtkbatt (only the
  inactive Qualcomm qcom_battmgr path fills them) → no AppleRawCurrentCapacity/
  NominalChargeCapacity/DesignCapacity/CycleCount in IOPS. No health field.
- Safe additions: design_uah = 5020000 (DT constant) → DesignCapacity 5020 mAh;
  charge_uah derivable from seed% + CAR delta; health as a derived NTC-temperature
  enum. Real learned-full SOH / cycles only referenceably from Android BMS.

## Verification gaps (never covered in logs)
- All status lines are ~99.9% status 4; no charging/not-charging/discharging,
  no partial SOC, no CHRDET=0 (unplugged) ever observed.
- Plan A: partial charge (40–80%) + USB → expect status 1/3 by current sign,
  IsCharging/FullyCharged accordingly; cross-check Android dumpsys + power_supply.
- Plan B: unplug USB → status 2, ExternalConnected=no, Battery Power, gauge
  negative, SOC falling; serial may drop with USB — use persistent/loader log.
- One IOPS reader (rc.jobs.lancelot-battery); battery_test watch N + battery_test 1.
