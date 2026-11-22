# Redmi 9 (lancelot): audio backend — confirmed hardware & implementation plan

From subagent audio-research (read-only: phone DT dump, DTBO parsed in memory,
open MiCode/Xiaomi_Kernel_OpenSource branch lancelot-q-oss). **Corrects the earlier
draft: the speaker is an ANALOG Awinic AW87519 on MT6358 LINEOUT L, NOT an I2S/DSP
smart-PA; I2S out 3 is not used for lancelot's speaker.**

## Confirmed hardware
- **Speaker amp: Awinic AW87519**, analog input. DT `i2c7@11004000 → aw87519@58`,
  compatible `awinic,aw87519_pa`, **reset-gpio GPIO22**, I2C 400 kHz,
  SCL=GPIO175, SDA=GPIO176. Chip ID **0x59** in reg 0x00. Vendor driver
  aw87519_audio.c is a plain I2C driver (8-bit regs, no ASoC DAI); profiles
  kspk/drcv/hvload. CONFIG_SND_SOC_AW87519=y.
- **Speaker route (analog): DL1 → ADDA DAC → MT6358 LINEOUT L (LOL) → AW87519 →
  speaker.** Machine mt6768-mt6358.c DAPM `Ext_Speaker_Amp` ← "LINEOUT L";
  POST_PMU → aw87519_audio_kspk(), PRE_PMD → off(). `speaker_amp@34` is MTK's
  generic probe (finds nothing on lancelot → dummy codec); `sound-dai=<0x5e>` is a
  dangling phandle. CS35L41/I2S3 is for merlin (Redmi Note 9), not lancelot — that
  explains the Cirrus/NXP entries in the shared SmartPa_AudioParam.xml.
- **Mic: MT6358 ADC**, AMIC, MTKAIF protocol 1 (no calibration). Main mic = ADC1(L)
  AIN0/AU1, MICBIAS0=1.9 V (AUDENC_ANA_CON9=0x0021); ref mic = ADC2(R) AIN2/AU3;
  headset AIN1/AU2 MICBIAS1 2.6 V. PGA default index 3. AFE record memif = VUL12;
  ADDA UL→UL1 via AFE_CONN21 bit3 / CONN22 bit4; AUD_PAD_TOP=0x31, MTKAIF_CFG0=0.
- **AFE 0x11220000** (regmap 0x1000), IRQ GIC SPI 169, DMA mask 34-bit;
  audio_sram 0x11221000 (optional; DRAM buffer allowed). Port has no IOKit IRQs —
  drivers poll (thread_call); start with polling DL1_CUR/VUL12_CUR.

## Key registers
- DL1 memif: BASE 0x040 / CUR 0x044 / END 0x048, MSB 0xb00/0xb04/0xb08;
  fs AFE_DAC_CON1 0x014[3:0] (**48 kHz = code 10**), mono bit21, enable
  AFE_DAC_CON0 0x010 bit1; HD 0x3f8 bit0; pbuf 0x3d8[1:0], minlen 0x3d0[3:0].
- VUL12: BASE 0x350 / CUR 0x35c / END 0x358, MSB 0xb80/0xb8c/0xb88;
  fs DAC_CON0[23:20], mono bit10, enable bit9, HD bit12.
- IRQ: MCU_CON0 0x3a0(en), CON1 0x2e4(fs), CNT0/1/2 0x300/0x3ac/0x3b0(=period,18bit),
  EN 0x3b4, STATUS 0x3a4, CLR 0x3a8. Buffer 16-byte align, playback zeroed;
  pbuf=3 (256 B) for period >10 ms.
- Clocks (48 kHz): one APLL. APLL1=196.608 MHz (apmixed 0x1000c000: PCW 0x310=
  0x078FD526, postdiv CON1 0x30c[26:24]=0, PWR 0x318 bit0=1, tuner AP_PLL_CON3
  0x0c bit0=1). topckgen 0x10000000: CLK_CFG_3 0x70[25:24] audio_sel=0(26M),
  CLK_CFG_4 0x80[1:0] aud_intbus=1, bit8 aud_1=1(apll1), [17:16] aud_engen1=3,
  CLK_AUDDIV_0 0x320[27:24]=7 (÷8 → 24.576M). audiosyscon 0x11220000 reg 0x000
  (RMW): bit2 aud_afe, bit9 aud_24m, bit19 tuner, bit24 adc, bit25 dac,
  bit26 dac_predis, bit27 tml. infracfg 0x1020e000: CG_1 SET 0x88 bit25,
  CG_2 SET 0xA4 bit4. AFE APLL_TUNER_CFG 0x3f0=0x434|1, HD_ENGEN 0xdd0 bit1.

