#!/usr/bin/env python3
"""
logmix.py RUN.log: what takes the room in the part of a run's log that was kept.

The loader keeps a small head and a large tail of XNU's log (mtkloader's save_log), so what matters is
how many bytes a second the lines of the tail take: how much of the run's end they let through, and
which of them are worth their room.
"""
import sys

def kind(line):
    text = line.decode('utf-8', 'replace')
    at = text.find('tl: ')
    if at >= 0:
        w = text[at:].split()
        if len(w) >= 2:
            return 'tl: ' + w[1]
    stripped = text.strip()
    if stripped.startswith('--- kernel message buffer'):
        return 'msgbuf marker'
    if stripped[:1].isdigit():
        return 'kernel-stamped (mlog)'
    for prefix, name in (('MiniIO', 'MiniIO'), ('redwood-gl', 'redwood-gl'), ('rwtouch', 'rwtouch'),
                         ('mali:', 'mali'), ('fts:', 'fts'), ('kept:', 'kept'), ('rwm', 'rwm')):
        if stripped.startswith(prefix):
            return name
    if stripped.startswith('['):
        return 'kernel-stamped'
    if not stripped:
        return 'blank'
    return 'other'

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for path in sys.argv[1:]:
        data = open(path, 'rb').read()
        cut = data.find(b'bytes of the XNU log left out here')
        head = data[:cut] if cut >= 0 else b''
        tail = data[cut:] if cut >= 0 else data
        msgbuf = tail.find(b'--- kernel message buffer ---')
        live = tail[:msgbuf] if msgbuf >= 0 else tail
        print(f'{path}: {len(data)} bytes; head {len(head)}, live tail {len(live)}, '
              f'msgbuf {len(tail) - len(live)}')
        for name, part in (('head', head), ('live tail', live)):
            kinds, total = {}, 0
            for line in part.split(b'\n'):
                k = kind(line)
                n = len(line) + 1
                kinds[k] = kinds.get(k, 0) + n
                total += n
            print(f'  --- {name}: {total} bytes')
            for k, v in sorted(kinds.items(), key=lambda x: -x[1])[:12]:
                print(f'    {v:8d} {100 * v / max(1, total):5.1f}%  {k}')

if __name__ == '__main__':
    main()