#!/usr/bin/env python3
"""jobsabs.py RUN.log FROM_S TO_S: the chains of a window with their absolute times.

Prints queue time, begin, end, and what changed since the line before, to see whether the jobs are
handed over in a burst and picked up later, or handed over one by one.
"""
import sys

def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    path, start_s, end_s = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
    start, end = int(start_s * 1e6), int(end_s * 1e6)
    rows, seen = [], set()
    for raw in open(path, 'rb'):
        text = raw.decode('utf-8', 'replace')
        at = text.find('tl: j ')
        if at < 0:
            continue
        w = text[at:].split()
        try:
            job = (int(w[2]), w[3], int(w[5]), int(w[7]), int(w[9]))
        except (IndexError, ValueError):
            continue
        if job in seen:
            continue
        seen.add(job)
        rows.append(job)
    rows = [r for r in rows if start <= r[3] <= end]
    rows.sort()
    previous = None
    for seq, kind, queued, began, ended in rows:
        print(f'{seq:6} {kind} q {queued} b {began} e {ended}  '
              f'new queue line after {queued - previous if previous else 0:7} us  '
              f'queue {began - queued:7} us  work {ended - began:6} us')
        previous = queued

if __name__ == '__main__':
    main()