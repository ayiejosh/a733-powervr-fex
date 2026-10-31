#!/usr/bin/env python3
"""replay.py CAP [--mem-from CAP2 [--mem-range LO:HI ...]] [--ctx-from CAP2] [--cmd-from CAP2]
Re-issue a pvrcap capture on the powervr render node and report whether each submit's signal syncobjs signal.
--mem-from: before each geometry submit, use CAP2's memory dump instead (only inside --mem-range VA ranges
            if given; outside them CAP's own dump is used). VAs must match between the two captures.
--ctx-from: create render contexts with CAP2's static context state bytes.
--cmd-from: submit CAP2's geometry cmd stream bytes instead of CAP's."""
import ctypes, fcntl, mmap, os, struct, sys, time, glob

R_BO, R_MAP, R_UNMAP, R_VMCTX, R_CTX, R_FL, R_HWRT, R_SUBMIT, R_MEM, R_CLOSE, R_SYNCOBJ, R_DESTROY, R_SUBMIT_RET = range(1, 14)
GEOM = 0
CPU = 1 << 2
PMFW = 1 << 1


def IOC(d, nr, size):
    return (d << 30) | (size << 16) | (ord('d') << 8) | nr


def IOWR(nr, size): return IOC(3, nr, size)
def IOW(nr, size): return IOC(1, nr, size)


P = 0x40
CREATE_BO, MMAP_OFF, CREATE_VM, DESTROY_VM = IOWR(P + 1, 24), IOWR(P + 2, 16), IOWR(P + 3, 8), IOW(P + 4, 8)
VM_MAP, VM_UNMAP, CREATE_CTX, DESTROY_CTX = IOW(P + 5, 40), IOW(P + 6, 24), IOWR(P + 7, 40), IOW(P + 8, 8)
CREATE_FL, DESTROY_FL, CREATE_HWRT, DESTROY_HWRT, SUBMIT = IOWR(P + 9, 32), IOW(P + 10, 8), IOWR(P + 11, 136), IOW(P + 12, 8), IOW(P + 13, 16)
GEM_CLOSE, SYNC_CREATE, SYNC_WAIT = IOW(0x09, 8), IOWR(0xBF, 8), IOWR(0xC3, 32)
DESTROY_BY_NR = {4: (DESTROY_VM, 'vm'), 8: (DESTROY_CTX, 'ctx'), 10: (DESTROY_FL, 'fl'), 12: (DESTROY_HWRT, 'hwrt')}


def load(path):
    recs, b, o = [], open(path, 'rb').read(), 0
    while o + 8 <= len(b):
        t, n = struct.unpack_from('<II', b, o)
        recs.append((t, b[o + 8:o + 8 + n]))
        o += 8 + n
    return recs


def submits(recs):
    """Group records into submits: [(mem_dumps, [(jobbytes, cmd, syncops)])]."""
    out, mem, i = [], [], 0
    while i < len(recs):
        t, d = recs[i]
        if t == R_MEM:
            mem.append((struct.unpack_from('<QQ', d)[0], d[16:]))
        elif t == R_SUBMIT and len(d) == 8:
            cnt, _ = struct.unpack('<II', d)
            jobs = []
            for k in range(cnt):
                jd = recs[i + 1 + 2 * k][1]
                sd = recs[i + 2 + 2 * k][1]
                jobs.append((jd[:48], jd[48:], sd[16:]))
            i += 2 * cnt
            out.append((mem, jobs))
            mem = []
        i += 1
    return out


def is_geom(jobs):
    return any(struct.unpack_from('<I', j)[0] == GEOM for j, c, s in jobs)


