#!/usr/bin/env python3
"""Host address -> function, from the linker map (build-farm/pod.map, else build/pod.map).

    py -3 tools/addr2name.py 610B607B 60326298 ...

A fault or watchdog report prints host addresses (`=== fault ... at 0x...`,
`thread N: eip=... host frames: ...`); this names the lifted function or the
host function each one is in.
"""
import bisect
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load():
    for d in ('build-farm', 'build'):
        p = os.path.join(ROOT, d, 'pod.map')
        if os.path.exists(p):
            syms = []
            for line in open(p, errors='replace'):
                m = re.match(r'\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s', line)
                if m:
                    syms.append((int(m.group(2), 16), m.group(1)))
            return sorted(syms)
    sys.exit('no pod.map: build first')


def main():
    syms = load()
    keys = [a for a, _ in syms]
    for x in sys.argv[1:]:
        a = int(x, 16)
        i = bisect.bisect_right(keys, a) - 1
        print('%08X  %s+0x%X' % (a, syms[i][1], a - syms[i][0]) if i >= 0 else '%08X  ?' % a)


if __name__ == '__main__':
    main()
