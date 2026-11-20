#!/usr/bin/env python3
"""texcost.py RUN.log [MORE.log ...]: what the textures of a draw cost, in the driver's own parts.

Reads the lines a run with boot-arg glprof=1 glenv=PANPROF=1 writes (tools/mesa/panfrost-texprof.patch,
redwood_prof_say() in pan_cmdstream.c), one every 6000 draws:

  redwood draw: 6000 draws, 20 us a draw (prepare 2, vertex buffers 0, state 13: textures 7
                (room 0, views 0, buffers 6: add 1, access 5, 3 submits of 14 adds, grows 4,
                handle max 812), samplers 0, renderer state 1, uniforms 2, clean 0; varying 2,
                launch 0); 1320 batches submitted in them, 11 us a draw

"textures" is the whole of panfrost_emit_texture_descriptors, and its three parts: the room for the
descriptors, each view (panfrost_update_sampler_view and the descriptor), and each buffer of those
views put on the batch's list (panfrost_batch_read_rsrc and panfrost_batch_add_bo). "add" is the
first of those two, "access" the second (panfrost_batch_update_access, which submits another batch
when the texture's writer is still queued -- "submits" counts those). "adds" counts the calls to
panfrost_batch_add_bo_old, "grows" how often the batch's list of buffers was made longer and
"handle max" how long it got: the list is one byte a buffer handle (pan_job.c,
panfrost_batch_get_bo_access), so a handle in the thousands is a list in the thousands too.

Runs that predate the split say "buffers N" and no more; they are read as they are.
"""
import re
import sys

DRAW = re.compile(
    r'redwood draw: (\d+) draws, (\d+) us a draw \(prepare (\d+), vertex buffers (\d+), '
    r'state (\d+): textures (\d+) \(room (\d+), views (\d+), buffers (\d+)'
    r'(?:: add (\d+), access (\d+), (\d+) submits of (\d+) adds, grows (\d+), handle max (\d+))?\)')

if __name__ == '__main__':
    windows = []
    seen = set()
    for path in sys.argv[1:]:
        with open(path, 'rb') as f:
            for raw in f:
                text = raw.decode('utf-8', 'replace')
                at = text.find('redwood draw: ')
                if at < 0:
                    continue
                line = text[at:].strip()
                if line in seen:
                    continue
                seen.add(line)
                m = DRAW.search(line)
                if m is None:
                    continue
                g = m.groups()
                row = dict(draws=int(g[0]), us=int(g[1]), prepare=int(g[2]), vertex=int(g[3]),
                           state=int(g[4]), textures=int(g[5]), room=int(g[6]), views=int(g[7]),
                           buffers=int(g[8]))
                for key, at2 in (('add', 9), ('access', 10), ('submits', 11), ('adds', 12),
                                 ('grows', 13), ('handle_max', 14)):
                    row[key] = int(g[at2]) if g[at2] is not None else None
                windows.append(row)

    if not windows:
        print('texcost.py: no "redwood draw:" lines: a run with glprof=1 glenv=PANPROF=1?')
        raise SystemExit(1)

    split = windows[-1]['add'] is not None
    head = '  draws   us/draw  textures   room  views buffers'
    if split:
        head += '    add  access  submits  adds  grows handles'
    print(head)
    for w in windows:
        line = '%7d %8d %9d %6d %6d %7d' % (w['draws'], w['us'], w['textures'], w['room'],
                                            w['views'], w['buffers'])
        if split:
            line += ' %6s %7s %8d %5d %6s %7s' % (
                '-' if w['add'] is None else w['add'],
                '-' if w['access'] is None else w['access'],
                -1 if w['submits'] is None else w['submits'],
                -1 if w['adds'] is None else w['adds'],
                '-' if w['grows'] is None else w['grows'],
                '-' if w['handle_max'] is None else w['handle_max'],
            )
        print(line)

    print()
    us = sum(w['us'] for w in windows)
    tex = sum(w['textures'] for w in windows)
    n = len(windows)
    print('%d windows, %d us a draw in all, %d of them in the textures (%.0f%%)'
          % (n, us / n, tex / n, 100.0 * tex / max(us, 1)))
    if tex:
        print('textures: room %d, views %d, buffers %d us a draw (%.2f us a texture)'
              % (sum(w['room'] for w in windows) / n, sum(w['views'] for w in windows) / n,
                 sum(w['buffers'] for w in windows) / n,
                 sum(w['buffers'] for w in windows) / max(sum(w['textures'] for w in windows), 1)))
    if split:
        adds = sum(w['adds'] for w in windows)
        print('buffers split: add %d, access %d us a draw; %.2f us a call of add_bo (of %d), '
              '%.2f us a read_rsrc' % (sum(w['add'] for w in windows) / n,
                                       sum(w['access'] for w in windows) / n,
                                       sum(w['add'] for w in windows) / max(adds, 1),
                                       adds,
                                       sum(w['access'] for w in windows) /
                                       max(sum(w['textures'] for w in windows), 1)))
        subs = sum(w['submits'] for w in windows)
        print('%d submits from update_access in all of them, %.0f%% of the reads of a texture'
              % (subs, 100.0 * subs / max(sum(w['textures'] for w in windows), 1)))
        print('the list of buffers grew %d times, to %d handles'
              % (sum(w['grows'] for w in windows), max(w['handle_max'] for w in windows)))