def graft_source(path):
    """Live mappings (addr, size, off, bo_size, flags) and memory at CAP2's first geometry submit."""
    bos, live, mem = {}, [], {}
    for t, d in load(path):
        if t == R_BO:
            size, orig, flags, h = struct.unpack('<QQQQ', d); bos[h] = (size, flags)
        elif t == R_MAP:
            vm, fl, addr, h, _, off, size = struct.unpack('<IIQIIQQ', d)
            live = [m for m in live if not (m[0] < addr + size and addr < m[0] + m[1])]
            live.append((addr, size, off) + bos[h])
        elif t == R_UNMAP:
            addr = struct.unpack_from('<Q', d, 8)[0]
            live = [m for m in live if m[0] != addr]
        elif t == R_MEM:
            mem[(struct.unpack_from('<Q', d)[0], len(d) - 16)] = d[16:]
        elif t == R_SUBMIT and len(d) == 8:
            if mem: return live, mem
    return live, mem


def render_fd():
    for r in sorted(glob.glob('/sys/class/drm/renderD*')):
        if os.path.basename(os.path.realpath(r + '/device/driver')) == 'powervr':
            return os.open('/dev/dri/' + os.path.basename(r), os.O_RDWR)
    sys.exit('no powervr render node (switch to the open stack first)')


def io(fd, req, buf):
    fcntl.ioctl(fd, req, buf, True)
    return buf


