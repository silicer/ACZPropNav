#!/usr/bin/env python3
"""Fast cross-reference scanner for the unpacked acz.exe (no Ghidra needed).

Finds, for one or more target RVAs:
  * direct call/jmp (E8/E9 rel32)
  * any RIP-relative disp32 operand that resolves to the target
    (covers lea / mov reg,[rip+x] / movss xmm,[rip+x] / call [rip+x] ...)
  * absolute qword pointers in non-code sections

Every hit is attributed to its containing function using .pdata.

Usage:
  xref.py 0x19F30 0x1B2C0
  xref.py --json cache.json 0x19F30
Options:
  --max N        max hits printed per target (default 40)
  --addrs        print hit addresses (RVA) with 16 bytes of context
"""
import json, os, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PE = os.path.join(ROOT, 'work', 'acz_copy.exe.unpacked.exe')
CACHE = os.path.join(ROOT, 'work', 'xref_cache.json')


def sections(buf):
    e = struct.unpack_from('<I', buf, 0x3C)[0]
    coff = e + 4
    nsec = struct.unpack_from('<H', buf, coff + 2)[0]
    optsize = struct.unpack_from('<H', buf, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from('<H', buf, opt)[0]
    ib = struct.unpack_from('<Q', buf, opt + 24)[0] if magic == 0x20b else struct.unpack_from('<I', buf, opt + 28)[0]
    sec = opt + optsize
    out = []
    for i in range(nsec):
        o = sec + i * 40
        name = buf[o:o + 8].rstrip(b'\0').decode('latin1')
        vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', buf, o + 8)
        chars = struct.unpack_from('<I', buf, o + 36)[0]
        out.append(dict(name=name, vsize=vsize, rva=vaddr, rawsize=rawsize,
                        raw=rawptr, chars=chars))
    return ib, out


def build_funcs(buf, secs):
    pd = next((s for s in secs if s['name'] == '.pdata'), None)
    funcs = []
    if pd:
        for i in range(0, pd['vsize'], 12):
            b, e, u = struct.unpack_from('<III', buf, pd['raw'] + i)
            if 0 < b < e < 0x10000000:
                funcs.append((b, e))
    funcs.sort()
    return funcs


def containing(funcs, rva):
    lo, hi = 0, len(funcs) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        b, e = funcs[mid]
        if rva < b:
            hi = mid - 1
        elif rva >= e:
            lo = mid + 1
        else:
            return b, e
    return None, None


def scan(pe_buf, secs, funcs, targets):
    text = next(s for s in secs if s['name'] == '.text')
    tb, trva, tsize = text['raw'], text['rva'], min(text['rawsize'], text['vsize'])
    data = [s for s in secs if s['name'] in ('.rdata', '.data', '.rodata')]
    res = {t: {'calls': [], 'rip': [], 'ptr': []} for t in targets}

    # 1. direct call/jmp: E8/E9 rel32 anywhere in .text
    for p in range(tb, tb + tsize - 5):
        b0 = pe_buf[p]
        if b0 == 0xE8 or b0 == 0xE9:
            rel = struct.unpack_from('<i', pe_buf, p + 1)[0]
            tgt = (p - tb) + trva + 5 + rel
            if tgt in res:
                res[tgt]['calls'].append(trva + (p - tb))

    # 2. rip-relative disp32: value at p = tgt - (spot_after + 4)
    for p in range(tb, tb + tsize - 4):
        disp = struct.unpack_from('<i', pe_buf, p)[0]
        cur = trva + (p - tb)
        tgt = cur + 4 + disp
        if tgt in res:
            res[tgt]['rip'].append(cur)

    # 3. absolute pointers
    for s in data:
        raw, rva, size = s['raw'], s['rva'], min(s['rawsize'], s['vsize'])
        for p in range(raw, raw + size - 8, 8):
            q = struct.unpack_from('<Q', pe_buf, p)[0]
            cand = q - 0x140000000
            if cand in res:
                res[cand]['ptr'].append(rva + (p - raw))
    return res


def main():
    args = sys.argv[1:]
    maxhits = 40
    if '--max' in args:
        i = args.index('--max')
        maxhits = int(args[i + 1])
        del args[i:i + 2]
    show_addrs = False
    if '--addrs' in args:
        show_addrs = True
        args.remove('--addrs')

    buf = open(PE, 'rb').read()
    ib, secs = sections(buf)
    funcs = build_funcs(buf, secs)
    targets = set(int(a, 16) for a in args)
    if not targets:
        print(__doc__)
        return

    cache = {}
    if os.path.exists(CACHE):
        try:
            cache = json.load(open(CACHE))
        except Exception:
            cache = {}
    # cache is keyed by target -> result dict
    missing = [t for t in targets if f'0x{t:X}' not in cache]
    if missing:
        res = scan(buf, secs, funcs, set(missing))
        for t, r in res.items():
            cache[f'0x{t:X}'] = r
        json.dump(cache, open(CACHE, 'w'))

    text = next(s for s in secs if s['name'] == '.text')
    for t in sorted(targets):
        r = cache[f'0x{t:X}']
        print(f'\n===== xrefs to 0x{t:X}  (function {containing(funcs, t)[0] and hex(containing(funcs, t)[0])}) ====')
        for kind, label in (('calls', 'direct call/jmp'), ('rip', 'RIP-relative operand'), ('ptr', 'absolute pointer')):
            hits = r.get(kind, [])
            print(f'  {label}: {len(hits)} hit(s)')
            for a in hits[:maxhits]:
                f = containing(funcs, a)
                fs = f'func 0x{f[0]:X}' if f[0] else '?'
                print(f'    0x{a:06X}  in {fs}')
                if show_addrs:
                    fo = None
                    for s in secs:
                        if s['rva'] <= a < s['rva'] + s['rawsize']:
                            fo = s['raw'] + (a - s['rva'])
                            break
                    if fo:
                        raw = buf[fo - 12:fo + 12]
                        print('        ' + ' '.join(f'{b:02X}' for b in raw))
            if len(hits) > maxhits:
                print(f'    ... {len(hits) - maxhits} more')


if __name__ == '__main__':
    main()
