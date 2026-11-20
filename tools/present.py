#!/usr/bin/env python3
"""present.py RUN.log: what a swap costs in the display's driver, split by where the time goes.

A run with touch_lat=1 writes "tl: s ID at US" when the swap is handed over and
"tl: v ID ready R shown S done D" when it is on the panel. Between ready and shown the driver waits
for the panel's beat (boot-arg fb_beat=1, show()); between shown and done it scales the frame's
surfaces onto the panel (drawLayer()); so the two are told apart here. Prints the medians and the
share of frames over a refresh's worth of time, for the whole run and per second of it.
"""
import sys

def stats(name, xs):
    if not xs:
        print(f'  {name:14} none')
        return
    xs = sorted(xs)
    n = len(xs)
    print(f'  {name:14} n={n:5} mean {sum(xs) / n / 1000:7.2f} ms  median {xs[n // 2] / 1000:7.2f}  '
          f'p90 {xs[n * 9 // 10] / 1000:7.2f}  max {xs[-1] / 1000:7.2f}')

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    handed, shown, seen = {}, [], set()
    for raw in open(path, 'rb'):
        text = raw.decode('utf-8', 'replace')
        at = text.find('tl: v ')
        if at >= 0:
            w = text[at:].split()
            try:
                row = (int(w[2]), int(w[4]), int(w[6]), int(w[8]))
            except (IndexError, ValueError):
                continue
            if row not in seen:
                seen.add(row)
                shown.append(row)
            continue
        at = text.find('tl: s ')
        if at >= 0:
            w = text[at:].split()
            try:
                handed[int(w[2])] = int(w[4])
            except (IndexError, ValueError):
                pass
    if not shown:
        sys.exit('no "tl: v" lines: run without touch_lat=1?')
    shown.sort()
    print(f'{len(shown)} frames on the panel, {len(handed)} swaps handed over')
    beat = [s - r for _, r, s, d in shown if s >= r]
    draw = [d - s for _, r, s, d in shown]
    hand = [d - handed[i] for i, r, s, d in shown if i in handed]
    stats('beat (r->s)', beat)
    stats('draw (s->d)', draw)
    stats('swap (s->d)', hand)
    stats('ready (s->r)', [r - handed[i] for i, r, s, d in shown if i in handed])
    # per second: how many frames, how much waiting on the beat, how much drawing
    print('\nper second: frames, beat median, draw median')
    by_second = {}
    for i, r, s, d in shown:
        by_second.setdefault(s // 1000000, []).append((s - r, d - s))
    for second in sorted(by_second):
        rows = by_second[second]
        b = sorted(x[0] for x in rows)
        w = sorted(x[1] for x in rows)
        print(f'  {second:6} s: {len(rows):3} frames, beat {b[len(b) // 2] / 1000:7.2f} ms, '
              f'draw {w[len(w) // 2] / 1000:7.2f} ms')

if __name__ == '__main__':
    main()