def main():
    a = sys.argv[1:]
    cap = load(a[0])
    opt, ranges, grafts, k = {}, [], [], 1
    while k < len(a):
        if a[k] == '--graft':
            lo, hi = a[k + 2].split(':'); grafts.append((a[k + 1], int(lo, 0), int(hi, 0))); k += 3; continue
        if a[k] == '--skip-overlap':
            opt[a[k]] = '1'; k += 1; continue
        if a[k] == '--mem-range':
            lo, hi = a[k + 1].split(':'); ranges.append((int(lo, 0), int(hi, 0)))
        else:
            opt[a[k]] = a[k + 1]
        k += 2
    other_geom_mem = None
    if '--mem-from' in opt:
        other_geom_mem = [m for m, js in submits(load(opt['--mem-from'])) if is_geom(js)]
    ctx_other = [d for t, d in load(opt['--ctx-from']) if t == R_CTX and struct.unpack_from('<I', d)[0] == 0] if '--ctx-from' in opt else None
    cmd_other = None
    if '--cmd-from' in opt:
        cmd_other = [c for m, js in submits(load(opt['--cmd-from'])) for (j, c, s) in js if struct.unpack_from('<I', j)[0] == GEOM]

    fd = render_fd()

    def do_grafts(glist):
        vmh = list(H['vm'].values())[-1]
        for path, lo, hi in glist:
            live, gmem = graft_source(path)
            for addr, size, off, bsize, bflags in live:
                if not (lo <= addr < hi): continue
                f = bflags | (0 if bflags & PMFW else CPU)
                rh = struct.unpack_from('<QI', io(fd, CREATE_BO, bytearray(struct.pack('<QIIQ', bsize, 0, 0, f))))[1]
                for m in maps:
                    if m[4] and m[0] < addr + size and addr < m[0] + m[1]: m[4] = False
                io(fd, VM_MAP, bytearray(struct.pack('<IIQIIQQ', vmh, 0, addr, rh, 0, off, size)))
                maps.append([addr, size, rh, off, True])
                data = gmem.get((addr, size))
                if data and not (f & PMFW):
                    mo = struct.unpack_from('<IIQ', io(fd, MMAP_OFF, bytearray(struct.pack('<IIQ', rh, 0, 0))))[2]
                    mm = mmap.mmap(fd, bsize, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=mo)
                    mm[off:off + len(data)] = data; mm.close()
                print('  graft 0x%x+0x%x (bo 0x%x flags 0x%x)%s' % (addr, size, bsize, bflags, '' if data else ' no data'))

    hwrt_src = None
    if '--hwrt-from' in opt:
        R2 = load(opt['--hwrt-from'])
        hwrt_src = {'fl': [d for t, d in R2 if t == R_FL], 'hwrt': [d for t, d in R2 if t == R_HWRT], 'done': False, 'flmap': {}}
        lo, hi = (int(x, 0) for x in opt.get('--hwrt-va', '0x8000001000:0x9000000000').split(':'))
        hwrt_src['graft'] = [(opt['--hwrt-from'], lo, hi)]
    H = {n: {} for n in ('bo', 'vm', 'ctx', 'fl', 'hwrt', 'sync')}
    size_of, maps, cpumap, keep = {}, [], {}, []
    nctx = ngeom = 0
    subs = iter(submits(cap))
    for t, d in cap:
        if t == R_BO:
            size, orig, flags, h = struct.unpack('<QQQQ', d)
            if h == 0xffffffff: continue
            f = flags | (0 if flags & PMFW else CPU)
            # bisection knobs: --clear-flags M clears M on non-PM BOs; --pm-flags-or M ORs M into PM_FW_PROTECT BOs;
            # --clear-va LO:HI limits --clear-flags to BOs first mapped inside that VA range (resolved at MAP time)
            if '--clear-flags' in opt and not (f & PMFW) and '--clear-va' not in opt:
                f &= ~int(opt['--clear-flags'], 0)
            if '--pm-flags-or' in opt and (f & PMFW):
                f |= int(opt['--pm-flags-or'], 0)
            b = io(fd, CREATE_BO, bytearray(struct.pack('<QIIQ', size, 0, 0, f)))
            H['bo'][h] = struct.unpack_from('<QI', b)[1]; size_of[H['bo'][h]] = (size, f)
        elif t == R_VMCTX:
            h = struct.unpack_from('<I', d)[0]
            H['vm'][h] = struct.unpack_from('<I', io(fd, CREATE_VM, bytearray(8)))[0]
        elif t == R_MAP:
            vm, fl, addr, h, _, off, size = struct.unpack('<IIQIIQQ', d)
            hit = [m for m in maps if m[4] and m[0] < addr + size and addr < m[0] + m[1]]
            if hit:
                print('  overlap: map va=0x%x size=0x%x over %s' % (addr, size, ', '.join('0x%x+0x%x' % (m[0], m[1]) for m in hit)))
                if '--skip-overlap' in opt:
                    continue
                for m in hit: m[4] = False  # drm_gpuvm replaces the old mapping
            io(fd, VM_MAP, bytearray(struct.pack('<IIQIIQQ', H['vm'][vm], fl, addr, H['bo'][h], 0, off, size)))
            maps.append([addr, size, H['bo'][h], off, True])
        elif t == R_UNMAP:
            vm, _, addr, size = struct.unpack('<IIQQ', d)
            io(fd, VM_UNMAP, bytearray(struct.pack('<IIQQ', H['vm'][vm], 0, addr, size)))
            for m in maps:
                if m[0] == addr: m[4] = False
        elif t == R_CTX:
            ty, fl, prio, h, _, slen, vm, cs = struct.unpack_from('<IIiIQIIQ', d)
            state = d[40:40 + slen]
            if ctx_other and ty == 0 and nctx < len(ctx_other):
                od = ctx_other[nctx]; state = od[40:40 + struct.unpack_from('<I', od, 24)[0]]
                cs = struct.unpack_from('<Q', od, 32)[0]
            if ty == 0: nctx += 1
            sb = ctypes.create_string_buffer(state, max(len(state), 1)); keep.append(sb)
            r = io(fd, CREATE_CTX, bytearray(struct.pack('<IIiIQIIQ', ty, fl, prio, 0, ctypes.addressof(sb) if slen else 0,
                                                         len(state), H['vm'][vm], cs)))
            H['ctx'][h] = struct.unpack_from('<I', r, 12)[0]
        elif t == R_FL:
            addr, ini, mx, gr, th, vm, h = struct.unpack('<QIIIIII', d)
            if hwrt_src:
                if not hwrt_src['done']:
                    do_grafts(hwrt_src['graft']); hwrt_src['done'] = True
                src = [x for x in hwrt_src['fl'] if struct.unpack_from('<I', x, 8)[0] == ini][0]
                addr, ini, mx, gr, th, _, h2 = struct.unpack('<QIIIIII', src)
                print('  FL from other capture: addr 0x%x init %d max %d' % (addr, ini, mx))
            r = io(fd, CREATE_FL, bytearray(struct.pack('<QIIIIII', addr, ini, mx, gr, th, H['vm'][vm], 0)))
            H['fl'][h] = struct.unpack_from('<I', r, 28)[0]
            if hwrt_src: hwrt_src['flmap'][h2] = H['fl'][h]
        elif t == R_HWRT:
            b = bytearray(d)
            f0, f1 = struct.unpack_from('<II', b, 80)
            if hwrt_src:
                b = bytearray(hwrt_src['hwrt'][0])
                g0, g1 = struct.unpack_from('<II', b, 80)
                struct.pack_into('<II', b, 80, hwrt_src['flmap'][g0], hwrt_src['flmap'][g1] if g1 else 0)
                print('  HWRT from other capture')
            else:
                struct.pack_into('<II', b, 80, H['fl'][f0], H['fl'][f1] if f1 else 0)
            h = struct.unpack_from('<I', b, 132)[0]
            struct.pack_into('<I', b, 132, 0)
            H['hwrt'][h] = struct.unpack_from('<I', io(fd, CREATE_HWRT, b), 132)[0]
        elif t == R_SYNCOBJ:
            h, fl = struct.unpack('<II', d)
            H['sync'][h] = struct.unpack_from('<I', io(fd, SYNC_CREATE, bytearray(struct.pack('<II', 0, fl))))[0]
        elif t == R_DESTROY:
            nr, h = struct.unpack('<II', d)
            req, kind = DESTROY_BY_NR.get(nr, (None, None))
            if req and h in H[kind]:
                io(fd, req, bytearray(struct.pack('<II', H[kind].pop(h), 0)))
        elif t == R_SUBMIT and len(d) == 8:
            mem, jobs = next(subs)
            geom = is_geom(jobs)
            if geom and mem:
                src = mem
                if other_geom_mem and ngeom < len(other_geom_mem):
                    alt = dict(other_geom_mem[ngeom])
                    src = [(ad, alt.get(ad, dt) if (not ranges or any(lo <= ad < hi for lo, hi in ranges)) else dt) for ad, dt in mem]
                for ad, data in src:
                    for m in reversed(maps):  # newest live mapping: VAs repeat across VM contexts
                        if m[4] and m[0] == ad and m[1] == len(data):
                            h = m[2]
                            if h not in cpumap:
                                off = struct.unpack_from('<IIQ', io(fd, MMAP_OFF, bytearray(struct.pack('<IIQ', h, 0, 0))))[2]
                                cpumap[h] = mmap.mmap(fd, size_of[h][0], mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=off)
                            n = min(len(data), len(cpumap[h]) - m[3])
                            if n != len(data):
                                print('  note: map @0x%x size 0x%x, bo room 0x%x' % (ad, len(data), n))
                            cpumap[h][m[3]:m[3] + n] = data[:n]
                            break
            if geom and grafts and ngeom == 0:
                do_grafts(grafts)
            if geom and '--poke' in opt and ngeom == 0:
                vmh = list(H['vm'].values())[-1]
                for spec in opt['--poke'].split(';'):
                    va, ws = spec.split('=')
                    va = int(va, 0)
                    rh = struct.unpack_from('<QI', io(fd, CREATE_BO, bytearray(struct.pack('<QIIQ', 0x1000, 0, 0, CPU))))[1]
                    for m in maps:
                        if m[4] and m[0] < va + 0x1000 and va < m[0] + m[1]: m[4] = False
                    io(fd, VM_MAP, bytearray(struct.pack('<IIQIIQQ', vmh, 0, va, rh, 0, 0, 0x1000)))
                    maps.append([va, 0x1000, rh, 0, True])
                    mo = struct.unpack_from('<IIQ', io(fd, MMAP_OFF, bytearray(struct.pack('<IIQ', rh, 0, 0))))[2]
                    mm = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=mo)
                    words = [int(w, 16) for w in ws.split(',')]
                    mm[0:4 * len(words)] = struct.pack('<%dI' % len(words), *words); mm.close()
                    print('  poke page 0x%x: %s' % (va, ws))
            arr, signals = bytearray(), []
            if False:
                for path, lo, hi in grafts:
                    live, gmem = graft_source(path)
                    for addr, size, off, bsize, bflags in live:
                        if not (lo <= addr < hi): continue
                        f = bflags | (0 if bflags & PMFW else CPU)
                        rh = struct.unpack_from('<QI', io(fd, CREATE_BO, bytearray(struct.pack('<QIIQ', bsize, 0, 0, f))))[1]
                        for m in maps:
                            if m[4] and m[0] < addr + size and addr < m[0] + m[1]: m[4] = False
                        io(fd, VM_MAP, bytearray(struct.pack('<IIQIIQQ', vmh, 0, addr, rh, 0, off, size)))
                        maps.append([addr, size, rh, off, True])
                        data = gmem.get((addr, size))
                        if data and not (f & PMFW):
                            mo = struct.unpack_from('<IIQ', io(fd, MMAP_OFF, bytearray(struct.pack('<IIQ', rh, 0, 0))))[2]
                            mm = mmap.mmap(fd, bsize, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=mo)
                            mm[off:off + len(data)] = data; mm.close()
                        print('  graft 0x%x+0x%x (bo 0x%x flags 0x%x)%s' % (addr, size, bsize, bflags, '' if data else ' no data'))
            arr, signals = bytearray(), []
            for j, c, s in jobs:
                ty, ch, fl, clen, cptr, sstride, scount, sarr, hset, hidx = struct.unpack('<IIIIQIIQII', j)
                if ty == GEOM and cmd_other and ngeom < len(cmd_other):
                    c = cmd_other[ngeom]
                if ty == GEOM and '--patch-cmd' in opt:
                    c = bytearray(c)
                    for kv in opt['--patch-cmd'].split(','):
                        wi, wv = kv.split('='); struct.pack_into('<I', c, 4 * int(wi), int(wv, 0))
                    c = bytes(c)
                cb = ctypes.create_string_buffer(c, len(c)); keep.append(cb)
                ops = bytearray(s)
                for q in range(scount):
                    sh, sf, sv = struct.unpack_from('<IIQ', ops, q * sstride)
                    if sh not in H['sync']:
                        H['sync'][sh] = struct.unpack_from('<I', io(fd, SYNC_CREATE, bytearray(8)))[0]
                    struct.pack_into('<I', ops, q * sstride, H['sync'][sh])
                    if sf & (1 << 31): signals.append(H['sync'][sh])
                ob = ctypes.create_string_buffer(bytes(ops), max(len(ops), 1)); keep.append(ob)
                arr += struct.pack('<IIIIQIIQII', ty, H['ctx'][ch], fl, len(c), ctypes.addressof(cb), sstride, scount,
                                   ctypes.addressof(ob) if scount else 0, H['hwrt'][hset] if hset in H['hwrt'] else hset, hidx)
            ab = ctypes.create_string_buffer(bytes(arr), len(arr)); keep.append(ab)
            try:
                io(fd, SUBMIT, bytearray(struct.pack('<IIQ', 48, len(jobs), ctypes.addressof(ab))))
                err = 0
            except OSError as e:
                err = e.errno
            res = []
            for sh in signals:
                hb = ctypes.c_uint32(sh)
                dl = time.clock_gettime_ns(time.CLOCK_MONOTONIC) + 2_000_000_000
                try:
                    io(fd, SYNC_WAIT, bytearray(struct.pack('<QqIIII', ctypes.addressof(hb), dl, 1, 0, 0, 0)))
                    res.append('signalled')
                except OSError:
                    res.append('TIMEOUT')
            types = '+'.join('GFCT'[struct.unpack_from('<I', j)[0]] for j, c, s in jobs)
            print('submit %-6s err=%d %s%s' % (types, err, ' '.join(res) or '-', ' [mem %d ranges]' % len(mem) if geom else ''), flush=True)
            if geom: ngeom += 1


main()
