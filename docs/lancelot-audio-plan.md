# Redmi 9 (lancelot): audio bring-up plan

Status: **not working**. The HAL plug-in opens its endpoint and iOS routes to it,
but there is no MediaTek sink yet, so nothing is heard and the microphone input
is silent. This plan records what exists and what must be built; it is not a
claim of support.

## What already exists (reuse)

- `userland/src/audio_plugin.c` — the AudioServerPlugIn that presents Speaker /
  Codec / Actuator / Hawking to mediaserverd and writesSpeaker samples to
  `/dev/redwood-audio` (48 kHz, stereo, s16le). Volume ioctl
  `REDWOOD_AUDIO_SET_VOLUME`. This layer is SoC-independent and stays.
- `bsd/dev/redwood_audio.c` — the endpoint and the sink interface
  `redwood_audio_set_sink(name, fn)`, plus the host recording (`rec_sound`).
  A new sink registers here; nothing above it changes.
- `bsd/dev/redwood_speaker.c` — Awinic amplifier bring-up from Android's own
  `.acf` configuration (`fw-audio/aw882xx_acf.bin` is staged by build.sh):
  prepare/start/volume/healthy/stop/quiesce. The amplifier side may be partly
  reusable once the actual lancelot amp is confirmed.

## What is Qualcomm-specific and does NOT apply to lancelot

- `bsd/dev/redwood_adsp.c`, `redwood_sound.c`, `redwood_mic.c` use the Qualcomm
  LPASS/ADSP APR path (`APR_SVC_AFE`, ASM/ADM, SoundWire, codec-lpass clocks).
  The Redmi 9 is MediaTek MT6769T/MT6768 with an MT6358 PMIC codec; there is no
  LPASS APR. These must be replaced by a MediaTek backend, not adapted.

## What must be built (MediaTek backend)

1. **Confirm the board route from the phone (read-only).** Identify the actual
   speaker amplifier (device tree lists `speaker_amp@34` and `aw87519@58` on
   I2C7; vendor DTS gives AW87519 reset GPIO 22) and the microphone route
   (MT6358 ADC channels, AMIC bias). Read Android's `/proc/device-tree`,
   `tinymix`/`audio_policy` configuration and the vendor audio HAL params to
   fix the exact amp part, I2C bus/address, reset GPIO, I2S port and mic bias.
   Do not guess register values or enable bias blindly.
2. **MT6768 AFE** at `0x11220000`: clock/PLL enable, I2S out port 3 (speaker)
   and the capture path, DMA ring for playback frames and capture frames,
   interrupt or polling drain. Register sequences taken from the vendor
   `mt6768` AFE driver studied locally (not copied into the product).
3. **MT6358 codec via PWRAP**: playback DAC path and volume, ADC path and mic
   bias for capture. The loader already has PWRAP helpers (`mtkpwrap.h`).
4. **Amplifier**: reset GPIO, I2C probe, power-up/unmute at the lowest volume,
   fade-out/power-down on stop and quiesce, fault handling. Reuse
   `redwood_speaker.c` structure if the part matches its Awinic flow.
5. **Sink**: a `redwood_audio_set_sink("mtk-afe", ...)` that feeds the AFE DMA
   ring without blocking the writer; a capture source for `/dev/redwood-mic`
   so the Codec input returns real samples.

## Verification gates (independent, low risk first)

- Speaker: play a known tone at the lowest volume, confirm with the host
  recording (`rec_sound`) and a physical listen; verify route change and mute.
- Microphone: capture separately with normal app permission; do not infer
  capture from playback. Confirm non-silent samples and correct rate.
- No charger/rail voltage changes; amplifier bias only after the route is
  confirmed from the phone.

## Confirmed from the phone (read-only, Android)

- Machine driver: `/proc/device-tree/sound` compatible
  `mediatek,mt6768-mt6358-sound`; AFE `audio@11220000`
  (`mediatek,audiosyscon`), `audio_sram@11221000`. Card name `mt6768mt6358`.
- **CORRECTED by detailed research — see [lancelot-audio-backend.md](lancelot-audio-backend.md).**
  The speaker amp is an **Awinic AW87519 with ANALOG input** on MT6358 LINEOUT L
  (DL1 → ADDA DAC → LOL → AW87519 → speaker), reset GPIO22, I2C7@0x58, chip ID 0x59.
  **I2S out 3 is NOT used for lancelot's speaker** (that is merlin/Redmi Note 9).
  The Cirrus/NXP/Richtek entries in SmartPa_AudioParam.xml and the DSP-style
  audio_device.xml controls are a shared vendor framework, inactive on lancelot
  (`speaker_amp@34` finds nothing; `sound-dai=<0x5e>` is a dangling phandle).
  Mic is MT6358 ADC1(L)/AIN0 main, MICBIAS0 1.9 V, MTKAIF protocol 1. The backend
  doc has the full confirmed register sequences and a staged implementation plan
  (new redwood_mtkafe.c, boot-arg mtkafe=N).

## Next concrete step

When the phone is free, run a read-only Android inspection (device tree audio
nodes, I2C7 devices, vendor audio param files) to fix the amp part and mic
route, then implement the AFE sink incrementally with a host-recorded tone test.
