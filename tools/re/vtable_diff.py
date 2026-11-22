#!/usr/bin/env python3
"""RTTI inheritance + vtable diffing for the unpacked acz.exe.

Modes:
  bases  <class-name-substr>...        print base-class chains from RTTI CHD
  diff   [--max N] <class-substr>...   dump + side-by-side diff of vtables
  dump   [--max N] <class-substr>...   dump vtables

Usage:
  vtable_diff.py bases  WeaponExQaamCtrl WeaponExSaamCtrl
  vtable_diff.py diff   WeaponExQaamCtrl WeaponExSaamCtrl WeaponExXmaaCtrl
  vtable_diff.py dump   --max 30 WeaponExQaamCtrl
"""
import re, struct, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PE = os.path.join(ROOT, 'work', 'acz_copy.exe.unpacked.exe')
TSV = os.path.join(ROOT, 'work', 'acz_rtti.tsv')


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
        out.append(dict(name=name, vsize=vsize, rva=vaddr, rawsize=rawsize, raw=rawptr))
    return ib, out


class PE:
    def __init__(self, path=PE):
        self.buf = open(path, 'rb').read()
        self.ib, self.secs = sections(self.buf)
        self.text = next(s for s in self.secs if s['name'] == '.text')
        self.funcs = {}
        pd = next((s for s in self.secs if s['name'] == '.pdata'), None)
        if pd:
            for i in range(0, pd['vsize'], 12):
                b, e, u = struct.unpack_from('<III', self.buf, pd['raw'] + i)
                if 0 < b < e < 0x10000000:
                    self.funcs[b] = e - b

    def rva2fo(self, rva):
        for s in self.secs:
            if s['rva'] <= rva < s['rva'] + s['rawsize']:
                return s['raw'] + (rva - s['rva'])
        return None

    def u32(self, rva):
        fo = self.rva2fo(rva)
        return struct.unpack_from('<I', self.buf, fo)[0] if fo is not None else None

    def u64(self, rva):
        fo = self.rva2fo(rva)
        return struct.unpack_from('<Q', self.buf, fo)[0] if fo is not None else None

    def cstr(self, rva):
        fo = self.rva2fo(rva)
        if fo is None:
            return None
        e = self.buf.find(b'\0', fo)
        return self.buf[fo:e].decode('latin1')

    def is_text(self, rva):
        return self.text['rva'] <= rva < self.text['rva'] + self.text['vsize']

    def func_size(self, rva):
        return self.funcs.get(rva)


def load_rtti(tsv=TSV):
    rows = []
    for line in open(tsv, encoding='utf-8'):
        p = line.rstrip('\n').split('\t')
        if len(p) < 3 or p[0] == 'name':
            continue
        rows.append((p[0], int(p[1], 16), int(p[2], 16)))
    by_vt = {r[1]: r for r in rows}
    by_col = {r[2]: r for r in rows}
    return rows, by_vt, by_col


def demangle_min(name):
    """Very small MSVC mangling readability pass for class names only."""
    if name.startswith('.?AV'):
        return name[4:].rstrip('@@').replace('@', '::')
    if name.startswith('.?AU'):
        return name[4:].rstrip('@@').replace('@', '::')
    return name


def td_name(pe, td_rva):
    # TypeDescriptor: +0 pVFTable(8), +8 spare(8), +0x10 name
    return demangle_min(pe.cstr(td_rva + 0x10) or '')


def class_bases(pe, col_rva):
    """Return list of (name, pmd_disp) for the full hierarchy, self first."""
    sig, off, cdOff, pTD, pCHD, pSelf = struct.unpack_from('<IIIIII', pe.buf, pe.rva2fo(col_rva))
    if sig != 1:
        return None, f'bad COL signature {sig}'
    chd = pCHD
    csig, attrs, nbase, pBaseArr = struct.unpack_from('<IIII', pe.buf, pe.rva2fo(chd))
    if csig != 0:
        return None, f'bad CHD signature {csig}'
    out = []
    for i in range(min(nbase, 24)):
        btd = pe.u32(pBaseArr + i * 4)
        if not btd:
            break
        # RTTIBaseClassDescriptor: pTypeDescriptor(4) numContained(4) pmd(8) attrs(4) pCHD(4)
        fo = pe.rva2fo(btd)
        ptd2, ncont, mdisp, pdisp, vdisp, battrs, pchd2 = struct.unpack_from('<IIiiIII', pe.buf, fo)
        nm = td_name(pe, ptd2)
        out.append((nm, mdisp, pdisp, vdisp, battrs))
    return out, None


