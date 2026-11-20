#!/usr/bin/env python3
"""pick.py FILE REGEX [LIMIT] [TAIL]: the lines of a log that match, in order.

The log mixes the loader's console, XNU's kprintf lines and the programs' own; a regex that names
one of them is easier to give here than to quote through the shell.
"""
import re
import sys

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    path = sys.argv[1]
    # several patterns, any of them: a shell's "|" does not survive the trip through PowerShell
    pattern = re.compile('|'.join(sys.argv[2:]))
    limit = 0
    tail = False
    hits = []
    for raw in open(path, 'rb'):
        line = raw.decode('utf-8', 'replace').rstrip('\r\n')
        if pattern.search(line):
            hits.append(line)
    if tail:
        hits = hits[-limit:] if limit else hits
    elif limit:
        hits = hits[:limit]
    for line in hits:
        print(line)
    print(f'-- {len(hits)} lines', file=sys.stderr)

if __name__ == '__main__':
    main()