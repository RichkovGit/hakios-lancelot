# HakiOS lancelot autotest dashboard

Latest run per profile (tools/autotest-summary.py).

| Profile | Run | PASS | FAIL | UNKNOWN |
| --- | --- | --- | --- | --- |
| hwprobe | 20261121-232931-hwprobe | 7 | 0 | 0 |
| audio3 | 20261121-230059-audio3 | 6 | 0 | 0 |
| audio4 | 20261121-235646-audio4 | 12 | 0 | 0 |
| audio4g | 20261122-001936-audio4g | 12 | 0 | 0 |
| audio5 | 20261122-024401-audio5 | 4 | 0 | 0 |
| sinkfed | 20261122-172715-sinkfed | 4 | 0 | 0 |
| mic | 20261122-203629-mic | 5 | 0 | 0 |
| conn | 20261121-223151-conn | 6 | 0 | 0 |
| demo | 20261121-233904-demo | 5 | 0 | 0 |
| btapp | 20261122-023034-btapp | 5 | 0 | 0 |
| btsettings | 20261122-164107-btsettings | 7 | 0 | 0 |
| perf | 20261122-030320-perf | 5 | 0 | 0 |
| native | 20261122-030840-native | 4 | 0 | 0 |
| nativefps | 20261122-182824-nativefps | 6 | 0 | 0 |
| fps | 20261122-175251-fps | 4 | 0 | 0 |
| nativefps2 | 20261122-184112-nativefps2 | 6 | 0 | 0 |

**Overall: 98 pass / 0 fail / 0 unknown across 16 profiles.**

## hwprobe  (2026-11-21T23:35:29)
- [PASS] kernel-boot - XNU boots and returns cleanly to Android
- [PASS] accel-scp - SCP sensor hub brings up and runs
- [PASS] accel-data - real accelerometer x/y/z samples
- [PASS] torch-probe - flashlight GPIO probe (EN/SEL)
- [PASS] audio-codec - MT6358 codec readable over PWRAP (AUD_TOP_ID 0xc800)
- [PASS] audio-amp - AW87519 speaker amp chip id 0x59 on I2C7
- [PASS] battery - battery telemetry SOC + voltage

## audio3  (2026-11-21T23:06:58)
- [PASS] kernel-boot - XNU boots and returns cleanly
- [PASS] apll1-powered - APLL1 powered for 48k audio (PWR bit0)
- [PASS] apll1-enabled - APLL1 PLL enabled (CON0 bit0)
- [PASS] audiosyscon-gates - audiosyscon audio gates set
- [PASS] afe-tuner - AFE APLL tuner + HD engen enabled
- [PASS] audio-amp - AW87519 amp still acknowledges

## audio4  (2026-11-22T00:03:40)
- [PASS] kernel-boot - XNU boots and returns cleanly
- [PASS] apll1-powered - APLL1 powered for 48k audio
- [PASS] apll1-enabled - APLL1 PLL enabled (CON0 bit0)
- [PASS] codec-digital - MT6358 digital playback path set
- [PASS] lol-gain - LOL analog up at -40 dB (ZCD_CON1 0x0f9f)
- [PASS] amp-profile - AW87519 kspk profile written
- [PASS] dl1-on - DL1 memif enabled (DAC_CON0 bit1)
- [PASS] irq0-on - IRQ0 MCU on (paces DL1 memif)
- [PASS] conn-route - DL1->ADDA_DL interconnect routed (CONN3 0x20 / CONN4 0x40)
- [PASS] afe-master - AFE master block active (AFE_ON_RETM bit0=1)
- [PASS] dma-ran - DL1 DMA consumed frames (CUR advanced)
- [PASS] tone - 1 kHz tone played and stopped cleanly

## audio4g  (2026-11-22T00:25:38)
- [PASS] kernel-boot - XNU boots and returns cleanly
- [PASS] apll1-powered - APLL1 powered for 48k audio
- [PASS] apll1-enabled - APLL1 PLL enabled (CON0 bit0)
- [PASS] codec-digital - MT6358 digital playback path set
- [PASS] lol-gain - LOL analog up at -10 dB (gain control, ZCD_CON1 0x0912)
- [PASS] amp-profile - AW87519 kspk profile written
- [PASS] dl1-on - DL1 memif enabled (DAC_CON0 bit1)
- [PASS] irq0-on - IRQ0 MCU on (paces DL1 memif)
- [PASS] conn-route - DL1->ADDA_DL interconnect routed (CONN3 0x20 / CONN4 0x40)
- [PASS] afe-master - AFE master block active (AFE_ON_RETM bit0=1)
- [PASS] dma-ran - DL1 DMA consumed frames (CUR advanced)
- [PASS] tone - 1 kHz tone played and stopped cleanly

