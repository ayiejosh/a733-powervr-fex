#!/usr/bin/env python3
"""capinfo.py CAP: list BOs, mappings, contexts, free lists, HWRT args and submits of a pvrcap capture."""
import struct, sys

NAMES = {1: 'BO', 2: 'MAP', 3: 'UNMAP', 4: 'VMCTX', 5: 'CTX', 6: 'FL', 7: 'HWRT', 8: 'SUBMIT', 9: 'MEM', 10: 'CLOSE', 11: 'SYNCOBJ', 12: 'DESTROY', 13: 'RET'}
b = open(sys.argv[1], 'rb').read()
o, bos = 0, {}
while o + 8 <= len(b):
    t, n = struct.unpack_from('<II', b, o)
    d = b[o + 8:o + 8 + n]
    o += 8 + n
    if t == 1:
        s, orig, f, h = struct.unpack('<QQQQ', d)
        bos[h] = s
        print('BO h=%d size=0x%x flags=0x%x (orig 0x%x)' % (h, s, f, orig))
    elif t == 2:
        vm, fl, addr, h, _, off, size = struct.unpack('<IIQIIQQ', d)
        warn = '  <-- beyond BO' if off + size > bos.get(h, 0) else ''
        print('MAP va=0x%010x h=%d off=0x%x size=0x%x%s' % (addr, h, off, size, warn))
    elif t == 5:
        ty, fl, prio, h, _, slen, vm, cs = struct.unpack_from('<IIiIQIIQ', d)
        print('CTX type=%d flags=%d prio=%d h=%d callstack=0x%x state(%d)=%s' % (ty, fl, prio, h, cs, slen, d[40:40 + slen].hex()))
    elif t == 6:
        print('FL addr=0x%x init=%d max=%d grow=%d thr=%d vm=%d h=%d' % struct.unpack('<QIIIIII', d))
    elif t == 7:
        print('HWRT %s' % d.hex())
    elif t == 8 and len(d) == 8:
        expect = 'job'
        print('SUBMIT %d jobs' % struct.unpack('<II', d)[0])
    elif t == 8 and expect == 'job':
        expect = 'ops'
        ty, ch, fl, clen, cptr, ss, sc, sa, hs, hi = struct.unpack_from('<IIIIQIIQII', d)
        print('JOB type=%d ctx=%d flags=0x%x cmdlen=%d hwrt=%d/%d cmd=%s' % (ty, ch, fl, clen, hs, hi, d[48:].hex()))
    elif t == 8:
        expect = 'job'
        ss, sc, sa = struct.unpack_from('<IIQ', d)
        ops = [struct.unpack_from('<IIQ', d, 16 + q * ss) for q in range(sc)]
        print('  SYNCOPS %s' % ' '.join('h%d/f0x%x/v%d' % op for op in ops))
    elif t == 9:
        addr, size = struct.unpack_from('<QQ', d)
        print('MEM va=0x%010x size=0x%x' % (addr, size))
    elif t in (3, 4, 10, 11, 12, 13):
        print(NAMES[t], d.hex())
