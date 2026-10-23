#!/usr/bin/env python3
# usage: python3 -I summarize.py <label> [patterns...]   -> one line per pattern: rc, distinct layer message ids with counts, result line
import re, sys, os, collections
S = os.path.dirname(os.path.abspath(__file__))
label = sys.argv[1]
pats = sys.argv[2:] or "A1 A2 B1 B2 B3 B4 B5 C1 C2 C3 D1 D2 D3 E F".split()
pat_msg = re.compile(r'^([A-Za-z0-9_-]+)\((ERROR|WARN|WARNING|PERF|PERFORMANCE|INFO)')
for p in pats:
    f = os.path.join(S, 'logs', f'{label}-{p}.txt')
    if not os.path.exists(f):
        continue
    rc = open(f[:-4] + '.rc').read().strip()
    c = collections.Counter()
    res = ''
    for line in open(f, errors='replace'):
        m = pat_msg.match(line)
        if m:
            c[m.group(1) + ' ' + m.group(2)] += 1
        if 'pixels RIGHT' in line or 'pixels WRONG' in line or ' -> pixels ' in line:
            res = line.strip()[4:]
    print(f'{p:3} rc={rc:>3} msgs={sum(c.values()):3} distinct={len(c):2} | ' + '; '.join(f'{k} x{v}' for k, v in sorted(c.items())) + ' | ' + res[:110])
