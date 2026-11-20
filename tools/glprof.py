#!/usr/bin/env python3
"""glprof.py RUN.log [MORE.log ...]: where the window server's drawing time went, by GL call.

Reads the lines a run with boot-arg glprof=1 writes (userland/src/eagl.c, prof_take()):

  redwood-gl: glprof at US us, frames A to B: N calls, T us in them (U a frame); top: NAME T us
              (U each, xN); ...
  redwood-gl: glprof parts: N frames; draw ours T us (U each, xN); draw begin ...;

one every 50 frames, then starts counting again, so each is a window of about a second. With the
touch samples (tl: p) each window is put in the gesture it fell in (as tools/frames.py segments
them), and the windows of a gesture are added up: how long its drawing took a frame, and which
calls that was. "U a frame" is what tools/frames.py calls "draw" -- the window server's own thread
from a frame's first draw to its flush -- so a call's share of it is its share of the wall time,
whether it was the processor working or the thread waiting.

The "parts" line of a window splits the two calls that cost most -- glDrawElements and
glBindTexture -- into ours and the library's, which the calls' own lines cannot.
"""
import collections
import re
import sys

PROF = re.compile(r'glprof at (\d+) us, frames (\d+) to (\d+): (\d+) calls, (\d+) us in them '
                  r'\((\d+) a frame\); top:(.*)')
ENTRY = re.compile(r' ([A-Za-z0-9_]+) (\d+) \((\d+) each, x(\d+)\);')
PARTS = re.compile(r'glprof parts: (\d+) frames;(.*)')
SEGMENT_US = 2500000


def lines_of(paths):
    seen, out = set(), []
    for path in paths:
        with open(path, 'rb') as f:
            for raw in f:
                text = raw.decode('utf-8', 'replace')
                at = text.find('redwood-gl: glprof at ')
                if at < 0:
                    at = text.find('redwood-gl: glprof parts: ')
                if at < 0:
                    continue
                line = text[at:].strip()
                if line not in seen:
                    seen.add(line)
                    out.append(line)
    return out


def parts_of(text):
    """' draw ours 12 (3 each, x80); bind mesa 7 (1 each, x9);' -> [('draw ours', 12, 80), ...]"""
    out = []
    for chunk in text.split(';'):
        tokens = chunk.split()
        if tokens[:1] == ['sums'] and len(tokens) >= 4:
            try:
                out.append(('sums', int(tokens[2]), int(tokens[1].lstrip('x').rstrip(','))))
            except ValueError:
                pass
            continue
        if len(tokens) < 5 or not tokens[-3].startswith('('):
            continue
        try:
            out.append((' '.join(tokens[:-4]), int(tokens[-4]), int(tokens[-1].lstrip('x').rstrip(')'))))
        except ValueError:
            continue
    return out


