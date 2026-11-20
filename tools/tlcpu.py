#!/usr/bin/env python3
"""tlcpu.py RUN.log [MORE.log ...]: of a frame's time, how much the window server was working on it.

Reads the "tl: g" lines a run with boot-arg touch_lat=1 writes (userland/src/eagl.c, frame_end()):

  tl: g 2351 began 233268732 ended 233274459 70 cpu 412034567890

and compares, frame to frame, the wall time with the processor time the whole process had in it
(process_cpu_ns(): task_info TASK_BASIC_INFO_64, every thread of the window server -- the drawing
thread, CoreAnimation's, the main one -- and not the kernel's own copies to the panel, which are
threads of the driver).

A frame whose processor time is close to its wall time is one the process worked at all along; one
whose processor time is a small part of it waited -- for a commit, a fence, a clock -- and no amount
of drawing made it slow. "share" is that ratio: 100% is one processor busy the whole frame, 200% two.

Runs whose frames say no "cpu" (before 18 November) are read as they are, without the ratio.
"""
import sys

def lines_of(paths):
    seen, out = set(), []
    for path in paths:
        with open(path, 'rb') as f:
            for raw in f:
                text = raw.decode('utf-8', 'replace')
                at = text.find('tl: ')
                if at < 0:
                    continue
                line = text[at:].strip()
                if line in seen:
                    continue
                seen.add(line)
                out.append(line)
    return out

def main():
    frames = []
    for line in lines_of(sys.argv[1:]):
        w = line.split()
        try:
            if w[1] != 'g' or len(w) < 8:
                continue
            cpu = int(w[9]) if len(w) >= 10 and w[8] == 'cpu' else None
            frames.append(dict(id=int(w[2]), began=int(w[4]), ended=int(w[6]), draws=int(w[7]), cpu=cpu))
        except (ValueError, IndexError):
            pass
    if not frames:
        sys.exit('tlcpu.py: no "tl: g" lines: a run with touch_lat=1?')
    frames.sort(key=lambda f: f['ended'])
    steps = []
    for a, b in zip(frames, frames[1:]):
        wall = b['ended'] - a['ended']
        if wall <= 0 or wall >= 100000:              # a pause, or the clock went back
            continue
        share = None
        if a['cpu'] is not None and b['cpu'] is not None and b['cpu'] >= a['cpu']:
            share = 100.0 * (b['cpu'] - a['cpu']) / (wall * 1000.0)
        steps.append(dict(wall=wall, draws=b['draws'], share=share))
    if not steps:
        sys.exit('tlcpu.py: no two frames to compare')
    print('%d frames, %d steps between them' % (len(frames), len(steps)))
    known = [s for s in steps if s['share'] is not None]
    if not known:
        print('no processor time in the frames (a run before 18 November): wall time only')
    def mean(values):
        return sum(values) / len(values) if values else 0
    over = [s for s in steps if s['wall'] > 18000]
    under = [s for s in steps if s['wall'] <= 18000]
    print('mean %.1f ms a frame, %d draws; over 16.7 ms %d of %d (%.0f%%)'
          % (mean([s['wall'] for s in steps]) / 1000, mean([s['draws'] for s in steps]),
             len(over), len(steps), 100.0 * len(over) / len(steps)))
    if known:
        print('the processor was working on %.0f%% of a frame in all, %.0f%% of a slow one, %.0f%% of a quick one'
              % (mean([s['share'] for s in known]),
                 mean([s['share'] for s in over if s['share'] is not None]),
                 mean([s['share'] for s in under if s['share'] is not None])))
        bins = {}
        for s in known:
            k = int(s['share'] // 25) * 25
            bins.setdefault(k, 0)
            bins[k] += 1
        print('\nthe share of a frame the process worked in it:')
        for k in sorted(bins):
            print('  %4d-%4d%% %4d  %s' % (k, k + 25, bins[k], '#' * (bins[k] * 60 // len(known))))
        # what a slow frame's extra time is: waiting, or working
        slow = [s for s in over if s['share'] is not None]
        if slow:
            work = mean([s['wall'] * s['share'] / 100 for s in slow]) / 1000
            print('\na slow frame: %.1f ms of wall time, %.1f of them the processor working, %.1f waiting'
                  % (mean([s['wall'] for s in slow]) / 1000, work, mean([s['wall'] for s in slow]) / 1000 - work))

if __name__ == '__main__':
    main()