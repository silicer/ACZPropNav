#!/usr/bin/env python3
"""Build a function index for a PE from llvm-objdump output + .pdata + strings.

Produces, for each function (from the .pdata RUNTIME_FUNCTION table):
  RVA, size, and the sorted list of data addresses it references (with the
  matching string if the address falls on a string from extract_strings.py).

Usage:
  build_func_index.py <pe> <disasm.txt> <strings.txt> [out.txt]

Why .pdata: the DLL is built /GS+SEH so every non-leaf function has an entry,
which gives exact function boundaries without a symbol table.
"""
import re
import struct
import sys

def parse_sections(buf):
    e = struct.unpack_from('<I', buf, 0x3C)[0]
    coff = e + 4
    nsec = struct.unpack_from('<H', buf, coff + 2)[0]
    optsize = struct.unpack_from('<H', buf, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from('<H', buf, opt)[0]
    ib = struct.unpack_from('<Q', buf, opt + 24)[0] if magic == 0x20b else struct.unpack_from('<I', buf, opt + 28)[0]
    s = opt + optsize
    out = []
    for i in range(nsec):
        o = s + i * 40
        name = buf[o:o+8].rstrip(b'\0').decode('latin1')
        vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', buf, o + 8)
        out.append((name, vaddr, vsize, rawptr, rawsize))
    return ib, out

def rva2fo(secs, rva):
    for name, va, vs, rp, rs in secs:
        if va <= rva < va + max(vs, rs):
            return rp + (rva - va)
    return None

def read_pdata(buf, secs):
    """Return [(begin_rva, end_rva)] sorted."""
    pd = next((s for s in secs if s[0] == '.pdata'), None)
    if not pd:
        return []
    name, va, vs, rp, rs = pd
    out = []
    for i in range(0, vs, 12):
        begin, end, unwind = struct.unpack_from('<III', buf, rp + i)
        if 0 < begin < end < 0x1000000:
            out.append((begin, end))
    out.sort()
    return out

def load_strings(path):
    """addr -> text (max-prefix match).  strings.txt lines: OFFSET hex  enc  text"""
    d = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        if len(line) < 12:
            continue
        off = int(line.split('  ')[0], 16)
        rest = line.split('  ', 2)
        if len(rest) < 3:
            continue
        d[off] = rest[2]
    return d

def main():
    pe, dis, strf = sys.argv[1], sys.argv[2], sys.argv[3]
    outf = sys.argv[4] if len(sys.argv) > 4 else None
    buf = open(pe, 'rb').read()
    ib, secs = parse_sections(buf)
    funcs = read_pdata(buf, secs)
    # file offset -> rva for the string table
    str_by_rva = {}
    for fo, txt in load_strings(strf).items():
        for name, va, vs, rp, rs in secs:
            if rp <= fo < rp + rs:
                str_by_rva[va + (fo - rp)] = txt
                break
    str_rvas = sorted(str_by_rva)

    import bisect
    def str_at(rva):
        i = bisect.bisect_right(str_rvas, rva) - 1
        if i < 0:
            return None
        s = str_rvas[i]
        if rva - s < len(str_by_rva[s]):
            return str_by_rva[s][rva - s:]
        return None

    insn = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$')
    refs_by_func = {}   # func idx -> {target_rva: count}
    order = []
    for line in open(dis, encoding='utf-8', errors='replace'):
        m = insn.match(line)
        if not m:
            continue
        va = int(m.group(1), 16)
        rva = va - ib
        # find function by binary search on begin
        lo, hi = 0, len(funcs) - 1
        idx = None
        while lo <= hi:
            mid = (lo + hi) // 2
            b, e = funcs[mid]
            if rva < b:
                hi = mid - 1
            elif rva >= e:
                lo = mid + 1
            else:
                idx = mid
                break
        if idx is None:
            continue
        if idx not in refs_by_func:
            refs_by_func[idx] = {}
            order.append(idx)
        body = m.group(3)
        cm = re.search(r'#\s*0x([0-9a-f]+)', body)
        if cm:
            tgt = int(cm.group(1), 16) - ib
            refs_by_func[idx][tgt] = refs_by_func[idx].get(tgt, 0) + 1

    lines = []
    for idx in order:
        b, e = funcs[idx]
        lines.append(f'FUNC rva=0x{b:06X} size=0x{e-b:X} insns_refs={len(refs_by_func[idx])}')
        items = []
        for tgt, cnt in sorted(refs_by_func[idx].items(), key=lambda kv: -kv[1]):
            s = str_at(tgt)
            if s:
                s = s.replace('\n', '\\n')[:110]
                items.append(f'    -> 0x{tgt:06X} x{cnt}  {s!r}')
            else:
                items.append(f'    -> 0x{tgt:06X} x{cnt}')
        lines.extend(items)
    txt = '\n'.join(lines)
    if outf:
        open(outf, 'w', encoding='utf-8').write(txt)
        print(f'wrote {outf}: {len(lines)} lines, {len(order)} functions')
    else:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
        print(txt)

if __name__ == '__main__':
    main()