## Reused HakiOS infrastructure
- Sink contract redwood_audio.h: `uint32_t sink(const int16_t*, uint32_t)`,
  non-blocking, called under audio_lock; volume via redwood_audio_volume()
  (eighths dB); rec_sound/rec_sound_fed/rec_sound_mic verification already exists.
  HAL audio_plugin.c unchanged (Speaker 48k stereo s16le → /dev/redwood-audio;
  Codec mono input ← /dev/redwood-mic).
- PWRAP MT6358 ready (mtkpwrap.h WACS2, 16-bit regs), proven in mtkbatt.
- GPIO pattern proven in redwood_fts.c (0x10005000, DIR_SET 0x004+0x10·bank,
  DOUT_SET 0x104, DIN 0x200, MODE 0x300+0x10·(pin/8)). Bit-bang I2C + the
  prepare/start/volume/stop/quiesce skeleton from redwood_speaker.c (its AW88261
  registers do NOT apply; only the framework).
- /dev/redwood-mic ring (16384 mono frames, RING_KEEP 40 ms) in redwood_mic.c
  reused as-is.

## Implementation: new bsd/dev/redwood_mtkafe.c, boot-arg mtkafe=N
- **Stage 0 (read-only Android, when phone free):** tinymix MTK_SPK_TYPE_GET
  (expect 0=NOT_SMARTPA), Ext_Speaker_Amp Switch, ADC1/ADC2 Mux (AU1 main, AU3 ref),
  aw87519 chip ID via sysfs reg 0x00=0x59; pull /vendor/firmware/aw87519_kspk.bin,
  SmartPa_AudioParam.xml, MicInfo_AudioParam.xml.
- **Stage 1 (mtkafe=1, read-only):** clock state (topckgen/apmixed/audiosyscon/
  infracfg), AFE regs, PWRAP AUD_TOP_ID 0x2200, GPIO22/175/176, I2C chip ID. Enable nothing.
- **Stage 2 (clocks + digital, no analog):** infra CG → top mux → APLL1 →
  audiosyscon gates → AFE tuner/24M. DL1 ring: kmem_alloc_contig, 8 periods × 480
  frames (10 ms), 16-byte align, zeroed; BASE/END/MSB; DAC_CON1[3:0]=10, bit21=0;
  HD bit0=0; pbuf/minlen=3. Route AFE_CONN3 0x002c bit5=1, CONN4 0x0030 bit6=1
  (DL1→ADDA_DL). ADDA DL: PREDIS_CON0/1 0x260/0x264=0; DL_SRC2_CON0 0x108=
  0x83001803; DL_SRC2_CON1 0x10c=0xf74f0000 (−0.3 dB); SDM_DCCOMP_CON 0xc50[5:0]=0x1d;
  UL_DL_CON0 0x124 bit0=1. MT6358 digital (PWRAP): GPIO_MODE2 0xd8=0x0249,
  SMT_CON1 0x30=0x0ff0, DRV_CON3 0x3c=0x8888; DCXO_CW14 0x7ac bit13=1;
  AUDENC_ANA_CON6 0x2394=0x0001; AUDDEC_ANA_CON13 0x2422 bit4=0;
  AUD_TOP_CKPDN_CON0 0x220c mask0x66=0; PMIC_AUDIO_TOP_CON0 0x2292 mask0xc5=0 (250µs);
  AFUNC_AUD_CON2 0x229a 0x6→0x3→0xB; AFUNC_AUD_CON0 0x2296=0xCBA1;
  AFE_UL_DL_CON0 0x2288=1(mask0x4001); AFE_DL_SRC2_CON0_L 0x228a=1; PMIC_AFE_TOP_CON0 0x2290=0.
