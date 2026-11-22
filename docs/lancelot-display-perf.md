# Redmi 9 (lancelot): display performance, touch responsiveness, native resolution

From a read-only research subagent (touch-display-research) over source + ~40 run logs,
plus a verified hardware run (autotest `perf`, 20261122-030320).

## Touch is NOT the bottleneck
- The FT8719P reports every 8 ms (125 Hz); `poll_us=4000` catches each report. Measured
  HID dispatch 124.8 Hz (`touch_lat=2`, lancelot-glfps17.log): kernel post gap median
  8002 us, userland read-after-post 44 us, dispatch cost 219 us. ~2 touch events per
  display frame. No 30 fps quantization at the HID layer.
- The kernel touch path (redwood_fts.c report(), redwood_input.c queue, inject.c
  input_thread/send_touch) and the HID client type are correct. Do NOT change them.

## The ~30 fps is the display frame budget on underclocked silicon
- gestures 27-60 fps (tools/frames.py, lancelot-glfps39.log); slow frames dominated by
  "idle before" 19-34 ms (window server missing CoreAnimation ticks) + "draw" 2-19 ms.
  fbscale=2 draws 2.25x more pixels than fbscale=1, so draw often exceeds 16.7 ms ->
  every-other-tick -> ~30 fps.
- CPU/GPU ran at ~60% of rated clocks in EVERY recorded run (core ~1176 MHz, Mali
  syspll_d3 364 MHz). The kernel already implements raising them (redwood_mtkkeys.c):
  `mtk_cpufreq=2` -> A75 2000 MHz, `=3` -> also A55 1700 MHz; `mali_mhz=N` -> GPUPLL up
  to 950 MHz, with VSRAM-before-Vproc-before-PLL sequencing. Gated behind `buttons=2`.

## VERIFIED: raised clocks (autotest perf 5/5)
Boot-args `buttons=2 mtk_cpufreq=2 mali_mhz=800` (no code change):
- `cpufreq: A75s now at 1999 MHz` (Vproc step 96, VSRAM step 99).
- `gpufreq: the GPU at 800 MHz` (VGPU/VSRAM_GPU raised).
- Mali up step 6/6, SpringBoard launched, clean return to Android, no panic over 90 s.
This is the single highest-impact lever for touch responsiveness and native render.
Thermal/stability over long sessions + the 60 fps feel still need interactive verification.

## Native 1080x2340 is a cheap memcpy, not 6 fps
- The "51 ms/frame = 6 fps" comment was the uncached write-combining panel mapping,
  ALREADY FIXED by `fb_cached=1` (default, remaps COPYBACK + cleans cache per frame).
- With `fbwidth=1080 fbheight=2340 fbscale=0`, display==panel -> job.unscaled=true ->
  per-row memcpy (no per-pixel scaler). Surface allocation verified (connsys73).
- Real cost: ~30 MB/frame memory traffic (panel memcpy + cache clean + surface bzero)
  + 2.53 MP render. Tight for 60 fps at 1175 MHz, plausible 30-60 fps with raised clocks.
- UIKit scale unreconciled: native 1080x2340 at scale 3 = 360x780 points != 375x812.
  `fbscale=3` (1125x2436, standard iPhone X) downscaled 0.96x is layout-safe but costs a
  bilinear downscale. Touch mapping (picture_x/y normalize 0-65535) is fbscale-independent
  and correct for native fbfit=0. Avoid fbfit=1/3 (letterbox).

## VERIFIED: native resolution (autotest native 4/4, 20261122-030840)
`fbscale=0 fbwidth=1080 fbheight=2340 fbfit=0 fb_cached=1` + raised clocks:
`MiniIOMobileFramebuffer: display 1080x2340 on a panel of 1080x2340 (4352 bytes per row)`,
Mali up step 6/6, SpringBoard launched, clean return, no panic over 100 s. Native render
+ stability verified objectively; UIKit layout (360x780 points) + sustained fps feel still
need interactive verification.

## VERIFIED: ~60 fps touch gestures with raised clocks (autotest fps, 20261122-175251)
rc.jobs.fps drives automated gestures (rwtouch swipes/taps/Control Center/Settings), touch_lat=1,
tools/frames.py measures frame pacing. With buttons=2 mtk_cpufreq=2 mali_mhz=800:
- swipe/tap gestures: **58.5-59.2 fps** (near 60), 0% over 33 ms on most.
- all gestures: 408 frame steps, 16% over 16.7 ms, 4% over 33 ms (baseline was ~30 fps).
- home/unlock button animation: 30 fps (heavier zoom); 0 GPU waits >= 1 ms.
The ~30 fps root cause (underclocked silicon) is fixed: gestures now near 60 fps. The kernel
touch path is 125 Hz with 0 bad reports. Remaining: the unlock zoom and a few idle-before
frames (window server waiting for a commit) still drop below 60; subjective feel pending user.

## Native resolution reconciled (autotest nativefps 6/6, 20261122-182824)
KEY (MiniIOMobileFramebuffer.cpp:126-128): iOS carries the iPhone X identity and only knows that
screen (1125x2436). ANY other reported size -> MobileGestalt "unknown screen class" -> the UI has
NO layout for it. So truly-native fbwidth=1080 fbheight=2340 (the `native` profile) allocates the
surface but breaks UIKit layout. The LAYOUT-SAFE native is to render the iPhone X 1125x2436 and
scale to the 1080x2340 panel (the panel always scans out at its native 1080x2340):
- `nativefps` (fbscale=3, no fbwidth override): "display 1125x2436 on a panel of 1080x2340",
  SpringBoard with the correct iPhone X layout, GPU up, stable. VERIFIED 6/6.
- Perf tradeoff (frames.py, raised clocks): full 1125x2436 render (2.53 MP) gestures 10-41 fps,
  50% frames >16.7 ms, dominated by "idle before" (window server waiting for a commit/GPU fence,
  draw itself <1 ms); fbscale=2 (750x1624, 1.22 MP) gives 58-59 fps. So fbscale=2 is the 60 fps
  sharp-enough compromise; full iPhone X native render is layout-safe + sharpest but needs the
  idle-before/commit bottleneck addressed for 60 fps. The native-resolution + stable-GPU goal is
  met at the render/layout level; 60 fps at full native render is the remaining optimization.
- ca_vbl=1 tested (nativefps2): CoreAnimation synced to display vsync cuts idle-before to 7-8 ms,
  but the full 1125x2436 draw is 14-15 ms + gpu 8-12 ms (~22-27 ms/frame) -> ~40 fps, draw-bound.
  So full iPhone X native render is ~40 fps even at raised clocks; fbscale=2 (750x1624, draw ~5 ms)
  is the 60 fps mode. 60 fps at full native render would need a faster draw (NEON/GPU offload).

## Recommended next (interactive)
1. Re-test native gated on SpringBoard (`waitproc SpringBoard` + settled frame before
   tapping): `fbscale=0 fbwidth=1080 fbheight=2340 fbfit=0 fb_filter=0 fb_threads=6
   fb_beat=0 buttons=2 mtk_cpufreq=2 mali_mhz=800`. Measure touch_lat=1 + tools/frames.py.
2. Verify 60 fps feel at fbscale=2 with raised clocks; if UIKit layout breaks at native,
   fall back to fbscale=3 (layout-safe) or keep fbscale=2 (sharpness/perf compromise).
3. NEON bilinear only if fb_filter=1 sharpness wanted at 2x; irrelevant for native memcpy.