def vtable_of(pe, vt_rva, maxslot):
    """Return list of (slot, rva) until a non-.text pointer."""
    out = []
    fo = pe.rva2fo(vt_rva)
    if fo is None:
        return out
    for s in range(maxslot):
        q = struct.unpack_from('<Q', pe.buf, fo + s * 8)[0]
        rva = q - pe.ib
        if not pe.is_text(rva):
            break
        out.append((s, rva))
    return out


def lookup(rows, pat):
    rx = re.compile(pat, re.I)
    return [r for r in rows if rx.search(r[0])]


def cmd_bases(pe, rows, pats):
    _, by_vt, by_col = load_rtti()
    for pat in pats:
        hits = lookup(rows, pat)
        print(f'### pattern {pat!r}: {len(hits)} class(es)')
        for name, vt, col in hits:
            bases, err = class_bases(pe, col)
            print(f'  {demangle_min(name)}  vt=0x{vt:06X} col=0x{col:06X}')
            if err:
                print(f'    !! {err}')
                continue
            for i, (bn, mdisp, pdisp, vdisp, battrs) in enumerate(bases):
                # find base vtable: the class entry whose name matches bn
                bvt = None
                for rn, rv, rc in rows:
                    if rn == bn:
                        bvt = rv
                        break
                extra = ''
                if i == 0:
                    extra = '  <- self'
                elif bvt is not None:
                    extra = f'  (own vt=0x{bvt:06X})'
                print(f'    [{i}] {bn:<44s} mdisp={mdisp:6d} pdisp={pdisp:4d} vdisp={vdisp:4d} attr=0x{battrs:X}{extra}')
        print()


def cmd_dump(pe, rows, pats, maxslot):
    for pat in pats:
        for name, vt, col in lookup(rows, pat):
            print(f'== {demangle_min(name)}  vt=0x{vt:06X} col=0x{col:06X}')
            for slot, rva in vtable_of(pe, vt, maxslot):
                sz = pe.func_size(rva)
                print(f'   [{slot:2d}] 0x{rva:06X}  {"size=0x%X" % sz if sz else "not-func-start"}')
            print()


def cmd_diff(pe, rows, pats, maxslot):
    groups = []
    for pat in pats:
        hits = lookup(rows, pat)
        if not hits:
            print(f'!! no class matches {pat!r}')
            continue
        groups.append((pat, hits[0][0], vtable_of(pe, hits[0][1], maxslot)))

    width = max((max((len(demangle_min(n)) for _, n, _ in groups), default=10)), 12) + 2
    hdr = 'slot | ' + ' | '.join(f'{demangle_min(n)[:width]:<{width}}' for _, n, _ in groups) + ' | same?'
    print(hdr)
    print('-' * len(hdr))
    nmax = max(len(v) for _, _, v in groups)
    for slot in range(nmax):
        cells = []
        vals = []
        for _, _, v in groups:
            d = dict(v)
            r = d.get(slot)
            if r is None:
                cells.append('-' * width)
                vals.append(None)
            else:
                s = f'0x{r:06X}'
                sz = pe.func_size(r)
                if sz:
                    s += f'({sz})'
                cells.append(f'{s:<{width}}')
                vals.append(r)
        known = [x for x in vals if x is not None]
        if not known:
            continue
        same = 'SAME' if len(set(known)) == 1 and len(known) == len(vals) else 'diff'
        print(f'{slot:4d} | ' + ' | '.join(cells) + f' | {same}')


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    mode = sys.argv[1]
    args = sys.argv[2:]
    maxslot = 20
    if '--max' in args:
        i = args.index('--max')
        maxslot = int(args[i + 1])
        del args[i:i + 2]
    pe = PE()
    rows, _, _ = load_rtti()
    print(f'# {len(rows)} RTTI classes, image base 0x{pe.ib:X}, {len(pe.funcs)} pdata functions')
    if mode == 'bases':
        cmd_bases(pe, rows, args)
    elif mode == 'dump':
        cmd_dump(pe, rows, args, maxslot)
    elif mode == 'diff':
        cmd_diff(pe, rows, args, maxslot)
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