- **Stage 3 (analog + amp, tone at min volume):** MT6358 LOL (Speaker_Amp_Change):
  AUDDEC_ANA_CON2 0x240c=0x4000; NCP CLKDIV_CON1 0x223c=1, CON2 0x223e=0x2c,
  CON0 0x223a=1, CON4 0x2242=3, CON3 0x2240=0 (250µs); CON14 0x2424|=0x1055,
  CON15 0x2426=1 (100µs); ZCD_CON0 0x2488=0; CON7 0x2416 0x0010→0x0110→0x0112→0x0113;
  CON12 0x2420=0x0055, CON11 0x241e=0x4900; CON9 0x241a=0, CON10 0x241c=0;
  **gain ZCD_CON1 0x248a=(g<<7)|g, start g=0x1f (−40 dB)**, ramp to g=18 (−10 dB);
  CON13 bit0=1; CON0 0x2408=0x0009; CON9=0x0001; CON7=0x011b (LOL mux→DAC).
  AW87519: GPIO22 out low 2 ms→high 2 ms; reg 0x64=0x2C; check 0x00==0x59; write
  kspk profile (21 pairs): 69:80,69:B7,01:F0,02:09,03:E8,04:11,05:10,06:43,07:4E,
  08:03,09:08,0A:4A,60:16,61:20,62:01,63:0B,64:C5,65:A4,66:78,67:C4,68:90.
  Off: reg 0x01=0x00 + GPIO22 low. I2C bit-bang GPIO175/176 (redwood_fts.c pattern).
  **Volume:** AW87519 has no digital volume → map redwood_audio_volume() to LOL PGA
  ZCD_CON1 (~1 dB steps, idx8=0 dB … 31=−40 dB) + ADDA_DL_GAIN 0x10c fine trim;
  stepped ramp. **Speaker protection:** no DSP → reuse redwood_guard.c peak/RMS
  limiter, conservative thresholds, ceiling −10 dB LOL until physically verified.
- **Sink/DMA:** redwood_audio_set_sink("mtk-afe", fn); fn copies frames into DL1
  ring, free space from DL1_CUR (0x044), no sleep/no extra playback thread, drop
  excess (lost accounting exists). Start DL1_ON bit1 + IRQ0 counter=480 fs=10 EN
  bit0 (or poll). Stop/panic: DL1 off → LOL −40 dB → DAC off → AW87519 off → clock
  gate (quiesce without sleep).
- **Mic (separate stage, mtkmic=N):** AFE AUD_PAD_TOP 0xe40=0x31, MTKAIF_CFG0
  0xe00=0, ADDA_TOP_CON0 0x120 bit0=0, UL_SRC_CON0 0x114=0x60401, IIR coeffs
  0x290…0x2a0 (35 Hz@48k), MTKAIF_RX_CFG0 0xe20 bit0=0 (AMIC); route CONN21 bit3 /
  CONN22 bit4 → UL1 or VUL12. MT6358 (TurnOnADcPowerACC): AUDDEC_ANA_CON13 bit5=1,
  AUDENC_ANA_CON3 0x238e=0, CON14|=0x2500 (LCLDO_ENC 1.8 V); **MICBIAS0
  AUDENC_ANA_CON9 0x239a=0x0021 (1.9 V) only after route confirmed**; PGA idx3;
  ADC1 CON0 0x2388 0x0041→0x5041; GPIO_MODE3 0xde=0x0249; PMIC_AUDIO_TOP_CON0
  mask0xbf=0; PMIC_AFE_TOP_CON0=0; AFE_UL_DL_CON0=1; PMIC_AFE_ADDA_MTKAIF_CFG0
  0x22b4=0; AFE_AUD_PAD_TOP 0x22cc=0x3100; AFE_UL_SRC_CON0_H 0x228c=0, _L 0x228e=1.
  Drain VUL12_CUR every 10 ms (thread_call), downmix L→mono, ring_put into the
  existing /dev/redwood-mic ring; raise only while device open (like mic=4).

## Verification gates
- 1 kHz tone at −40 dB LOL → rec_sound + physical listen; mute/route check.
- Mic separately (non-silent samples, 48k). Do not touch VAUD28/VIO18 rails
  (loader already enables them). Read back every write; on mismatch, full stop.

