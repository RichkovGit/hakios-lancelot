#!/usr/bin/env python3
"""Write an APFS manifest of modes for a tree built on a filesystem without permission bits.

    apfs-modes.py DIR > MANIFEST
    python3 tools/apfs/mkapfs.py -o root.apfs redwood=DIR,manifest=MANIFEST

On WSL's /mnt/c (drvfs) every file comes out 0777, and an image built straight from such a tree
hands iOS launchd plists that are group/other-writable. launchd then refuses every one of them:

    Caller specified a plist with bad ownership/permissions ... error = 122

The tree's intended bits are not lost anywhere else (the iOS system image has ios165/system.manifest
with Apple's own modes); this writes the same kind of file for the port's own small image, taking
the mode from the content instead of the host: directories 0755, Mach-O/ELF files and #! scripts
0755, everything else 0644, all owned root:wheel (0:0). Symbolic links are left to mkapfs, which
takes them from the tree.

Deterministic: sorted walk, no timestamps, so the same tree gives the same manifest.
"""
import os
import stat
import sys

MACHO = (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca")


def mode_of(path, st):
    if stat.S_ISDIR(st.st_mode):
        return stat.S_IFDIR | 0o755
    if not stat.S_ISREG(st.st_mode):
        return None
    try:
        with open(path, "rb") as f:
            head = f.read(4)
    except OSError:
        head = b""
    if head[:4] == b"\x7fELF" or head in MACHO or head[:2] == b"#!":
        return stat.S_IFREG | 0o755
    return stat.S_IFREG | 0o644


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: apfs-modes.py DIR > MANIFEST")
    top = sys.argv[1]
    out = sys.stdout
    for root, dirs, files in os.walk(top):
        dirs.sort()
        for name in sorted(dirs) + sorted(files):
            path = os.path.join(root, name)
            st = os.lstat(path)
            mode = mode_of(path, st)
            if mode is None:
                continue
            rel = os.path.relpath(path, top).replace(os.sep, "/")
            out.write("%o\t0\t0\t%s\n" % (mode, rel))


if __name__ == "__main__":
    main()