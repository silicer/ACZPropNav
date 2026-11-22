#!/usr/bin/env python3
"""arc_tool.py -- dependency-free reader for ACE COMBAT ZERO (Steam) .arc archives.

Why this exists
---------------
`ACZero/{a,b,c,d,e}.arc` are NOT plain LZ4 archives.  The body of every archive is
obfuscated with a 32-bit XOR keystream, so the files look like uniform noise and
no plaintext index is visible.  This tool implements the container from scratch:

  * no `lz4` python module needed (pure-python LZ4 block decoder),
  * no `numpy` needed (optional acceleration for the XOR keystream),
  * read-only: it never writes to the archive.

Commands
--------
  info    <arc>                       header/keys/checksum + one-line summary
  list    <arc> [--grep RE] [--long]  print the index (names + sizes)
  json    <arc> [--out F]             dump the whole index as JSON
  extract <arc> [--grep RE] [--out DIR] [--limit-n BYTES] [--max-file BYTES]
  find    <arc> --name SUBSTR         show index rows whose name contains SUBSTR

Examples
--------
  python arc_tool.py info     ACZero/e.arc
  python arc_tool.py list     ACZero/e.arc --grep '\\.wpb$'
  python arc_tool.py extract  ACZero/e.arc --grep 'mis|wpb|wpt' --out work/x
  python arc_tool.py json     ACZero/a.arc --out work/a.index.json

Format notes
------------
See ACZPropNav/docs/ARC.md for the full layout, field by field.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys

try:  # optional, only for speed
    import numpy as _np
except Exception:  # noqa: BLE001
    _np = None

M32 = 0xFFFFFFFF
OUTER_MAGIC = 0x454758C3          # first dword of every .arc
INNER_MAGIC = 0x414758E1          # first dword of the (xor'd) index
KEY_C1 = 0x5DF38C35
KEY_C2 = 0x4CE49F4D
HEADER_LEN = 12                   # outer magic + k1 + k2
TRAILER_LEN = 8                   # k3 + encrypted body checksum
CHUNK = 16 << 20                  # streaming read granularity


class ArcError(Exception):
    pass


# --------------------------------------------------------------------------- crypto
def keystream_xor(data: bytes, pos: int, a: int, b: int) -> bytes:
    """XOR `data` with the archive keystream; `data` starts at stream position `pos`.

    The keystream is a dword sequence:  ks[i] = A + B * (pos>>2 + i)   (mod 2**32)
    applied little-endian.  Stream position 0 is file offset 12.
    """
    if not data or (a == 0 and b == 0):
        return bytes(data)
    n = len(data)
    pad = pos & 3
    if _np is not None:
        total = ((pad + n + 3) // 4) * 4
        tmp = _np.zeros(total, dtype=_np.uint8)
        tmp[pad:pad + n] = _np.frombuffer(data, dtype=_np.uint8)
        w = tmp.view("<u4")
        idx = _np.arange(len(w), dtype=_np.uint64) + _np.uint64(pos >> 2)
        w ^= ((_np.uint64(a) + _np.uint64(b) * idx) & _np.uint64(M32)).astype(_np.uint32)
        return tmp[pad:pad + n].tobytes()
    # pure-python fallback
    out = bytearray(data)
    pad = pos & 3
    start = (pos >> 2) & M32
    # bring to a dword boundary
    buf = bytearray(4 - pad) + out if pad else out
    for i in range(0, len(buf) - len(buf) % 4, 4):
        ks = (a + b * ((start + (i + pad) // 4) & M32)) & M32
        buf[i] ^= ks & 0xFF
        buf[i + 1] ^= (ks >> 8) & 0xFF
        buf[i + 2] ^= (ks >> 16) & 0xFF
        buf[i + 3] ^= (ks >> 24) & 0xFF
    return bytes(buf[(4 - pad) if pad else 0:])


def rotr_sum(buf: bytes) -> int:
    """sum(ror32(dword, 7)) over `buf`; the game's body checksum primitive."""
    buf = buf[:len(buf) // 4 * 4]
    if not buf:
        return 0
    if _np is not None:
        w = _np.frombuffer(buf, dtype="<u4").astype(_np.uint64)
        return int((((w >> _np.uint64(7)) | (w << _np.uint64(25))) & _np.uint64(M32)).sum()) & M32
    s = 0
    for (v,) in struct.iter_unpack("<I", buf):
        s = (s + (((v >> 7) | (v << 25)) & M32)) & M32
    return s


# ----------------------------------------------------------------------------- LZ4
def lz4_block_decompress(src: bytes, expected: int | None = None) -> bytes:
    """Pure-python LZ4 *block* decoder (no frame header, no checksum).

    This is the concrete implementation of the LZ4 block format; it is roughly
    10-20 MiB/s, which is fine for selective extraction.  Raises ArcError on a
    malformed block.
    """
    dst = bytearray()
    i = 0
    n = len(src)
    src_mv = src
    try:
        while i < n:
            token = src_mv[i]
            i += 1
            lit = token >> 4
            if lit == 15:
                while True:
                    b = src_mv[i]
                    i += 1
                    lit += b
                    if b != 255:
                        break
            if lit:
                dst += src_mv[i:i + lit]
                i += lit
            if i >= n:
                break
            if i + 2 > n:
                raise ArcError("truncated match offset")
            off = src_mv[i] | (src_mv[i + 1] << 8)
            i += 2
            if off == 0 or off > len(dst):
                raise ArcError(f"invalid match offset {off} (dst len {len(dst)})")
            ml = token & 0x0F
            if ml == 15:
                while True:
                    b = src_mv[i]
                    i += 1
                    ml += b
                    if b != 255:
                        break
            ml += 4
            start = len(dst) - off
            if off >= ml:                      # non-overlapping: one slice copy
                dst += dst[start:start + ml]
            else:                              # overlapping run: repeat pattern
                chunk = dst[start:]
                reps = ml // len(chunk) + 1
                dst += (chunk * reps)[:ml]
    except IndexError as exc:
        raise ArcError(f"truncated LZ4 block: {exc}") from exc
    if expected is not None and len(dst) != expected:
        raise ArcError(f"LZ4 size mismatch: got {len(dst)}, expected {expected}")
    return bytes(dst)


# ------------------------------------------------------------------------------ arc
class Entry:
    __slots__ = ("index", "name", "off", "raw", "stored")

    def __init__(self, index, name: bytes, off: int, raw: int, stored: int):
        self.index, self.name, self.off, self.raw, self.stored = index, name, off, raw, stored

    @property
    def text(self) -> str:
        return self.name.decode("latin-1")

    @property
    def compressed(self) -> bool:
        return self.raw != self.stored

    def __repr__(self) -> str:
        return f"<Entry {self.text!r} off={self.off} raw={self.raw} stored={self.stored}>"


class Arc:
    """Read-only view of an .arc archive."""

    def __init__(self, path: str):
        self.path = path
        self.size = os.path.getsize(path)
        if self.size < HEADER_LEN + TRAILER_LEN + 16:
            raise ArcError(f"{path}: too small to be an archive ({self.size} bytes)")
        self.f = open(path, "rb")
        magic, k1, k2 = struct.unpack("<III", self.f.read(HEADER_LEN))
        if magic != OUTER_MAGIC:
            raise ArcError(f"{path}: bad outer magic {magic:08x} "
                           f"(expected {OUTER_MAGIC:08x})")
        self.k1, self.k2 = k1, k2
        self.f.seek(self.size - TRAILER_LEN)
        self.k3, self.enc_checksum = struct.unpack("<II", self.f.read(TRAILER_LEN))
        self.A = (k1 ^ self.k3 ^ KEY_C1) & M32
        self.B = (k2 ^ self.k3 ^ KEY_C2) & M32
        self.body_len = self.size - HEADER_LEN - TRAILER_LEN

        magic2, count, nsz, ver = struct.unpack("<IIII", self.read(0, 16))
        if magic2 != INNER_MAGIC:
            raise ArcError(f"{path}: bad inner magic {magic2:08x} "
                           f"(expected {INNER_MAGIC:08x}) -- index damaged")
        if ver > 1:
            raise ArcError(f"{path}: unsupported index version {ver}")
        self.version, self.count, self.names_len = ver, count, nsz
        names = self.read(16, nsz).split(b"\0")
        if len(names) < count:
            raise ArcError(f"{path}: name table has {len(names)} names < count {count}")
        esz = 12 if ver else 8
        table = self.read(16 + nsz, count * esz)
        self.index_len = 16 + nsz + count * esz
        self.entries: list[Entry] = []
        for i in range(count):
            if ver:
                off, raw, stored = struct.unpack_from("<III", table, i * esz)
            else:
                off, raw = struct.unpack_from("<II", table, i * esz)
                stored = raw
            self.entries.append(Entry(i, names[i], off, raw, stored))

    # -- io -----------------------------------------------------------------
    def read(self, pos: int, n: int) -> bytes:
        self.f.seek(HEADER_LEN + pos)
        data = self.f.read(n)
        if len(data) != n:
            raise ArcError(f"{self.path}: short read at stream pos {pos} "
                           f"(wanted {n}, got {len(data)})")
        return keystream_xor(data, pos, self.A, self.B)

    def extract(self, e: Entry) -> bytes:
        data = self.read(e.off, e.stored)
        if e.compressed:
            data = lz4_block_decompress(data, e.raw)
        return data

    def iter_chunks(self, e: Entry):
        done = 0
        while done < e.stored:
            n = min(CHUNK, e.stored - done)
            yield self.read(e.off + done, n)
            done += n

    def verify(self) -> tuple[bool, int, int]:
        """Recompute the body checksum the way the game does."""
        s = 0
        self.f.seek(HEADER_LEN)
        left = self.body_len
        while left > 0:
            chunk = self.f.read(min(left, 64 << 20))
            if not chunk:
                break
            left -= len(chunk)
            s = (s + rotr_sum(chunk)) & M32
        want = struct.unpack("<I", keystream_xor(
            struct.pack("<I", self.enc_checksum), self.body_len, self.A, self.B))[0]
        return s == want, s, want

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


# --------------------------------------------------------------------------- utils
def fmt_size(n: int) -> str:
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.2f} {unit}"
        n /= 1024.0
    return f"{n} B"


