#!/usr/bin/env python3
"""dupes.py FILE [PATTERN]: which lines of a log come twice in a row, and what the bytes look like.

A line written to the console by a program reaches the loader's log twice, once from kmoutput() and
once through console_write()'s patched uart; a kernel "tl:" line is written with loader_log_line()
and comes once. This counts both and prints the first pair in raw bytes.
"""
import collections
import sys


def main():
    path = sys.argv[1]
    data = open(path, 'rb').read()
    lines = data.split(b'\n')
    kinds = collections.Counter()
    doubled = collections.Counter()
    example = {}
    for i in range(len(lines) - 1):
        a = lines[i].strip(b'\r')
        b = lines[i + 1].strip(b'\r')
        if not a or a != b:
            continue
        at = a.find(b'tl: ')
        key = (a[at:at + 8] if at >= 0 else a[:24])
        doubled[key] += 1
        example.setdefault(key, (i, a))
    for key, n in doubled.most_common(20):
        i, text = example[key]
        print(f'{n:5} pairs  {key!r}')
        if key.startswith(b'tl: '):
            print(f'          e.g. line {i + 1}: {text[:100]!r}')
    at = data.find(b'tl: g')
    print('\nfirst "tl: g" at', at)
    if at >= 0:
        print(repr(data[at - 60:at + 160]))
    for word in (b'memory 2032 MB', b'[ ok ] sysctl: kernel identifies itself'):
        n = data.count(word)
        print(f'{word!r}: {n} times')


if __name__ == '__main__':
    main()