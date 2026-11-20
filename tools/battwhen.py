#!/usr/bin/env python3
"""when() RUN.log: the wall clock (from the nearest timestamped line before it) of each battery_test call."""
import re, sys

stamp = re.compile(r'^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)')
calls = []
now = None
with open(sys.argv[1], 'rb') as f:
    for raw in f:
        line = raw.decode('utf-8', 'replace')
        m = stamp.match(line)
        if m:
            now = m.group(1)
        if 'battery_test:' in line and 'the kernel says' in line:
            calls.append([now, line.strip()[:80]])
        elif calls and 'battery_test: powerd lists no' in line:
            calls[-1].append('EMPTY')
        elif calls and 'battery_test: power source: {' in line:
            calls[-1].append('listed')
        elif calls and 'battery_test: the power adapter:' in line:
            calls[-1].append('adapter:' + line.split('adapter:')[1].strip()[:30])
for c in calls:
    print(c[0], '|', c[-2:] if len(c) > 2 else '?')