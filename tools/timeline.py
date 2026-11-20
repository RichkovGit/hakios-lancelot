#!/usr/bin/env python3
"""timeline.py RUN.log FROM_S TO_S [STEP_MS]: the GPU's chains and frames on one time line.

A run with mali_trace=1 writes "tl: j SEQ f|v q QUEUED b BEGAN e ENDED" and the display writes
"tl: g SEQ began BEGAN ended ENDED draws" (twice: the console log's two writers; the repeats are
dropped here). Prints, per chain in the window, how long it waited in the queue and how long it
took, and per STEP_MS how much of the time was queue, driver+GPU, and nothing.
"""
import sys

def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    path, start_s, end_s = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
    step = int(sys.argv[4]) if len(sys.argv) > 4 else 20
    start, end = int(start_s * 1e6), int(end_s * 1e6)
    jobs, frames, seen = [], [], set()
    for raw in open(path, 'rb'):
        text = raw.decode('utf-8', 'replace').strip()
        at = text.find('tl: ')
        if at < 0:
            continue
        w = text[at:].split()
        try:
            if w[1] == 'j':
                job = (int(w[2]), w[3] == 'f', int(w[5]), int(w[7]), int(w[9]))
                if job not in seen:
                    seen.add(job)
                    jobs.append(job)
            elif w[1] == 'g' and len(w) >= 8:
                frame = (int(w[4]), int(w[6]), int(w[7]))
                if frame not in seen:
                    seen.add(frame)
                    frames.append(frame)
        except (IndexError, ValueError):
            pass
    jobs = [j for j in jobs if start <= j[3] <= end]
    frames = [f for f in frames if start <= f[0] <= end]
    if not jobs:
        sys.exit(f'no chains in {start_s}..{end_s} s')
    print(f'{len(jobs)} chains and {len(frames)} frames in {start_s}..{end_s} s')
    print('  seq  kind   queue->begin   begin->end   draws' )
    for seq, fragment, queued, began, ended in jobs:
        marks = ''
        for fb, fe, draws in frames:
            if fb <= began < fe:
                marks = f'  (frame {fb}..{fe}, {draws} draws)'
                break
        print(f'{seq:6} {"frag" if fragment else "vert"}  {began - queued:9} us  '
              f'{ended - began:9} us{marks}')
    print(f'\nper {step} ms: queued, in the driver+GPU, idle')
    edges = list(range(start, end, step * 1000))
    for lo in edges:
        hi = lo + step * 1000
        inside = [j for j in jobs if lo <= j[3] < hi]
        if not inside:
            continue
        queued = sum(j[3] - j[2] for j in inside)
        work = sum(j[4] - j[3] for j in inside)
        busy = 0
        cursor = None
        for _, _, q, b, e in sorted(inside, key=lambda j: j[3]):
            busy += e - b
        print(f'  {lo / 1e6:9.3f} s: {len(inside):3} chains, queue {queued / 1000:8.1f} ms, '
              f'work {work / 1000:8.1f} ms, one after another {busy / 1000:8.1f} ms')

if __name__ == '__main__':
    main()