## Stage 1 probe VERIFIED on lancelot (run mtkafe1, redwood_mtkafe.c mtkafe=1)
Read-only, nothing enabled. Confirms the addresses are correct and the hardware
is accessible:
- topckgen CLK_CFG_3 0x10101, CLK_CFG_4 0x30101, CLK_AUDDIV_0 0x770000ff (audio
  clocks at default, not on APLL yet).
- apmixed APLL1 PCW 0x78fd5265, postdiv 0x4000000, PWR 0x2 (APLL1 not powered for
  audio yet); infracfg CG_1/CG_2 = 0 (audio infra gated).
- AFE/audiosyscon reg0 0xa0fd4038, DAC_CON0/1 = 0 (DAC off), AUD_PAD_TOP 0x30
  (plan wants 0x31), APLL_TUNER 0, HD_ENGEN 0.
- **MT6358 AUD_TOP_ID (0x2200) = 0xc800 — the codec is readable over PWRAP.**
  AUDDEC_ANA_CON13 0x0010, MICBIAS0 (0x239a) 0, ADC1 PGA (0x2388) 0,
  PMIC_AUDIO_TOP_CON0 0 (audio top off, as expected at boot).
- GPIO22 (amp reset) input, reads high; GPIO175/176 (I2C7) input, high (pull-ups).
**AW87519 confirmed (run mtkafe2, mtkafe=2): chip id (reg 0x00) = 0x59 "as
expected".** The MT6768 I2C bit-bang on GPIO175/176 (open-drain via DIR/DOUT,
NOT the Qualcomm TLMM of redwood_speaker.c) works and the amplifier acknowledges
at 7-bit address 0x58. So the whole audio chain is confirmed present and
accessible: MT6358 codec (PWRAP, AUD_TOP_ID 0xc800), AW87519 amp (I2C7, 0x59),
AFE/clock registers readable. **Stage 2 clocks partially enabled (run mtkafe3, mtkafe=3):** the audio muxes are
clr/set immediate-effect (INVALID_UPDATE_REG in clk-mt6768.c; parents confirmed:
audio_sel 0=clk26m, aud_intbus 1=syspll1_d4, aud_1 1=apll1_ck, aud_engen1
3=apll1_d8). After clock_enable: **APLL1 PCW 0x78fd526, postdiv 0, PWR 0x80000003
(powered)**, audiosyscon reg0 0xa0fd4038→0xaffd423c (gates set), AFE APLL_TUNER
0x435, HD_ENGEN 0x2 — these took effect, clean return. CLK_CFG_4 reads 0x30101
(aud_intbus/aud_1/aud_engen1 correct); CLK_CFG_3 audio_sel reads 1 and infracfg
CG reads 0 — those writes need re-checking (SET-register readback semantics).
Next: verify the mux/infra writes, then DL1 ring + ADDA DAC digital path, stage 3
(LOL analog + AW87519 kspk profile + tone at −40 dB), then the microphone.

## Stage 4 implementation (redwood_mtkafe.c, mtkafe=4 tone / mtkafe=5 sink)
- DL1 ring: kmem_alloc_contig (3840 frames = 8x480, 16-byte align, zeroed), phys via
  kvtophys, CPU writes through a write-combine alias (ml_io_map_wcomb) so the samples
  land in DRAM for the AFE DMA. BASE 0x040 / END 0x048 / MSB 0xb00/0xb08/0xb04.
- **Critical fix (run audio4 first attempt: CUR stuck at 0):** AFE_DAC_CON0 (0x010)
  **bit0 is the master AFE enable**, bit1 is DL1_ON. Setting only bit1 (0x2) leaves the
  whole AFE digital block off, so the memif never clocks and DL1_CUR stays 0 even though
  every register reads back correctly. Vendor mt6768_afe_runtime_resume sets bit0
  (`regmap_update_bits(AFE_DAC_CON0, 0x1, 0x1)`). Now set bit0 then bit1 (0x3) and verify
  AFE_DAC_MON (0x2ec) bit0 AFE_ON_RETM == 0 (AFE out of retention).
- Also from runtime_resume: AFE_CONN_24BIT (0x06c) = 0xffffffff, AFE_CONN_24BIT_1
  (0x0994) = 0xffffffff (24-bit output ports), AFE_MEMIF_MSB (0x0cc) bit28 = 0 (force
  8_24 memif format), AFE_AUD_PAD_TOP (0xe40) bit0 = 1.