def safe_join(root: str, name: str) -> str:
    norm = name.replace("\\", "/").lstrip("/")
    parts = [p for p in norm.split("/") if p not in ("", ".", "..")]
    return os.path.join(root, *parts)


def filter_entries(entries, pattern):
    if not pattern:
        return entries
    rx = re.compile(pattern, re.IGNORECASE)
    return [e for e in entries if rx.search(e.text)]


# ------------------------------------------------------------------------- commands
def cmd_info(args):
    with Arc(args.arc) as arc:
        print(f"file            : {arc.path}")
        print(f"size            : {arc.size} bytes ({fmt_size(arc.size)})")
        print(f"outer magic     : {OUTER_MAGIC:08x}  (OK)")
        print(f"k1, k2, k3      : {arc.k1:08x} {arc.k2:08x} {arc.k3:08x}")
        print(f"keystream A, B  : {arc.A:08x} {arc.B:08x}"
              f"{'   (identity: body not encrypted)' if arc.A == 0 and arc.B == 0 else ''}")
        print(f"index version   : {arc.version}")
        print(f"file count      : {arc.count}")
        print(f"name table      : {arc.names_len} bytes")
        print(f"index length    : {arc.index_len} bytes")
        print(f"body length     : {arc.body_len} bytes")
        comp = sum(1 for e in arc.entries if e.compressed)
        raw = sum(e.raw for e in arc.entries)
        stored = sum(e.stored for e in arc.entries)
        print(f"compressed      : {comp}/{arc.count} entries")
        print(f"total raw/stored: {raw} / {stored}")
        ok, got, want = arc.verify()
        print(f"checksum        : {'OK' if ok else 'MISMATCH'}  computed={got:08x} "
              f"expected={want:08x}")
        return 0 if ok else 3


