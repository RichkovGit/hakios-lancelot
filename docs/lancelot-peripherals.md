# Redmi 9 (lancelot): peripheral bring-up queue

Read-only inventory (subagent peripherals-inventory). The Qualcomm drivers
`redwood_haptic.c` / `redwood_torch.c` are for POCO X5 Pro and do NOT apply to
lancelot, but their iOS-side ABI is SoC-independent and already written.
**Do not enable `haptic=` / `torch=` boot-args on lancelot — they map Qualcomm
addresses; new MTK backends need their own boot-arg values.**

## Priority 1 — SCP accelerometer → screen rotation (code ready)
- `bsd/dev/redwood_scp.c` is already MT6768 and in the build (`bsd/conf/files`,
  `redwood_scp_init()` in redwood_input.c). On Infinix X6826B it reached step 7
  (LK loads SCP firmware, driver starts it, IPI sensor hub, nanohub accel 50 Hz,
  DRAM FIFO, `kern.redwood.accel`).
- lancelot hardware matches: `scp@10500000`, SPI 0xCD, SCP reserved
  0x9f900000+0x600000 (same as X6826B, below the XNU window).
- Screen rotation already works from `kern.redwood.accel` (inject.c reads it
  5×/s into BKOrientationManager; `orientation=0` disables).
- **WORKING on lancelot (logs/lancelot-scp7b.log).** LK left the DRAM size unset
  (0xffffffff), which tripped the step-2 guard; fixed in redwood_scp.c by clamping
  an unset size to the SCP reserved region (0x9f900000+0x600000 → DRAM
  0x9f980000+0x580000). With `scp_step=7`: step 2 starts the SCP (PC runs from
  0xfffffffe → 0x8fd2 → 0xd564), step 3 reads its messages, step 4 sets the DRAM
  FIFO, step 5 enables the accelerometer at 50 Hz (`nanohub config sensor 1
  enable 1 rate 51200: taken 1`), step 6 reads **real samples: x −622 y 473
  z 9799** (z ≈ 1 g, phone flat), updating each second; step 7 takes messages
  every 10 ms into `kern.redwood.accel`. The FIFO at 0x4e000000 (size 262108)
  works. **Screen rotation now applied (run scp7c).** The manager instance was
  not captured because backboardd never called the three activation methods;
  fixed by also hooking BKOrientationManager `init` and
  `_queue_updateDeviceOrientationFromCoreMotion:` to capture `self` (inject.c).
  Now: "orientation: 5 from x −696 y 505 z 9787" and "last sent 5, seen 5,
  1 changes; manager 0x12082c5a0" — the orientation (5 = face up, phone flat) is
  computed from the accelerometer and sent to backboardd's manager. The
  accel → orientation → backboardd pipeline is verified end-to-end; a physical
  turn changes the orientation and rotates the screen.
- Light/proximity sensors share the SCP (nanohub types beyond accel need parsing).

## Priority 2 — Torch / flashlight (IMPLEMENTED & exercised)
- DT `flashlights_led191`: **EN=GPIO152, SEL=GPIO153** (no I2C/PMIC writes).
- **New driver `bsd/dev/redwood_mtktorch.c`** (own boot-arg `mtktorch=N`, NOT the
  Qualcomm `torch=`): 1=read-only probe, 2=light once `mtktorch_secs`, 3=iOS
  `/dev/redwood-torch` (level 0–1000). Sets the pins to GPIO mode/output, SEL low
  (torch position), EN on/off; `mtktorch_max_s` time limit (≤120), battery ≤45 °C
  guard, quiesce off at run end/panic. Registered in bsd/conf/files,
  redwood_audio_init, loader_return quiesce.
- **Probed (lancelot-torch1.log):** pins already GPIO mode (0x111100), input, low.
  **Exercised (lancelot-torch2.log, mtktorch=2 mtktorch_secs=3):** "light ON in the
  torch position for 3 s" → "off again after 3 s", clean return. GPIO driven
  correctly; **physical light to be confirmed by eye** (SEL polarity assumed
  low=torch — verify against Android if it does not light). iOS Control Center
  integration (mtktorch=3 + MobileGestalt camera-flash) is the next step.

## Priority 3 — Vibration (MT6358 VIBR LDO via PWRAP, infra ready)
- DT `vibrator@0`, `mediatek,vibrator`, vib_timer=25 vib_limit=9 vib_vol=11 →
  MT6358 VIBR LDO through PWRAP (0x1000d000), not an I2C chip.
- PWRAP WACS2 + redwood_pwrap_lock() already shared. iOS side ready: Actuator
  (Taptic) HAL writes `/dev/redwood-haptic`.
- Need an MTK backend mapping Actuator samples to timed on/off VIBR (no waveform
  RAM; volume = vib_vol). Respect vib_limit duty and Android vib_vol.

## Priority 4 — Main camera (method proven, weeks)
- Skeleton `redwood_mtkcam_skel.c` (`/dev/redwood-camera`, mtkcam_platform ops).
  Main I2C4@0x20 candidates ov13b10/s5k3l6; SENINF 0x1a040000, CAMSV, CCU — same
  MT6768 IP as X6826B. Tools: cam-probe.sh, camtable.py, camrx.py. Identify the
  real module (chip-ID under Android), capture init table + SENINF/CAMSV dump.

## Priority 5 — GPS (last, research)
- GPS in CONNSYS MT6631, STP task 2; no iOS locationd/CLLocation side at all. Defer.

## Queue
1. SCP accel → rotation (scp_step=1..7, verify FIFO 0x4e000000).
2. Torch (2 GPIO). 3. Vibration (MT6358 VIBR). 4. ALS via SCP. 5. Main camera. 6. GPS.
First steps (scp_step=1, torch GPIO read, VIBR read, camera chip-ID) are read-only.
