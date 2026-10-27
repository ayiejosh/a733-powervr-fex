#!/usr/bin/env python3
"""cmp.py REF.bin GOT.bin [float]: compare per-invocation 16-dword records; floats compared with a small ulp tolerance."""
import struct, sys
a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read()
isf = len(sys.argv) > 3
n = len(a) // 4
ua = struct.unpack('<%dI' % n, a); ub = struct.unpack('<%dI' % n, b)
bad = []
for i in range(n):
    x, y = ua[i], ub[i]
    if x == y: continue
    if isf:
        fx, fy = struct.unpack('<f', struct.pack('<I', x))[0], struct.unpack('<f', struct.pack('<I', y))[0]
        if fx == fx and fy == fy and abs(fx - fy) <= 2e-6 * max(1.0, abs(fx)): continue
    bad.append(i)
print('%s: %d of %d dwords differ' % ('PASS' if not bad else 'FAIL', len(bad), n))
for i in bad[:6]:
    print('  invocation %d dword %d: ref 0x%08x got 0x%08x' % (i // 16, i % 16, ua[i], ub[i]))
sys.exit(1 if bad else 0)
