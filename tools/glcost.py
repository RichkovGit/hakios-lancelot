#!/usr/bin/env python3
"""glcost.py RUN.log [MORE.log ...]: what a glDrawElements costs, split between Mesa and the driver.

Reads the lines a run with boot-arg glprof=1 glenv=PANPROF=1 writes (main/draw.c and
state_tracker/st_draw.c in tools/mesa/panfrost-texprof.patch; said by redwood_prof_say() in
pan_cmdstream.c), one every 6000 draws of the driver:

  redwood gl: 5850 draws, 31 us a draw in all: entry 2, prepare 7 (st_prepare_draw 5, index bounds
              1, other 1), state tracker 23 (of which this driver 21), unaccounted 0; the state
              tracker found state to update on 5850 of them

  entry        FLUSH_FOR_DRAW, _mesa_update_state and the validation, before the draw is made
  prepare      st_prepare_draw, validate_index_bounds, and the rest of the draw's setup
  state tracker  cso_draw_vbo; the driver's own share of it is the "redwood draw" line
  unaccounted  all - entry - prepare - state tracker (noise, mostly)

The driver's own line, "redwood draw", is read too, and its "us a draw" is what the driver spent, so
the state tracker's own share is state tracker - driver. tools/texcost.py reads that line in full.
"""
import re
import sys

LINE = re.compile(
    r'redwood gl: (\d+) draws, (\d+) us a draw in all: entry (\d+), prepare (\d+) '
    r'\(st_prepare_draw (\d+), index bounds (\d+), other (\d+)\), state tracker (\d+) '
    r'\(of which this driver (\d+)\), unaccounted (-?\d+); the state tracker found state to '
    r'update on (\d+) of them')
DRAW = re.compile(r'redwood draw: (\d+) draws, (\d+) us a draw')

if __name__ == '__main__':
    rows, seen = [], set()
    for path in sys.argv[1:]:
        with open(path, 'rb') as f:
            for raw in f:
                text = raw.decode('utf-8', 'replace')
                at = text.find('redwood gl: ')
                if at < 0:
                    continue
                line = text[at:].strip()
                if line in seen:
                    continue
                seen.add(line)
                m = LINE.search(line)
                if m is None:
                    continue
                g = [int(x) for x in m.groups()]
                rows.append(dict(draws=g[0], all=g[1], entry=g[2], prepare=g[3], st_prep=g[4],
                                 bounds=g[5], other=g[6], cso=g[7], driver=g[8], rest=g[9],
                                 dirty=g[10]))

    if not rows:
        print('glcost.py: no "redwood gl:" lines: a run with glprof=1 glenv=PANPROF=1?')
        raise SystemExit(1)

    print('  draws    all  entry prepare  of it: st_prepare  bounds  other   cso  driver  '
          'tracker  rest  state to update')
    for r in rows:
        print('%7d %6d %6d %7d %21d %7d %6d %5d %7d %8d %5d %d%%'
              % (r['draws'], r['all'], r['entry'], r['prepare'], r['st_prep'], r['bounds'],
                 r['other'], r['cso'], r['driver'], r['cso'] - r['driver'], r['rest'],
                 100 * r['dirty'] // max(r['draws'], 1)))

    n = len(rows)
    avg = lambda key: sum(r[key] for r in rows) / n
    print()
    print('%d windows, %d us a draw in all: entry %d, prepare %d (st_prepare_draw %d, index bounds '
          '%d, other %d), cso %d (driver %d, state tracker %d), rest %d'
          % (n, avg('all'), avg('entry'), avg('prepare'), avg('st_prep'), avg('bounds'),
             avg('other'), avg('cso'), avg('driver'), avg('cso') - avg('driver'), avg('rest')))
    print('the state tracker had state to update on %d%% of the draws'
          % (100 * sum(r['dirty'] for r in rows) // max(sum(r['draws'] for r in rows), 1)))