#!/usr/bin/env python3
"""
battwatch.py RUN.log: who saw the power source list, by process.

battery_test watch prints one line a second, "battery_test: watch N (process P): K sources, ...".
If one process always says 0 and another always says 1, the list is not coming and going: two
processes are asking, and libIOPS answers them differently.
"""
import re, sys

line = re.compile(r'battery_test: watch (\d+) \(process (\d+)\): (-?\d+) source')
seen = {}
for raw in open(sys.argv[1], 'rb'):
    m = line.search(raw.decode('utf-8', 'replace'))
    if not m:
        continue
    look, pid, count = int(m.group(1)), int(m.group(2)), int(m.group(3))
    seen.setdefault(pid, []).append((look, count))

for pid, looks in sorted(seen.items()):
    counts = [c for _, c in looks]
    first = looks[0][0]
    print(f'process {pid}: {len(looks)} looks, from {first}, counts {sorted(set(counts))}, '
          f'{counts.count(0)} empty, {len(counts) - counts.count(0)} with a source')
if len(seen) > 1:
    print(f'({len(seen)} processes were watching at the same time)')