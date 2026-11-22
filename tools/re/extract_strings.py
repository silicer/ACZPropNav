#!/usr/bin/env python3
"""Extract UTF-8 and UTF-16LE strings with file offsets from a PE/text file.

Usage: extract_strings.py <file> [minlen]
Writes nothing; prints  offset  enc  string
Non-ASCII UTF-8 (e.g. Chinese, 3-byte sequences) is preserved as-is, so this
recovers the AC8PropNav.dll log templates that plain `strings` truncates.
"""
import sys

def is_printable_utf8(buf, i):
    b = buf[i]
    if b < 0x20:
        return 0
    if b < 0x7f:
        return 1
    if b < 0x80:
        return 0
    if b < 0xc0:
        return 0
    if b < 0xe0:
        if i + 1 < len(buf) and 0x80 <= buf[i+1] < 0xc0:
            return 2
        return 0
    if b < 0xf0:
        if i + 2 < len(buf) and 0x80 <= buf[i+1] < 0xc0 and 0x80 <= buf[i+2] < 0xc0:
            return 3
        return 0
    if b < 0xf8:
        if i + 3 < len(buf) and all(0x80 <= buf[i+k] < 0xc0 for k in (1,2,3)):
            return 4
        return 0
    return 0

def utf8_strings(buf, minlen):
    out = []
    i, n = 0, len(buf)
    while i < n:
        start = i
        chars = []
        while i < n:
            l = is_printable_utf8(buf, i)
            if not l:
                break
            chars.append(buf[i:i+l].decode('utf-8', 'replace'))
            i += l
        if i - start >= minlen and chars:
            out.append((start, 'u8', ''.join(chars)))
        i += 1 if i == start else 0
    return out

def utf16_strings(buf, minlen):
    """ASCII-only UTF-16LE runs (safe heuristic; Chinese via UTF-8 path)."""
    out = []
    i, n = 0, len(buf) - 1
    while i < n:
        start = i
        chars = []
        while i < n:
            lo, hi = buf[i], buf[i+1]
            if hi == 0 and 0x20 <= lo < 0x7f:
                chars.append(chr(lo)); i += 2
            else:
                break
        if i - start >= max(minlen, 6) * 2 and chars:
            out.append((start, 'u16', ''.join(chars)))
        i += 2 if i == start else 0
    return out

def main():
    path = sys.argv[1]
    minlen = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    buf = open(path, 'rb').read()
    lines = [f'{off:08X}  {enc}  {s}'
             for off, enc, s in sorted(utf8_strings(buf, minlen) + utf16_strings(buf, minlen))]
    open(sys.argv[3], 'w', encoding='utf-8') if len(sys.argv) > 3 else None
    if len(sys.argv) > 3:
        open(sys.argv[3], 'w', encoding='utf-8').write('\n'.join(lines))
    else:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
        print('\n'.join(lines))

if __name__ == '__main__':
    main()
