#!/usr/bin/env python3
"""passes.py RUN.log [MS]: the fragment jobs of a run with mali_trace=1 by how long they took.

The GPU's work is a few passes a frame; this says how many take long (the ones that set a frame's
time) and what share of the whole they are, so a change can be aimed at them.
"""
import collections
import sys

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    long_ms = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    jobs, seen = [], set()
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
        jobs.append(job)
    jobs.sort()
    if not jobs:
        sys.exit('no "tl: j" lines: run without mali_trace=1?')
    buckets = collections.Counter()
    work = collections.Counter()
    for _, kind, queued, began, ended in jobs:
        us = ended - began
        bucket = 0 if us < 100 else (1 if us < 500 else (2 if us < 2000 else (3 if us < 5000 else
                  (4 if us < 10000 else (5 if us < 20000 else 6)))))
        buckets[(kind, bucket)] += 1
        work[(kind, bucket)] += us
    names = ['< 0.1 ms', '0.1-0.5', '0.5-2', '2-5', '5-10', '10-20', '> 20 ms']
    for kind in ('f', 'v'):
        total = sum(work[(kind, b)] for b in range(7))
        if not total:
            continue
        print(f'{"fragment" if kind == "f" else "vertex"} jobs: {sum(buckets[(kind, b)] for b in range(7))}, '
              f'{total / 1e6:.2f} s of the GPU')
        for b in range(7):
            if buckets[(kind, b)]:
                print(f'  {names[b]:9} {buckets[(kind, b)]:5} jobs  {work[(kind, b)] / 1000:9.1f} ms  '
                      f'{100 * work[(kind, b)] / total:5.1f}% of the work')
    heavy = [j for j in jobs if j[4] - j[3] >= long_ms * 1000]
    print(f'\n{long_ms} ms or longer: {len(heavy)} jobs, '
          f'{sum(j[4] - j[3] for j in heavy) / 1e6:.2f} s of the {sum(j[4] - j[3] for j in jobs) / 1e6:.2f} s')
    span = (jobs[-1][4] - jobs[0][2]) / 1e6
    print(f'over {span:.1f} s: one every {span / max(1, len(heavy)):.2f} s, '
          f'{100 * sum(j[4] - j[3] for j in heavy) / (span * 1e6):.0f}% of wall time')

if __name__ == '__main__':
    main()