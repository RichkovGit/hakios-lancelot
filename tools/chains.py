#!/usr/bin/env python3
"""
chains.py RUN.log [FROM_S] [TO_S]: what the GPU's jobs did, chain by chain (boot-arg mali_trace=1).

A run with mali_trace=1 writes a line per job chain, "tl: j SEQ f|v q QUEUED b BEGAN e ENDED" in
microseconds of mach_absolute_time: when the window server handed it over, when the driver's worker
began on it, when it was done. For each chain:

  q->b  the worker was busy with the one before (or asleep, or the queue was full)
  b->e  the driver programming it, the GPU drawing it, and the wait noticing it is done

and per frame ("tl: g" began/ended) how many chains it took. Prints a histogram, then the worst
frames chain by chain.
"""
import sys


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    jobs, frames = [], []
    for raw in open(path, 'rb'):
        text = raw.decode('utf-8', 'replace').strip()
        at = text.find('tl: ')
        if at < 0:
            continue
        w = text[at:].split()
        try:
            if w[1] == 'j':
                jobs.append((int(w[2]), w[3] == 'f', int(w[5]), int(w[7]), int(w[9])))
            elif w[1] == 'g' and len(w) >= 8:
                frames.append((int(w[4]), int(w[6]), int(w[7])))
        except (IndexError, ValueError):
            pass
    jobs.sort(key=lambda j: j[0])
    frames.sort(key=lambda f: f[1])
    if not jobs:
        sys.exit('no "tl: j" lines: run without mali_trace=1?')
    print(f'{len(jobs)} chains, {len(frames)} frames; {jobs[0][2]}..{jobs[-1][4]} us of the run')
    queue, work, gaps = [], [], []
    previous_end = None
    for seq, fragment, queued, began, ended in jobs:
        queue.append(began - queued)
        work.append(ended - began)
        if previous_end is not None and began >= previous_end:
            gaps.append(began - previous_end)
        previous_end = max(previous_end or 0, ended)
    total = jobs[-1][4] - jobs[0][2]

    def say(name, values, unit=1000.0):
        if not values:
            return
        values = sorted(values)
        n = len(values)
        print(f'{name:22} n={n:5} mean {sum(values) / n / unit:7.3f} ms  '
              f'median {values[n // 2] / unit:7.3f}  p90 {values[int(n * 0.9)] / unit:7.3f}  '
              f'max {values[-1] / unit:7.3f}')

    busy = sum(work)
    print(f'wall {total / 1e6:.1f} s, in the driver+GPU {100 * busy / total:.0f}% of it, '
          f'fragment {sum(1 for j in jobs if j[1])}')
    say('queue -> begin', queue)
    say('begin -> ended', work)
    say('between chains', gaps)
    # chains per frame, and the frames with the most
    per = []
    for began, ended, draws in frames:
        inside = [j for j in jobs if began <= j[3] < ended]
        if inside:
            per.append((len(inside), sum(j[4] - j[3] for j in inside), began, ended, draws))
    per.sort()
    say('chains a frame', [p[0] for p in per], 1.0)
    say('GPU us a frame', [p[1] for p in per])
    print('\nthe frames with the most chains:')
    for chains, gpu, began, ended, draws in per[-6:]:
        print(f'  began {began} ended {ended}: {chains} chains, {gpu / 1000:.1f} ms of them, {draws} draws, '
              f'{100 * gpu / max(1, ended - began):.0f}% of the frame')
    print('\nthe last chains:')
    for seq, fragment, queued, began, ended in jobs[-12:]:
        print(f'  {seq} {"f" if fragment else "v"} queue {queued} begin {began} end {ended}: '
              f'waited {began - queued} us, took {ended - began} us')


if __name__ == '__main__':
    main()