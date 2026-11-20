#!/usr/bin/env python3
"""slowcalls.py RUN.log [N]: the calls a run with glslow=US says took long, by name.

Prints, per name of a call, how often it was slow and how much time it took together, then the last
few of the worst, so a frame's own time (frames.py's "draw") can be put on one function.
"""
import collections
import sys

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    shown = int(sys.argv[2]) if len(sys.argv) > 2 else 10
    count, total, worst, seen = collections.Counter(), collections.Counter(), [], set()
    for raw in open(path, 'rb'):
        text = raw.decode('utf-8', 'replace')
        at = text.find('tl: slow ')
        if at < 0:
            continue
        w = text[at:].split()
        try:
            name, us = w[2], int(w[3])
        except (IndexError, ValueError):
            continue
        key = (name, us, w[5] if len(w) > 5 else '')
        if key in seen:
            continue
        seen.add(key)
        count[name] += 1
        total[name] += us
        worst.append((us, name, ' '.join(w[3:])))
    if not count:
        sys.exit('no "tl: slow" lines: run without glslow=US?')
    print(f'{sum(count.values())} slow calls, {sum(total.values()) / 1e6:.2f} s of them, by name:')
    for name, n in count.most_common():
        print(f'  {name:22} {n:5} calls  {total[name] / 1000:9.1f} ms  mean {total[name] / n / 1000:7.2f} ms')
    worst.sort(reverse=True)
    print(f'\nthe {shown} worst:')
    for us, name, rest in worst[:shown]:
        print(f'  {us / 1000:8.2f} ms  {name}  {rest}')

if __name__ == '__main__':
    main()