- Clocks: the audio gates are audiosyscon AUDIO_TOP_CON0 (reg0) bits 2/9/19/24/25/26/27
  (no_setclr, RMW) -- confirmed against clk-mt6768.c audio0_cg_regs (sta_ofs 0). The
  top muxes CLK_CFG_4 aud_intbus[1:0]=1, aud_1[8]=1, aud_engen1[17:16]=3 (immediate
  clr/set). There is NO named infra audio gate in clk-mt6768.c; the earlier infra
  0x88/0xa4 writes are not the audio bus gate and reading those SET regs returns 0.
- **APLL1 enable (run audio4 second attempt: CUR still 0 even with AFE master on):** the
  vendor PLL offsets (clk-mt6768.c) are APLL1_CON0=0x308 (PLL_EN bit0), CON1=0x30c
  (postdiv[26:24]), CON2=0x310 (PCW), CON4=0x318 (PLL_PWR_ON bit0 / PLL_ISO_EN bit1),
  tuner_en=0x0c bit0 (AP_PLL_CON3). Powering (0x318 bit0) is NOT enough: PWR read
  0x80000003 = PWR+ISO+ack. Must clear ISO (0x318 bit1), then set PLL_EN (0x308 bit0),
  wait ~20us to lock. Without PLL_EN the APLL1 outputs nothing, aud_engen1 (apll1_d8 =
  24.576 MHz) is dead, the ADDA DL has no sample clock, and DL1_CUR stays 0. The earlier
  draft's "APLL1 PWR 0x318 bit0" alone was incomplete.
- **IRQ0 paces the memif (run audio4 third attempt: CUR still 0 with APLL1 enabled):** the
  DL1 memif DMA does not free-run on DL1_ON alone. The vendor trigger (mtk_afe_fe_trigger)
  sets IRQ0 after DL1_ON: AFE_IRQ_MCU_CNT0 (0x300) = period frames (480 = 10 ms),
  AFE_IRQ_MCU_CON1 (0x2e4)[3:0] = fs (10 = 48k), then AFE_IRQ_MCU_CON0 (0x3a0) bit0
  IRQ0_MCU_ON = 1. Without IRQ0_MCU_ON the memif cursor stays 0. END is programmed as
  base + bytes - 1 (last byte), per mtk_memif_set_addr.
- **Infra audio bus gate (run audio4 CONN probe: CONN3/4/0x998 all read 0, ignore writes):**
  the AFE interconnect (CONN matrix) sits on the infra audio bus, gated by CLK_IFR_AUDIO
  (GATE_IFR3 bit25: SET 0x88, STA 0x94) and CLK_IFR_AUDIO_26M_BCLK (GATE_IFR4 bit4: SET
  0xa4, STA 0xac) in infracfg 0x1020e000. With these gated the CONN registers read 0 and
  ignore writes, DL1 cannot route to ADDA_DL, and DL1_CUR stays 0. Enable both (SET, then
  verify STA bit25/bit4; fallback write STA directly). This is the gate the early draft
  called "infra CG_1/CG_2" -- the offsets were right but the readback used the SET regs
  (always 0) instead of STA, hiding that the bus was off.
- **CORRECTION: infracfg base is 0x10001000** (SCP/mtkcam), not the draft's 0x1020e000.
  The wrong base read 0 and ignored writes, so the audio-bus gate never enabled and CONN
  was inaccessible in every earlier run. Fixed in redwood_mtkafe.c.
- MT6358 digital + LOL analog + AW87519 kspk profile as in the plan above; LOL gain
  ZCD_CON1 (0x248a) = (g<<7)|g, g=0x1f (-40 dB) start. Every PMIC write read back.
- Verification: autotest profile audio4 checks clocks, codec digital, LOL gain 0x0f9f,
  amp profile, DL1 on (DAC_CON0 0x3), AFE master on, and **DL1_CUR advances** over the
  tone (proves the DMA actually reads the ring). Physical listen still required.
