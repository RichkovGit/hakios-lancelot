#!/bin/bash
# Prepare the tracked tree for publication by UNTRACKING proprietary/private
# artifacts. Working files are kept (git rm --cached), so local builds still run.
# Dry-run by default; pass --apply to stage the deletions and extend .gitignore.
# This never commits and never pushes. Publish a fresh export afterwards.
set -e
cd "$(dirname "$0")/.."
APPLY=0
[ "$1" = "--apply" ] && APPLY=1

# Collect the tracked blockers.
mapfile -t FILES < <(
  {
    git ls-files 'uefi/files/Binaries/redwood/QcomPkg'
    git ls-files 'uefi/files/Binaries' | grep -E '\.efi$'
    git ls-files prebuilt | grep -Ev '\.py$'
    git ls-files mtkloader/touchfw.h xnu/files/bsd/dev/redwood_focaltech_fw.h
    git ls-files docs | grep -Ei '\.(jpg|jpeg|png)$'
  } | sort -u
)

echo "blockers tracked: ${#FILES[@]}"
printf '%s\n' "${FILES[@]}"
if [ "$APPLY" = 0 ]; then
  echo
  echo "dry-run: nothing changed. Re-run with --apply to untrack (files kept on disk)."
  exit 0
fi

printf '%s\n' "${FILES[@]}" | xargs -d '\n' git rm --cached --quiet --
{
  echo
  echo '# publication exclusions (added by tools/release-clean.sh)'
  echo '/uefi/files/Binaries/redwood/QcomPkg/'
  echo '/uefi/files/Binaries/**/*.efi'
  echo '/prebuilt/'
  echo '!/prebuilt/**/*.py'
  echo '/mtkloader/touchfw.h'
  echo '/xnu/files/bsd/dev/redwood_focaltech_fw.h'
  echo '/docs/*.jpg'
  echo '/docs/*.jpeg'
  echo '/docs/*.png'
} >> .gitignore
echo "untracked ${#FILES[@]} files; .gitignore extended. Review 'git status', then commit yourself."