def cmd_list(args):
    with Arc(args.arc) as arc:
        entries = filter_entries(arc.entries, args.grep)
        if args.long:
            print(f"{'off':>12} {'raw':>12} {'stored':>12}  {'c':1} name")
        total = 0
        for e in entries:
            total += e.raw
            if args.long:
                print(f"{e.off:12d} {e.raw:12d} {e.stored:12d}  "
                      f"{'C' if e.compressed else 's'} {e.text}")
            else:
                print(e.text)
        print(f"--- {len(entries)}/{arc.count} entries, {total} raw bytes "
              f"({fmt_size(total)})", file=sys.stderr)
        return 0


def cmd_json(args):
    with Arc(args.arc) as arc:
        doc = {
            "path": arc.path,
            "size": arc.size,
            "keys": {"k1": arc.k1, "k2": arc.k2, "k3": arc.k3, "A": arc.A, "B": arc.B},
            "index_version": arc.version,
            "index_len": arc.index_len,
            "count": arc.count,
            "entries": [
                {"i": e.index, "name": e.text, "off": e.off, "raw": e.raw,
                 "stored": e.stored, "lz4": e.compressed}
                for e in filter_entries(arc.entries, args.grep)
            ],
        }
        out = json.dumps(doc, indent=1)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(out)
        print(f"wrote {args.out} ({arc.count} entries)")
    else:
        print(out)
    return 0


