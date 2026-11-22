#!/usr/bin/env python3
"""Resolve which vtable slots / classes own the callers of given anchor functions.

Proof chain used to find the *generic* (plain-MSL) weapon update:
  1. read all vftables from an unpacked image using the rtti_scan TSV
     -> map function_rva -> [(class, slot), ...]
  2. scan .text for E8 rel32 calls to anchor helper RVAs
  3. attribute each call site to its owning function via .pdata
  4. report callers that are themselves vtable slots (with class+slot) — those
     are the generic implementations of the family, and callers that are not.

Also prints the MSVC RTTI inheritance chain for a requested class:
  COL+0x10 -> ClassHierarchyDescriptor -> BaseClassArray -> BaseClassDescriptors
  -> TypeDescriptor names.

Usage:
  owners.py <unpacked-pe> <rtti.tsv> --anchors 0x90510,0x907E0 [--class WeaponExQaamCtrl] [--maxslot 16]
"""
import bisect, re, struct, sys

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
        name = buf[o:o+8].rstrip(b'\0').decode('latin1')
        vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', buf, o + 8)
        out.append(dict(name=name, vsize=vsize, rva=vaddr, rawsize=rawsize, raw=rawptr))
    return ib, out

class PE:
    def __init__(self, path):
        self.buf = open(path, 'rb').read()
        self.ib, self.secs = sections(self.buf)
        self.text = next(s for s in self.secs if s['name'] == '.text')
        pd = next(s for s in self.secs if s['name'] == '.pdata')
        self.funcs = []
        for i in range(0, min(pd['vsize'], pd['rawsize']) // 12 * 12, 12):
            b, e, u = struct.unpack_from('<III', self.buf, pd['raw'] + i)
            if 0 < b < e < 0x10000000:
                self.funcs.append((b, e))
        self.funcs.sort()
        self.begins = [f[0] for f in self.funcs]

    def fo(self, rva):
        for s in self.secs:
            if s['rva'] <= rva < s['rva'] + s['rawsize']:
                return s['raw'] + (rva - s['rva'])
        return None

    def owner(self, rva):
        i = bisect.bisect_right(self.begins, rva) - 1
        if i >= 0 and self.funcs[i][0] <= rva < self.funcs[i][1]:
            return self.funcs[i]
        return None

    def qword(self, rva):
        fo = self.fo(rva)
        return struct.unpack_from('<Q', self.buf, fo)[0] if fo is not None else None

    def dword(self, rva):
        fo = self.fo(rva)
        return struct.unpack_from('<I', self.buf, fo)[0] if fo is not None else None

    def cstr_at_rva(self, rva):
        fo = self.fo(rva)
        if fo is None:
            return None
        end = self.buf.find(b'\0', fo)
        return self.buf[fo:end].decode('latin1', 'replace')

def load_vtables(pe, tsv, maxslot, want_class=None):
    slotmap = {}     # func_rva -> [(class, slot)]
    vt_of = {}       # class -> vftable rva
    for line in open(tsv, encoding='utf-8'):
        p = line.rstrip('\n').split('\t')
        if len(p) < 3 or p[0] == 'name':
            continue
        name, vt = p[0], int(p[1], 16)
        vt_of[name] = vt
        if want_class and want_class.lower() not in name.lower():
            continue
        for slot in range(maxslot):
            q = pe.qword(vt + slot * 8)
            if not q or q < pe.ib:
                break
            frva = q - pe.ib
            if not (pe.text['rva'] <= frva < pe.text['rva'] + pe.text['vsize']):
                break
            slotmap.setdefault(frva, []).append((name, slot))
    return slotmap, vt_of

def callers_of(pe, targets):
    """E8 rel32 call sites in .text -> {target_rva: [call_site_rva]}"""
    t = pe.text
    d = pe.buf[t['raw']:t['raw'] + t['rawsize']]
    out = {x: [] for x in targets}
    for p in range(len(d) - 5):
        if d[p] != 0xE8:
            continue
        disp = struct.unpack_from('<i', d, p + 1)[0]
        site = t['rva'] + p
        tgt = site + 5 + disp
        if tgt in out:
            out[tgt].append(site)
    return out

def rtti_chain(pe, tsv, needle):
    """Print the base-class chain for the first class whose name matches."""
    td2name, name2td = {}, {}
    # type descriptors: name at td+0x10
    for s in pe.secs:
        d = pe.buf[s['raw']:s['raw'] + s['rawsize']]
        i = 0
        while True:
            j = d.find(b'.?A', i)
            if j < 0:
                break
            i = j + 1
            end = d.find(b'\0', j)
            if end < 0 or end - j > 300:
                continue
            nm = d[j:end]
            if not all(0x20 <= c < 0x7f for c in nm):
                continue
            sname = nm.decode('latin1')
            s_rva = s['rva'] + j
            td = s_rva - 0x10
            td2name[td] = sname
            name2td.setdefault(sname, td)
    # find COLs referencing matching TD
    cols = []
    for s in pe.secs:
        if s['name'] not in ('.data', '_RDATA', '.rdata'):
            continue
        for off in range(0, s['rawsize'] - 24, 4):
            sig = struct.unpack_from('<i', pe.buf, s['raw'] + off)[0]
            if sig != 1:
                continue
            td = struct.unpack_from('<i', pe.buf, s['raw'] + off + 0x0C)[0]
            nm = td2name.get(td)
            if nm and needle.lower() in nm.lower():
                cols.append((s['rva'] + off, nm, td))
    if not cols:
        print(f'  (no COL found matching {needle!r})')
        return
    for col, nm, td in cols:
        cd = pe.dword(col + 0x10)
        print(f'  class {nm}  COL=0x{col:X}  classdesc=0x{cd:X}')
        if not cd:
            continue
        num = pe.dword(cd + 0x08)
        arr = pe.dword(cd + 0x0C)
        print(f'    attributes=0x{pe.dword(cd+0x04):X} numBaseClasses={num} baseArray=0x{arr:X}')
        for k in range(min(num or 0, 8)):
            bcd = pe.dword(arr + 4 * k)
            if not bcd:
                continue
            btd = pe.dword(bcd + 0x00)
            bname = td2name.get(btd, f'<td 0x{btd:X}>')
            mdisp = pe.dword(bcd + 0x08)
            pdisp = pe.dword(bcd + 0x0C)
            vdisp = pe.dword(bcd + 0x10)
            ncb = pe.dword(bcd + 0x04)
            print(f'    base[{k}] {bname}  td=0x{btd:X} numContained={ncb} '
                  f'mdisp={struct.unpack("<i", struct.pack("<I", mdisp))[0]} '
                  f'pdisp={struct.unpack("<i", struct.pack("<I", pdisp))[0]} '
                  f'vdisp={struct.unpack("<i", struct.pack("<I", vdisp))[0]}')

def main():
    pe_path, tsv = sys.argv[1], sys.argv[2]
    anchors = []
    if '--anchors' in sys.argv:
        anchors = [int(x, 16) for x in sys.argv[sys.argv.index('--anchors') + 1].split(',')]
    cls = sys.argv[sys.argv.index('--class') + 1] if '--class' in sys.argv else None
    maxslot = int(sys.argv[sys.argv.index('--maxslot') + 1]) if '--maxslot' in sys.argv else 16
    pe = PE(pe_path)
    slotmap, vt_of = load_vtables(pe, tsv, maxslot)
    print(f'vftable slot targets mapped: {len(slotmap)} distinct functions\n')

    if cls:
        print(f'== RTTI inheritance chain matching {cls!r}')
        rtti_chain(pe, tsv, cls)
        print()

    if anchors:
        calls = callers_of(pe, set(anchors))
        for a in anchors:
            n = len(calls[a])
            print(f'== anchor 0x{a:06X}: {n} E8 call sites')
            rows = []
            for site in calls[a]:
                fn = pe.owner(site)
                rows.append((site, fn))
            # group by owning function
            groups = {}
            for site, fn in rows:
                groups.setdefault(fn[0] if fn else None, []).append(site)
            for fb, sites in sorted(groups.items(), key=lambda kv: -(len(kv[1]))):
                if fb is None:
                    print(f'   no-pdata              x{len(sites)}') 
                    continue
                sl = slotmap.get(fb)
                if sl:
                    desc = ', '.join(f'{c.split("@@")[0]}slot{s}' for c, s in sl[:4])
                    print(f'   *SLOT* func 0x{fb:06X} x{len(sites)}   <-- {desc}')
                else:
                    print(f'   func 0x{fb:06X} x{len(sites)}')

if __name__ == '__main__':
    main()
