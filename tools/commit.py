#!/usr/bin/env python3
"""commit.py RUN.log FROM_S TO_S: frames against the window server's own clock (tl: a).

inject_sb.c writes "tl: a MAIN_WOKE COMMIT_BEGAN NOW" for a frame: when the main thread woke for it,
when the commit to the window server began, and when it ended. With the display's "tl: v" (ready,
shown, done) and the window server's "tl: g" (began, ended), this shows, frame by frame, which part
of the gap is the main thread asleep (nothing asked it for a frame), which is its work itself, and
which is the window server's drawing.
"""
import sys

def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    path, start_s, end_s = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
    start, end = int(start_s * 1e6), int(end_s * 1e6)
    commits, frames, shows, seen = [], [], {}, set()
    for raw in open(path, 'rb'):
        text = raw.decode('utf-8', 'replace')
        at = text.find('tl: ')
        if at < 0:
            continue
        w = text[at:].split()
        try:
            if w[1] == 'a' and len(w) >= 5:
                commits.append((int(w[2]), int(w[3]), int(w[4])))
            elif w[1] == 'g' and len(w) >= 8:
                row = (int(w[4]), int(w[6]), int(w[7]))
                if row not in seen:
                    seen.add(row)
                    frames.append(row)
            elif w[1] == 'v' and len(w) >= 9:
                shows[int(w[2])] = (int(w[4]), int(w[6]), int(w[8]))
        except (IndexError, ValueError):
            pass
    commits.sort()
    frames.sort()
    commits = [c for c in commits if start <= c[1] <= end]
    frames = [f for f in frames if start <= f[0] <= end]
    if not commits:
        sys.exit('no "tl: a" lines: is the commit clock of inject_sb on?')
    print(f'{len(commits)} commits and {len(frames)} frames in {start_s}..{end_s} s')
    print('    woke -> commit began: the main thread\'s own work before handing the frame over')
    previous = None
    for woke, began, now in commits:
        gap = f'{(woke - previous) / 1000:7.1f} ms asleep' if previous else ' ' * 18
        print(f'  woke {woke}  main {((began - woke) / 1000):7.2f} ms  commit {((now - began) / 1000):7.2f} ms  '
              f'{gap}')
        previous = now

if __name__ == '__main__':
    main()