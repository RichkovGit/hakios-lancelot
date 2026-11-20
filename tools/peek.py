#!/usr/bin/env python3
"""peek.py FILE PATTERN [BEFORE] [AFTER]: the raw bytes around the first match."""
import sys

path, pattern = sys.argv[1], sys.argv[2].encode()
before = int(sys.argv[3]) if len(sys.argv) > 3 else 120
after = int(sys.argv[4]) if len(sys.argv) > 4 else 600
data = open(path, 'rb').read()
at = data.find(pattern)
print('match at', at, 'of', len(data))
if at >= 0:
    print(repr(data[max(0, at - before):at + after]))
else:
    for i in range(len(data)):
        if pattern.lower() in data[i:i + len(pattern)].lower():
            print('case-insensitive at', i)
            print(repr(data[max(0, i - before):i + after]))
            break