def part_text(name, us, count, frames):
    if name == 'sums':
        return 'sums %d KB a frame (x%d)' % (us // frames, count // frames)
    return '%s %d (%d each, x%d)' % (name, us // frames, us // max(1, count), count // frames)


def gestures_of(paths):
    gestures = []
    for path in paths:
        with open(path, 'rb') as f:
            for raw in f:
                text = raw.decode('utf-8', 'replace')
                at = text.find('tl: p ')
                if at < 0:
                    continue
                w = text[at:].split()
                try:
                    if len(w) >= 8 and int(w[2]) in (1, 4):
                        gestures.append((int(w[7]), int(w[2]), int(w[3]), int(w[4]), int(w[5])))
                except ValueError:
                    pass
    gestures.sort()
    return gestures


def gesture_name(gesture, previous):
    """the same words tools/frames.py uses for a gesture"""
    _, kind, button, x, y = gesture
    what = 'button %d' % button if kind == 4 else 'touch at %d%%,%d%%' % (x / 655.35, y / 655.35)
    return what if kind == 1 or previous is None else what + ' after ' + previous


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    windows = []
    for line in lines_of(sys.argv[1:]):
        m = PROF.search(line)
        if m:
            at, first, last, calls, total, each, top = m.groups()
            entries = [(name, int(us), int(one), int(count)) for name, us, one, count in ENTRY.findall(top)]
            windows.append(dict(at=int(at), first=int(first), last=int(last), calls=int(calls),
                                total=int(total), each=int(each), entries=entries, parts=[]))
            continue
        m = PARTS.search(line)
        if m and windows:
            windows[-1]['parts'] = parts_of(m.group(2))
    if not windows:
        sys.exit('no "glprof" lines: run without glprof=1?')
    windows.sort(key=lambda w: w['at'])

    print('glprof.py: %d windows of 50 frames, %d s of drawing' %
          (len(windows), (windows[-1]['at'] - windows[0]['at']) // 1000000))

    # each window in the gesture whose stretch it ended in
    gestures = gestures_of(sys.argv[1:])
    segments = []
    previous = None
    for i, gesture in enumerate(gestures):
        end = gestures[i + 1][0] if i + 1 < len(gestures) else gesture[0] + SEGMENT_US
        segments.append((gesture[0], end, gesture_name(gesture, previous), gesture))
        previous = gesture_name(gesture, None)
    for w in windows:
        name = '(no gesture)'
        for began, end, label, _ in segments:
            if began <= w['at'] < end:
                name = label
                break
        w['gesture'] = name

    by_gesture = collections.OrderedDict()
    for w in windows:
        acc = by_gesture.setdefault(w['gesture'], dict(frames=0, us=0, calls=0, names=collections.Counter(),
                                                       counts=collections.Counter(), parts=collections.Counter(),
                                                       part_calls=collections.Counter()))
        acc['frames'] += w['last'] - w['first']
        acc['us'] += w['total']
        acc['calls'] += w['calls']
        for n, us, _, count in w['entries']:
            acc['names'][n] += us
            acc['counts'][n] += count
        for n, us, count in w['parts']:
            acc['parts'][n] += us
            acc['part_calls'][n] += count
    for name, acc in by_gesture.items():
        frames = acc['frames'] or 1
        print(' %-34s %4d frames, %5d us a frame in %4d calls; top: %s' % (
            name, acc['frames'], acc['us'] // 1000 // frames, acc['calls'] // frames,
            ', '.join('%s %d (%d each)' % (n, us // frames, us // max(1, acc['counts'][n]))
                      for n, us in acc['names'].most_common(6))))
        if acc['parts']:
            print(' %-34s   parts: %s' % ('', ', '.join(
                part_text(n, us, acc['part_calls'][n], frames) for n, us in acc['parts'].most_common())))

    # what the whole run's drawing was made of, and how much of it per call
    total = sum(w['total'] for w in windows)
    frames = sum(w['last'] - w['first'] for w in windows) or 1
    names = collections.Counter()
    counts = collections.Counter()
    for w in windows:
        for n, us, _, count in w['entries']:
            names[n] += us
            counts[n] += count
    print(' all windows together: %d us a frame in %d calls a frame, of which the dearest in the '
          'windows:' % (total // 1000 // frames, sum(w['calls'] for w in windows) // frames))
    for n, us in names.most_common(10):
        print('   %-28s %6d us a frame (%d each, x%d a frame)' %
              (n, us // frames, us // max(1, counts[n]), counts[n] // frames))

    parts = collections.Counter()
    part_calls = collections.Counter()
    for w in windows:
        for n, us, count in w['parts']:
            parts[n] += us
            part_calls[n] += count
    if parts:
        print(' parts of those calls:')
        for n, us in parts.most_common():
            print('   %-28s %s' % (n, part_text(n, us, part_calls[n], frames)[len(n) + 1:]))

    # the windows that weighed most, in order: which screen costs what
    heavy = sorted(windows, key=lambda w: -w['each'])[:8]
    print(' the eight windows that took longest a frame:')
    for w in heavy:
        print('   %8.3f s, frames %d..%d: %d us a frame, %d calls; top: %s' % (
            w['at'] / 1000000.0, w['first'], w['last'], w['each'], w['calls'],
            ', '.join('%s %d' % (n, us // max(1, w['last'] - w['first'])) for n, us, _, _ in w['entries'][:6])))


if __name__ == '__main__':
    main()