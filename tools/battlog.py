#!/usr/bin/env python3
"""battlog.py RUN.log: what the MT6358 driver said on lancelot, next to what Android said before the run.

    Android, before the run (adb, /sys/class/power_supply/{battery,bms}):
      capacity 100%, voltage 4393 mV, current -8.6 mA, temp 28.0 C,
      CHARGE_COUNTER 5020000 uAh, CHARGE_FULL(_DESIGN) 5020000 uAh, RESISTANCE 140 mOhm

What to look for in a first run:
  - BATADC in mV against Android's 4393: the ADC's channel and scale;
  - the charge counter against 5020000 uAh: the uAh a count, and so CAR_COUNT_PAH
    (the x6826b's 0.107425 uAh a count would give 46.7 million counts at 100%);
  - RTC_AL_MTH: 0xe4xx when Android said 100;
  - CHR_CON0 bit 5 with the cable in (0x62 on the x6826b);
  - whether low and high of the four reads differ (if they never do, the "ready" bit is not
    being cleared and every read is the previous conversion).
"""
import re
import sys

ANDROID = dict(mv=4393, uah=5020000, ma=-8.6, temp=28.0)

def lines(path):
    seen, out = set(), []
    with open(path, 'rb') as f:
        for raw in f:
            text = raw.decode('utf-8', 'replace')
            at = text.find('mtkbatt:')
            if at < 0:
                continue
            line = text[at:].strip()
            if line in seen:
                continue
            seen.add(line)
            out.append(line)
    return out

def main():
    said = lines(sys.argv[1])
    if not said:
        sys.exit('battlog.py: no "mtkbatt:" lines: was mtkbatt=2 among the boot-args?')
    print('%d rows from the driver\n' % len(said))
    run = re.compile(r'mtkbatt: (?P<pct>\d+)\.(?P<h>\d+)% \((?P<how>[^)]*)\), BATADC (?P<counts>\d+) counts '
                     r'\((?P<low>\d+)\.\.(?P<high>\d+) of (?P<good>\d+)\) = (?P<mv>\d+) mV, charger (?P<chr>in|out) '
                     r'\(CHR_CON0 (?P<reg>0x[0-9a-f]+)\), status (?P<status>\d+)')
    tail = re.compile(r'gauge (?P<g>-?\d+) counts = (?P<ma>-?\d+) mA, charge counter (?P<car>0x[0-9a-f]+)')
    rows = []
    for line in said:
        m = run.search(line)
        if not m:
            print('  %s' % line)
            continue
        row = m.groupdict()
        t = tail.search(line)
        if t:
            row.update(t.groupdict())
        rows.append(row)
    if not rows:
        sys.exit('\nnone of them is a reading: only the reason above')
    print('\n  %4s %7s %-16s %6s %10s %6s %4s %7s' % ('#', 'percent', 'by', 'mV', 'charge', 'mA', 'chr', 'CHR'))
    for i, r in enumerate(rows):
        car = int(r['car'], 16) if 'car' in r else None
        print('  %4d %3s.%02d%% %-16s %6s %10s %6s %4s %7s'
              % (i, r['pct'], int(r['h']), r['how'], r['mv'],
                 '%d' % car if car is not None else '-', r.get('ma', '-'), r['chr'], r['reg']))
    first = rows[0]
    print('\nagainst Android, before the run (voltage %d mV, charge %d uAh = %d%%):'
          % (ANDROID['mv'], ANDROID['uah'], 100))
    mv = int(first['mv'])
    print('  BATADC          %d mV, Android %d mV: %+d mV' % (mv, ANDROID['mv'], mv - ANDROID['mv']))
    print('  charges a look  %s counts (%s..%s of %s)'
          % (first['counts'], first['low'], first['high'], first['good']))
    if first['low'] == first['high']:
        print('                  (all four the same: the "ready" bit is likely not cleared between requests)')
    if 'car' in first:
        car = int(first['car'], 16)
        if car:
            print('  charge counter  %#x = %d counts; Android says %d uAh at that point'
                  % (car, car, ANDROID['uah']))
            print('                  -> %.6f uAh a count (%.1f pAh), where the x6826b used 0.107425 (107425 pAh)'
                  % (ANDROID['uah'] / car, 1e6 * ANDROID['uah'] / car))
        print('  current         %s mA, Android %s mA' % (first.get('ma'), ANDROID['ma']))
    chr_in = first['chr'] == 'in'
    print('  charger         %s (CHR_CON0 %s); Android said USB online at 5043 mV'
          % (first['chr'], first['reg']))

if __name__ == '__main__':
    main()