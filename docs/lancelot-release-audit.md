# Redmi 9 (lancelot): publication audit

Read-only audit of the tracked tree (`git ls-files`, 1011 files) against the
release gates in [lancelot-release-checklist.md](lancelot-release-checklist.md).
The checkout is **shallow**, so this is a working-tree audit, not a full history
audit. Do not push this history: publish a fresh, cleaned export instead.

Nothing here is published automatically. The items below are blockers to resolve
before a public release; `tools/release-clean.sh` (dry-run by default) untracks
them locally while keeping the working files, so local builds still run.

## Confirmed clean

- No private keys, no device serials, no Wi-Fi passphrase in tracked files. `docs/handoff-gpt-bluetooth.md` and the local run scripts are
  excluded via `.git/info/exclude`; `logs/` is not tracked (0 files).
- `ios165/` tracks only our four scripts (`fetch.sh`, `fetch-from-ipsw.py`,
  `select-system.py`, `mkvarskel.py`) — no Apple rootfs is tracked.
- `tools/wpa-test` passphrases are random or the IEEE 802.11 worked examples.

## Blockers (tracked, must be removed or rights-verified before release)

1. **Vendor UEFI binaries — `uefi/files/Binaries/redwood/QcomPkg/` (203 files).**
   Qualcomm proprietary EFI drivers (ChargerExDxe, ClockDxe, DisplayDxe, …).
   Remove from the publication tree and history.
2. **Other prebuilt UEFI — `uefi/files/Binaries/redwood/{ArmPkg,EmbeddedPkg,
   FatPkg,MdeModulePkg,…}/*.efi` (100 `.efi` total).** EDK2-derived; prefer
   rebuilding from upstream source under its BSD licence, or verify provenance
   and licence of each binary before distributing it.
3. **`prebuilt/` (27 files).** `loader.efi`, `mach.development.bcm2837`
   (a prebuilt Apple XNU kernel, APSL), `uefi/SILICIUM_UEFI.fd`,
   `uefi/BootShim.bin`, `uefi/*.dtb`, `uefi/ramdisk`, and the
   `prebuilt/userland/*` binaries. Publish source only; drop these binaries or
   document licence/provenance for each intentionally distributed one.
4. **Firmware headers — `mtkloader/touchfw.h` and
   `xnu/files/bsd/dev/redwood_focaltech_fw.h`.** Vendor touch firmware embedded
   as C arrays. Remove. The generators `tools/mktouchfw.py` / `tools/mkftsfw.py`
   are our code and may stay; they read firmware the user supplies locally.
5. **Documentation images — 26 `docs/*.jpg|png`.** iOS screenshots and system
   wallpapers (Apple UI). Remove from the publication tree.
6. **Radio identifier — `userland/rc.jobs.bt1`** named the controller BD_ADDR;
   redacted (the address itself is not kept in the publication tree).

## README correction

Both root READMEs claimed "no iOS screenshot is checked in"; 26 are tracked.
The wording is corrected to distinguish this research checkout from the intended
publication tree, and to point at this audit. After the images are actually
removed, restore the simple claim.

## Procedure (no automatic publication)

1. Run `tools/release-clean.sh` (dry-run) and review the list; `--apply`
   untracks the blockers with `git rm --cached` (working files kept) and adds
   `.gitignore` entries. This stages deletions; it does not commit or push.
2. Rebuild from a clean checkout with locally supplied device files
   (`ios165/fetch.sh`, phone firmware) and run the on-device gates.
3. Publish a **fresh export** of the cleaned tree (new repository or orphan
   branch), not this shallow history. Include source and checksums of reviewed
   artifacts only; keep Apple/vendor binaries, firmware, screenshots, serials,
   radio identifiers, private notes and raw logs out of both tree and history.
4. Update both READMEs to the verified Redmi 9 feature set (experimental vs
   unsupported), and document the exact partition the installer writes.