def cmd_extract(args):
    os.makedirs(args.out, exist_ok=True)
    with Arc(args.arc) as arc:
        entries = filter_entries(arc.entries, args.grep)
        if args.max_file:
            entries = [e for e in entries if e.raw <= args.max_file]
        if args.limit_n:
            entries = entries[:args.limit_n]
        written = skipped = failed = 0
        bytes_out = 0
        for e in entries:
            dst = safe_join(args.out, e.text)
            try:
                os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
                data = arc.extract(e)
            except ArcError as exc:
                failed += 1
                print(f"FAIL {e.text}: {exc}", file=sys.stderr)
                continue
            if os.path.exists(dst) and not args.force:
                skipped += 1
                continue
            with open(dst, "wb") as f:
                f.write(data)
            written += 1
            bytes_out += len(data)
            if args.verbose:
                print(f"  {e.text}  {len(data)}")
        print(f"extracted {written} files, {bytes_out} bytes ({fmt_size(bytes_out)}) "
              f"to {args.out}; skipped {skipped} existing, {failed} failed")
        return 0 if failed == 0 else 4


def cmd_find(args):
    with Arc(args.arc) as arc:
        pat = args.name.lower()
        hits = [e for e in arc.entries if pat in e.text.lower()]
        for e in hits:
            print(f"{e.off:12d} {e.raw:12d} {'C' if e.compressed else 's'} {e.text}")
        print(f"--- {len(hits)} hits", file=sys.stderr)
    return 0


def build_parser():
    p = argparse.ArgumentParser(
        description="Read ACE COMBAT ZERO (Steam) .arc archives without the lz4 module.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Format notes")[0])
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp, need_arc=True):
        if need_arc:
            sp.add_argument("arc", help="path to the .arc file (read-only)")

    sp = sub.add_parser("info", help="header, keys, checksum, totals")
    common(sp); sp.set_defaults(func=cmd_info)

    sp = sub.add_parser("list", help="print the archive index")
    common(sp)
    sp.add_argument("--grep", default=None, help="regex on entry name")
    sp.add_argument("--long", action="store_true", help="include offset/size columns")
    sp.set_defaults(func=cmd_list)

    sp = sub.add_parser("json", help="dump the index as JSON")
    common(sp)
    sp.add_argument("--grep", default=None)
    sp.add_argument("--out", default=None, help="write JSON here instead of stdout")
    sp.set_defaults(func=cmd_json)

    sp = sub.add_parser("extract", help="extract entries to a directory")
    common(sp)
    sp.add_argument("--grep", default=None, help="regex on entry name (default: all)")
    sp.add_argument("--out", default="extracted", help="output directory")
    sp.add_argument("--max-file", type=int, default=None, help="skip entries larger than N raw bytes")
    sp.add_argument("--limit-n", type=int, default=None, help="only the first N selected entries")
    sp.add_argument("--force", action="store_true", help="overwrite existing files")
    sp.add_argument("--verbose", action="store_true")
    sp.set_defaults(func=cmd_extract)

    sp = sub.add_parser("find", help="substring search on entry names")
    common(sp)
    sp.add_argument("--name", required=True)
    sp.set_defaults(func=cmd_find)
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except ArcError as exc:
        print(f"arc_tool: {exc}", file=sys.stderr)
        return 2
    except BrokenPipeError:
        return 0


if __name__ == "__main__":
    sys.exit(main())
