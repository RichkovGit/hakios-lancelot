# Lancelot early boot splash

The original HakiOS splash is an optional teal H monogram and a light block
wordmark on the loader's existing black background. The artwork is encoded as
small, hand-drawn cell maps in [splash.h](../mtkloader/splash.h); it uses no
external artwork, Apple assets, or firmware.

## Enable and disable

Set `-DHAKIOS_SPLASH=1` in the loader compiler options. [The existing build
script](../mtkloader/build.sh) accepts this option as an extra argument; use an
isolated copy if the loader build outputs are shared. Omit the option (the
default) or set `-DHAKIOS_SPLASH=0` to keep the previous behavior. There is
no runtime toggle or extra boot delay.

The splash is drawn just after the full-screen black clear on the XNU selection
path, before the loader's progress text. Logs remain enabled and may overwrite
the artwork. The Android selection, recovery and diagnostic boot paths do not draw
the splash. The existing black clear and cache flush before the jump to XNU are
unchanged; the artwork is **not** handed to XNU.

The renderer uses integer arithmetic and opaque `0xffRRGGBB` rectangle writes
through the existing framebuffer helper. It leaves LK framebuffer discovery,
pitch, panel geometry and layer setup unchanged. It runs with the existing
`-mgeneral-regs-only` and `-mstrict-align` options. On the 1080 × 2340
panel the artwork's centered layout is 350 × 240 pixels. On smaller surfaces the
artwork scales down to a whole-pixel grid, or is skipped if it cannot fit.

## Host check

From the HakiOS checkout root, use a native C compiler and write the test binary
to a temporary directory:

```sh
out=$(mktemp -d)
cc -std=c11 -O2 -Wall -Wextra -Werror tools/splash-test/test.c -o "$out/splash-test"
"$out/splash-test"
rm -- "$out/splash-test"
rmdir -- "$out"
```

The test uses the production framebuffer helper on guarded host memory. It
checks centering, colors, scaling, bounds and untouched row padding at the LK
pitch. It does not test physical scanout or boot a phone.