## audio5  (2026-11-22T02:50:30)
- [PASS] kernel-boot - XNU boots and returns cleanly
- [PASS] sink-registered - iOS audio sink registered (mtk-afe)
- [PASS] dma-ran - DL1 DMA running
- [PASS] sink-fed - iOS audio fed the sink (taken>0; needs audio activity)

## sinkfed  (2026-11-22T17:40:07)
- [PASS] kernel-boot - XNU boots and returns cleanly
- [PASS] sink-registered - iOS audio sink registered (mtk-afe)
- [PASS] ios-tone-fed - iOS audio reached the sink and played (redwood-audio wrote sound)
- [PASS] audio-written - kernel redwood-audio wrote samples at 48k

## mic  (2026-11-22T20:42:44)
- [PASS] kernel-boot - XNU boots and returns cleanly
- [PASS] mic-up - mic record path up (ADC + VUL12)
- [PASS] mic-route - ADDA_UL->VUL12 routed (CONN21 bit3)
- [PASS] mic-dma - VUL12 record DMA + drain fed frames
- [PASS] mic-content - mic captures non-silent audio (peak > 0)

## conn  (2026-11-21T22:36:19)
- [PASS] kernel-boot - XNU boots and returns cleanly
- [PASS] connsys-powered - CONNSYS radio powered (step 4)
- [PASS] wifi-en0 - Wi-Fi interface en0 created
- [PASS] wifi-traffic - Wi-Fi associated + traffic carried (sent>0)
- [PASS] bt-opened - bluetoothd opens the BT device
- [PASS] bt-stable - bluetoothd stable (no abort/panic)

## demo  (2026-11-21T23:43:34)
- [PASS] kernel-boot - XNU boots and returns cleanly
- [PASS] gpu-up - Mali GPU brought up (step 6/6)
- [PASS] gpu-ran - GPU executed a job
- [PASS] springboard - SpringBoard launched (home screen)
- [PASS] no-panic - no kernel panic over the session

## btapp  (2026-11-22T02:35:00)
- [PASS] kernel-boot - XNU boots and returns cleanly
- [PASS] bt-daemon - bluetoothd opens the BT device
- [PASS] app-launch - BT app launched + registered with bluetoothd
- [PASS] tcc-bypass - TCC bypass entitlement works (tccRequired 0, no prompt)
- [PASS] app-scan - CoreBluetooth central session (app scanning)

## btsettings  (2026-11-22T16:45:18)
- [PASS] kernel-boot - XNU boots and returns cleanly
- [PASS] power-forced - PowerManager power state forced ON (patch applied)
- [PASS] mgr-hook - BluetoothManager getters forced (client-side hook applied)
- [PASS] mbf-access - MBF XPC access level forced (no insufficient-entitlements reject)
- [PASS] module-power - BluetoothManager available 1 powered 1 (client getter hook)
- [PASS] bt-powered - PowerManager reports state 1 (forced ON)
- [PASS] settings-attach - MBF SessionAttach from bt_test/Settings reaches bluetoothd

## perf  (2026-11-22T03:07:14)
- [PASS] kernel-boot - XNU boots and returns cleanly (raised clocks stable)
- [PASS] cpufreq-raised - A75 cores raised to 2000 MHz
- [PASS] gpu-raised - Mali GPU raised to 800 MHz
- [PASS] gpu-up - Mali GPU up (step 6/6)
- [PASS] springboard - SpringBoard launched at raised clocks

## native  (2026-11-22T03:12:23)
- [PASS] kernel-boot - XNU boots and returns cleanly at native + raised clocks
- [PASS] native-surface - 1080x2340 native surface allocated
- [PASS] gpu-up - Mali GPU up (step 6/6)
- [PASS] springboard - SpringBoard launched at native resolution

## nativefps  (2026-11-22T18:34:37)
- [PASS] kernel-boot - XNU boots and returns cleanly
- [PASS] iphone-x-surface - iPhone X 1125x2436 render downscaled to 1080x2340 panel (layout-safe)
- [PASS] gpu-up - Mali GPU up (step 6/6)
- [PASS] springboard - SpringBoard launched (iPhone X layout)
- [PASS] gestures - automated gestures ran
- [PASS] frames - window server frames rendered

## fps  (2026-11-22T17:59:08)
- [PASS] kernel-boot - XNU boots and returns cleanly
- [PASS] gpu-up - Mali GPU up (step 6/6)
- [PASS] gestures - automated gestures ran (rwtouch tl: p samples)
- [PASS] frames - window server frames rendered (tl: g)

## nativefps2  (2026-11-22T18:47:22)
- [PASS] kernel-boot - XNU boots and returns cleanly
- [PASS] iphone-x-surface - iPhone X 1125x2436 render -> 1080x2340 panel
- [PASS] ca-vbl - CoreAnimation takes display vsync (ca_vbl=1)
- [PASS] springboard - SpringBoard launched
- [PASS] gestures - automated gestures ran
- [PASS] frames - window server frames rendered