## Status (autotest audio4) -- SPEAKER PLAYBACK DMA WORKING (12/12 PASS)
The full DL1 -> ADDA DAC -> MT6358 LOL -> AW87519 -> speaker chain runs: autotest audio4
12/12 (run 20261121-235646): APLL1 powered+enabled, MT6358 digital path, LOL gain 0x0f9f
(-40 dB), AW87519 kspk profile, DL1_ON (DAC_CON0 0x3), IRQ0 MCU on, CONN3 0x20 / CONN4 0x40
(DL1->ADDA_DL both channels), AFE active (DAC_MON bit0=1), and **DL1 DMA advanced**
(CUR 0xc52cc190 -> 0xc52cc260), ADDA_SRC_MON0 0x11ae (ADDA receiving data), IRQ0_CNT_MON
0x1ad (counter counting). A 1 kHz test tone plays for mtkafe_secs (physical listen pending).
TWO active-low gate findings (the whole debug saga):
1. infra audio AXI gate CLK_IFR_AUDIO (infracfg_ao@10001000, ifr3 bit25, STA 0x94):
   default (0) = bus ON; SETting bit25 gates the AFE register + CONN bus OFF. LEFT DEFAULT.
2. audiosyscon AUDIO_TOP_CON0 (0x11220000 reg0) clock gates (afe bit2, 24m bit9, tuner
   bit19, adc bit24, dac bit25, dac_predis bit26, tml bit27): ALSO active-low. Default
   reg0 0xa0fd4038 already has the audio clocks ON; OR-ing the bits to 1 gated them OFF
   (IRQ0_CNT_MON stayed 0, ADDA_SRC_MON0 0, DL1_CUR 0). KEEP DEFAULT reg0 -- then the
   sample clock runs, IRQ0 counts, ADDA receives data, and the DL1 memif DMA reads the ring.
The audio node has NO iommus binding -> memif DMA is physical. AFE_ON_RETM (DAC_MON 0x2ec
bit0): 1 = active, 0 = retention/off (vendor polls ==0 to confirm off).
iOS sink: mtkafe=5 registers redwood_audio_set_sink("mtk-afe"); the HAL Speaker plugin is
confirmed loaded (mediaserverd opens /dev/redwood-audio). audio5 3/3. To feed the sink, added:
inject_sb.c play_system_sound (com.redwood.sound notification + sound_test=1 auto-play), and
inject_media.c set_default_speaker (CoreAudio HAL: list devices, find UID "Speaker", set
kAudioHardwarePropertyDefaultOutputDevice) run from a pthread at 20/40 s (mediaserverd services
XPC not libdispatch, and its sandbox blocks kern.bootargs).
SINK-FED VERIFIED (autotest sinkfed 4/4, run 20261122-172715, RC_JOBS=rc.jobs.sinkfed): the
earlier 'sink taken 0' was just a silent boot (no audio activity). sound_test (springboard
route tone 5 raw 3 system 1100) drives real playback: kernel logs 'redwood-audio: 8.96 s
written at 47621 frames/s, 5.95 s of it sound, peak 15%' (the iOS AudioQueue tone through
mediaserverd -> HAL plugin -> /dev/redwood-audio -> mtk-afe sink -> speaker), '3.00 s ... peak
49%' (raw tone), '2.47 s ... sound' (system sound 1100). iOS AUDIO PLAYS THROUGH THE SPEAKER.
The kernel tone path (mtkafe=4) also verified; sink + DMA + iOS routing all working. Volume: mtkafe_gain=N (LOL PGA index, cap -10 dB=0x12); audio4g 12/12 at -10 dB.
MICROPHONE RECORD DMA WORKING (mtkmic=1, autotest mic 4/4, run 20261122-004136): MT6358
ADC1/AIN0 (MICBIAS0 1.9V, PGA idx3) -> ADDA UL -> VUL12 memif (record DMA) -> mono drain
-> redwood_mic_feed -> /dev/redwood-mic ring (redwood_mic_external_up, no DSP). VUL12 CUR
advances, ~48k mono frames/s fed (142168 -> 426036). Same AFE-master bit0 fix as DL1.
Mic content (non-silent) + iOS recording need physical/interactive verification.
NEXT: physical listen/record verification (user); mic to iOS apps (record path ready).

## Limits
- No local vendor audio_param/firmware dumps — AW87519 profile from driver source;
  confirm exact main-mic AUx and aw87519_kspk.bin presence on the phone (stage 0).
