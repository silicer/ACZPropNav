#!/usr/bin/env python3
import argparse, json, os, posixpath, secrets, struct, sys, time
import numpy as np
import lz4.block

M = 0xFFFFFFFF
OUTER_MAGIC = 0x454758C3
INNER_MAGIC = 0x414758E1
KEY_C1, KEY_C2 = 0x5DF38C35, 0x4CE49F4D
CHUNK = 16 << 20
MANIFEST = ".aczarc.json"
LANGS = {"jp": "lang/0jp/", "us": "lang/1us/", "uk": "lang/2uk/", "de": "lang/3ge/",
         "fr": "lang/4fr/", "it": "lang/5it/", "es": "lang/6es/"}
ORIGINAL_ARCHIVES = {"a.arc", "b.arc", "c.arc", "d.arc", "e.arc"}
NO_AUTO_COMPRESS = {".webm", ".pac", ".png"}   # streamed or already compressed
AUTO_COMPRESS_LIMIT = 64 << 20


class ArcError(Exception):
    pass


def keystream_xor(data, pos, A, B):
    """XOR `data` (which starts at stream position `pos`) with the archive keystream."""
    if not data or (A == 0 and B == 0):
        return bytes(data)
    pad = pos & 3
    n = len(data)
    tmp = np.zeros(((pad + n + 3) // 4) * 4, dtype=np.uint8)
    tmp[pad:pad + n] = np.frombuffer(data, dtype=np.uint8)
    w = tmp.view("<u4")
    idx = np.arange(len(w), dtype=np.uint64) + np.uint64(pos >> 2)
    w ^= ((np.uint64(A) + np.uint64(B) * idx) & np.uint64(M)).astype(np.uint32)
    return tmp[pad:pad + n].tobytes()


def rotr_sum(buf):
    """Sum of ror32(x, 7) over the little-endian dwords of `buf` (length multiple of 4)."""
    if not buf:
        return 0
    w = np.frombuffer(buf, dtype="<u4").astype(np.uint64)
    return int((((w >> np.uint64(7)) | (w << np.uint64(25))) & np.uint64(M)).sum()) & M


def normkey(name):
    """Comparison key for matching disk paths to archive names ("data/../x" == "x", any case)."""
    return posixpath.normpath(name.replace("\\", "/").lstrip("/")).lower()


def safe_join(root, name):
    norm = posixpath.normpath(name.replace("\\", "/").lstrip("/"))
    parts = [p for p in norm.split("/") if p not in ("", ".", "..")]
    return os.path.join(root, *parts)


def fmt_mib(n):
    return f"{n / 2**20:.1f} MiB"


class Entry:
    __slots__ = ("name", "off", "raw", "stored")

    def __init__(self, name, off, raw, stored):
        self.name, self.off, self.raw, self.stored = name, off, raw, stored

    @property
    def text(self):
        return self.name.decode("latin-1")

    @property
    def compressed(self):
        return self.raw != self.stored


class Arc:
    """Read access to an .arc file."""

    def __init__(self, path):
        self.path = path
        self.size = os.path.getsize(path)
        if self.size < 36:
            raise ArcError(f"{path}: too small to be an archive")
        self.f = open(path, "rb")
        magic, k1, k2 = struct.unpack("<III", self.f.read(12))
        if magic != OUTER_MAGIC:
            self.f.close()
            raise ArcError(f"{path}: not an Ace Combat Zero archive (magic {magic:08x})")
        self.f.seek(self.size - 8)
        self.k3, self.enc_checksum = struct.unpack("<II", self.f.read(8))
        self.A = (k1 ^ self.k3 ^ KEY_C1) & M
        self.B = (k2 ^ self.k3 ^ KEY_C2) & M
        magic2, count, nsz, ver = struct.unpack("<IIII", self.read(0, 16))
        if magic2 != INNER_MAGIC or ver > 1:
            raise ArcError(f"{path}: index is damaged (inner magic {magic2:08x}, version {ver})")
        self.version = ver
        names = self.read(16, nsz).split(b"\0")
        if len(names) < count:
            raise ArcError(f"{path}: name table is shorter than the file count")
        esz = 12 if ver else 8
        table = self.read(16 + nsz, count * esz)
        self.entries = []
        for i in range(count):
            if ver:
                off, raw, stored = struct.unpack_from("<III", table, i * esz)
            else:
                off, raw = struct.unpack_from("<II", table, i * esz)
                stored = raw
            self.entries.append(Entry(names[i], off, raw, stored))

    def close(self):
        self.f.close()

    def read(self, pos, n):
        self.f.seek(12 + pos)
        data = self.f.read(n)
        if len(data) != n:
            raise ArcError(f"{self.path}: truncated read at stream position {pos}")
        return keystream_xor(data, pos, self.A, self.B)

    def iter_stored(self, e):
        done = 0
        while done < e.stored:
            n = min(CHUNK, e.stored - done)
            yield self.read(e.off + done, n)
            done += n

    def extract(self, e):
        data = self.read(e.off, e.stored)
        if e.compressed:
            data = lz4.block.decompress(data, uncompressed_size=e.raw)
            if len(data) != e.raw:
                raise ArcError(f"{e.text}: LZ4 size mismatch {len(data)} != {e.raw}")
        return data

    def verify(self):
        """The game's integrity check. Returns (ok, computed, expected)."""
        body = self.size - 20
        s = 0
        self.f.seek(12)
        left = body
        while left > 0:
            chunk = self.f.read(min(left, 64 << 20))
            left -= len(chunk)
            s = (s + rotr_sum(chunk[:len(chunk) // 4 * 4])) & M
        want = struct.unpack("<I", keystream_xor(struct.pack("<I", self.enc_checksum), body, self.A, self.B))[0]
        return s == want, s, want


# ----------------------------------------------------------------------------- sources

class FileSource:
    """A file on disk that goes into an archive."""

    def __init__(self, path, hint_compressed=None):
        self.path = path
        self.key = ("file", os.path.abspath(path).lower())
        self.hint_compressed = hint_compressed

    def label(self):
        return self.path

    def prepare(self, version, mode, hc):
        raw = os.path.getsize(self.path)
        ext = os.path.splitext(self.path)[1].lower()
        want = False
        if version == 1 and raw > 0:
            if mode == "all":
                want = True
            elif mode == "auto":
                want = ext not in NO_AUTO_COMPRESS and raw <= AUTO_COMPRESS_LIMIT
            elif mode == "original":
                want = bool(self.hint_compressed) if self.hint_compressed is not None else (
                    ext not in NO_AUTO_COMPRESS and raw <= AUTO_COMPRESS_LIMIT)
        if want:
            with open(self.path, "rb") as fh:
                data = fh.read()
            comp = lz4.block.compress(data, mode="high_compression" if hc else "default",
                                      compression=9 if hc else 0, store_size=False)
            if len(comp) < raw:   # equal sizes would read back as "stored"
                return raw, len(comp), [comp]
            return raw, raw, [data]
        return raw, raw, self._chunks()

    def _chunks(self):
        with open(self.path, "rb") as fh:
            while True:
                b = fh.read(CHUNK)
                if not b:
                    break
                yield b


class BaseSource:
    """An entry copied from an existing archive (stored bytes are reused, not recompressed)."""

    def __init__(self, arc, e):
        self.arc, self.e = arc, e
        self.key = ("base", arc.path, e.off, e.stored, e.raw)

    def label(self):
        return f"{os.path.basename(self.arc.path)}:{self.e.text}"

    def prepare(self, version, mode, hc):
        if self.e.compressed and version == 0:
            data = self.arc.extract(self.e)   # v0 archives cannot hold compressed entries
            return self.e.raw, self.e.raw, [data]
        return self.e.raw, self.e.stored, self.arc.iter_stored(self.e)


# ----------------------------------------------------------------------------- writer

def write_archive(out_path, items, version=1, mode="auto", plain=False, hc=False, log=print):
    """items: list of (name_bytes, source). Writes to out_path (a temp path) and returns stats."""
    if plain:
        k3 = 0
        k1, k2 = KEY_C1, KEY_C2       # makes A = B = 0, so the body is stored unencrypted
    else:
        k1, k2, k3 = (secrets.randbits(32) for _ in range(3))
    A, B = (k1 ^ k3 ^ KEY_C1) & M, (k2 ^ k3 ^ KEY_C2) & M

    names = b"".join(n + b"\0" for n, _ in items)
    names += b"\0" * ((-len(names)) & 3)
    esz = 12 if version else 8
    header_len = 16 + len(names) + esz * len(items)

    t0 = time.time()
    stats = {"files": len(items), "raw": 0, "stored": 0, "lz4": 0, "shared": 0}
    table = []
    written = {}
    with open(out_path, "wb") as f:
        f.write(struct.pack("<III", OUTER_MAGIC, k1, k2))
        f.seek(12 + header_len)
        pos, csum, carry = header_len, 0, b""
        last_report = time.time()
        for i, (name, src) in enumerate(items):
            if src.key in written:
                table.append(written[src.key])
                stats["shared"] += 1
                continue
            off = pos
            raw, stored, chunks = src.prepare(version, mode, hc)
            n = 0
            for chunk in chunks:
                enc = keystream_xor(chunk, pos, A, B)
                f.write(enc)
                buf = carry + enc
                cut = len(buf) & ~3
                csum = (csum + rotr_sum(buf[:cut])) & M
                carry = buf[cut:]
                pos += len(chunk)
                n += len(chunk)
            if n != stored:
                raise ArcError(f"{src.label()}: expected {stored} bytes, got {n} (file changed while packing?)")
            if pos > M:
                raise ArcError("archive would exceed 4 GiB, which the index format cannot address")
            rec = (off, raw, stored)
            written[src.key] = rec
            table.append(rec)
            stats["raw"] += raw
            stats["stored"] += stored
            stats["lz4"] += raw != stored
            if time.time() - last_report > 5:
                log(f"  {i + 1}/{len(items)} files, {fmt_mib(pos)} written")
                last_report = time.time()
        pad = (-pos) & 3
        if pad:
            enc = keystream_xor(b"\0" * pad, pos, A, B)
            f.write(enc)
            buf = carry + enc
            csum = (csum + rotr_sum(buf)) & M
            carry = b""
            pos += pad
        assert not carry
        body_len = pos

        hdr = bytearray(struct.pack("<IIII", INNER_MAGIC, len(items), len(names), version))
        hdr += names
        for off, raw, stored in table:
            hdr += struct.pack("<III", off, raw, stored) if version else struct.pack("<II", off, raw)
        assert len(hdr) == header_len
        enc = keystream_xor(bytes(hdr), 0, A, B)
        f.seek(12)
        f.write(enc)
        csum = (csum + rotr_sum(enc)) & M

        f.seek(12 + body_len)
        f.write(struct.pack("<I", k3) + keystream_xor(struct.pack("<I", csum), body_len, A, B))
    stats["seconds"] = time.time() - t0
    stats["size"] = 12 + body_len + 8
    return stats


def check_output(path, expect_count):
    arc = Arc(path)
    try:
        ok, got, want = arc.verify()
        if not ok:
            raise ArcError(f"{path}: checksum mismatch after writing ({got:08x} != {want:08x})")
        if len(arc.entries) != expect_count:
            raise ArcError(f"{path}: wrote {len(arc.entries)} entries, expected {expect_count}")
    finally:
        arc.close()


def prepare_output(out, force):
    if os.path.exists(out) and not force:
        raise ArcError(f"{out} already exists. Pick another name or pass --force to replace it.")
    d = os.path.dirname(os.path.abspath(out))
    os.makedirs(d, exist_ok=True)
    return out + ".tmp"


def finalize_output(tmp, out, log):
    if os.path.exists(out):
        bak = out + ".bak"
        if os.path.basename(out).lower() in ORIGINAL_ARCHIVES and not os.path.exists(bak):
            os.replace(out, bak)
            log(f"  backed up the original to {bak}")
    os.replace(tmp, out)


def run_write(out, items, args, open_arcs=(), log=print):
    tmp = prepare_output(out, args.force)
    try:
        stats = write_archive(tmp, items, version=args.version, mode=args.compress,
                              plain=args.plain, hc=args.hc, log=log)
        check_output(tmp, len(items))
    except BaseException:
        for a in open_arcs:
            a.close()
        if os.path.exists(tmp):
            os.remove(tmp)
        raise
    for a in open_arcs:
        a.close()
    finalize_output(tmp, out, log)
    log(f"wrote {out}: {stats['files']} files ({stats['lz4']} LZ4"
        + (f", {stats['shared']} sharing data" if stats["shared"] else "")
        + f"), {fmt_mib(stats['size'])} in {stats['seconds']:.1f}s, checksum OK"
        + (", unencrypted" if args.plain else ""))


# ----------------------------------------------------------------------------- helpers

def collect_files(folder):
    if not os.path.isdir(folder):
        raise ArcError(f"{folder} is not a folder")
    out = []
    for root, dirs, files in os.walk(folder):
        dirs.sort()
        for fn in sorted(files):
            if fn == MANIFEST or fn.endswith(".tmp"):
                continue
            full = os.path.join(root, fn)
            rel = os.path.relpath(full, folder).replace(os.sep, "/")
            out.append((rel, full))
    return out


def parse_adds(adds):
    """--add LOCAL=ARCHIVE_NAME pairs."""
    out = []
    for a in adds or []:
        if "=" not in a:
            raise ArcError(f"--add needs LOCAL_FILE=ARCHIVE_NAME, got {a!r}")
        local, name = a.split("=", 1)
        if not os.path.isfile(local):
            raise ArcError(f"--add: {local} does not exist")
        out.append((name.replace("\\", "/"), local))
    return out


def plan_patch(base, mods, log):
    """base: Arc; mods: list of (name_bytes, source). Replace by name (keeping the base's exact
    spelling), append the rest. Returns items and (replaced, added) name lists."""
    index = {}
    for i, e in enumerate(base.entries):
        index.setdefault(normkey(e.text), i)
    replace, added, added_keys = {}, [], {}
    for name, src in mods:
        k = normkey(name.decode("latin-1"))
        if k in index:
            replace[index[k]] = src
        elif k in added_keys:
            added[added_keys[k]] = (name, src)
        else:
            added_keys[k] = len(added)
            added.append((name, src))
    items = [(e.name, replace.get(i) or BaseSource(base, e)) for i, e in enumerate(base.entries)]
    items += added
    return items, [base.entries[i].text for i in sorted(replace)], [n.decode("latin-1") for n, _ in added]


def report_plan(replaced, added, log, extra=""):
    log(f"  {len(replaced)} replaced, {len(added)} added{extra}")
    for n in replaced[:40]:
        log(f"    replace  {n}")
    if len(replaced) > 40:
        log(f"    ... {len(replaced) - 40} more")
    for n in added[:40]:
        log(f"    add      {n}")
    if len(added) > 40:
        log(f"    ... {len(added) - 40} more")


def known_game_names(game_dir, skip_path=None, names=("a.arc", "b.arc", "c.arc", "e.arc")):
    """Exact names of every file in the game's archives (a/b/c/e by default), keyed by normkey."""
    known = {}
    for n in names:
        p = os.path.join(game_dir, n)
        if not os.path.exists(p) or (skip_path and os.path.samefile(p, skip_path)):
            continue
        arc = Arc(p)
        for e in arc.entries:
            known.setdefault(normkey(e.text), (e.text, n))
        arc.close()
    return known


def default_game_dir():
    for d in (os.getcwd(), os.path.dirname(os.path.abspath(__file__))):
        if os.path.exists(os.path.join(d, "a.arc")):
            return d
    raise ArcError("can't find the game's a.arc here or next to aczarc.py; pass --game-dir")


def match_game_name(rel, known):
    """The game's exact spelling of `rel` (also for lang/<xx>/ copies of base files), or None."""
    k = normkey(rel)
    if k in known:
        return known[k][0]
    for prefix in LANGS.values():
        if k.startswith(prefix) and k[len(prefix):] in known:
            return prefix + known[k[len(prefix):]][0]
    for prefix in LANGS.values():   # a file the game only has per language, in d.arc
        if prefix + k in known:
            return known[prefix + k][0][len(prefix):]
    return None


# ----------------------------------------------------------------------------- commands

def cmd_extract(args, log=print):
    for p in args.archives:
        arc = Arc(p)
        tag = os.path.splitext(os.path.basename(p))[0]
        root = os.path.join(args.out, tag)
        tot_raw = sum(e.raw for e in arc.entries)
        comp = sum(e.compressed for e in arc.entries)
        log(f"{p}: v{arc.version}, {len(arc.entries)} files, {fmt_mib(tot_raw)}, {comp} LZ4")
        os.makedirs(root, exist_ok=True)
        manifest = {"tool": "aczarc", "archive": os.path.basename(p), "version": arc.version, "entries": []}
        errors = 0
        for i, e in enumerate(arc.entries):
            dst = safe_join(root, e.text)
            manifest["entries"].append({"name": e.text, "path": os.path.relpath(dst, root).replace(os.sep, "/"),
                                        "compressed": e.compressed})
            if args.manifest_only:
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            try:
                if e.compressed:
                    with open(dst, "wb") as fo:
                        fo.write(arc.extract(e))
                else:
                    with open(dst, "wb") as fo:
                        for chunk in arc.iter_stored(e):
                            fo.write(chunk)
            except Exception as ex:
                errors += 1
                log(f"  ERROR {e.text}: {ex}")
                continue
            if (i + 1) % 500 == 0:
                log(f"  {i + 1}/{len(arc.entries)}")
        with open(os.path.join(root, MANIFEST), "w", encoding="utf-8") as fm:
            json.dump(manifest, fm, indent=1)
        arc.close()
        if args.manifest_only:
            log(f"  manifest written to {os.path.join(root, MANIFEST)}")
        else:
            log(f"  done: {len(arc.entries) - errors} extracted, {errors} errors -> {root}")


def cmd_list(args, log=print):
    for p in args.archives:
        arc = Arc(p)
        log(f"{p}: v{arc.version}, {len(arc.entries)} files" + (" (unencrypted)" if arc.A == arc.B == 0 else ""))
        log(f"  {'offset':>10} {'size':>10} {'stored':>10}  name")
        for e in arc.entries:
            if args.filter and args.filter.lower() not in e.text.lower():
                continue
            log(f"  {e.off:10d} {e.raw:10d} {e.stored:10d}{'*' if e.compressed else ' '} {e.text}")
        arc.close()


def cmd_verify(args, log=print):
    bad = 0
    for p in args.archives:
        arc = Arc(p)
        ok, got, want = arc.verify()
        msg = f"{p}: {len(arc.entries)} files, checksum {'OK' if ok else f'MISMATCH ({got:08x} != {want:08x})'}"
        if args.deep:
            errs = 0
            for e in arc.entries:
                try:
                    if e.compressed:
                        arc.extract(e)
                    elif e.off + e.stored > arc.size - 20:
                        raise ArcError("data runs past the end of the archive")
                except Exception as ex:
                    errs += 1
                    log(f"  ERROR {e.text}: {ex}")
            msg += f", {len(arc.entries) - errs}/{len(arc.entries)} entries readable"
            ok = ok and not errs
        bad += not ok
        log(msg)
        arc.close()
    return 1 if bad else 0


def cmd_pack(args, log=print):
    files = collect_files(args.folder)
    names, order, hints = {}, {}, {}
    mpath = os.path.join(args.folder, MANIFEST)
    source = "folder paths"
    if args.base:
        base = Arc(args.base)
        for i, e in enumerate(base.entries):
            k = normkey(e.text)
            if k not in names:
                names[k], order[k], hints[k] = e.text, i, e.compressed
        if args.version is None:
            args.version = base.version
        base.close()
        source = f"names from {args.base}"
    elif os.path.exists(mpath) and not args.ignore_manifest:
        with open(mpath, encoding="utf-8") as fm:
            man = json.load(fm)
        for i, ent in enumerate(man["entries"]):
            k = normkey(ent["path"])
            if k not in names:
                names[k], order[k], hints[k] = ent["name"], i, ent.get("compressed")
        if args.version is None:
            args.version = man.get("version", 1)
        source = f"names from {MANIFEST}"
    if args.version is None:
        args.version = 1
    items, extras = [], []
    for rel, full in files:
        k = normkey(rel)
        if k in names:
            items.append((order[k], names[k].encode("latin-1"), FileSource(full, hints[k])))
        else:
            extras.append(((args.prefix or "") + rel, full))
    items.sort(key=lambda t: t[0])
    items = [(n, s) for _, n, s in items] + [(n.encode("utf-8"), FileSource(f)) for n, f in extras]
    items += [(n.encode("utf-8"), FileSource(f)) for n, f in parse_adds(args.add)]
    if not items:
        raise ArcError(f"{args.folder} has no files to pack")
    missing = len(names) - sum(1 for rel, _ in files if normkey(rel) in names) if names else 0
    log(f"packing {len(items)} files from {args.folder} ({source}), version {args.version}, compression {args.compress}")
    if extras:
        log(f"  {len(extras)} files not in the original index were added under their folder paths")
    if missing:
        log(f"  {missing} files listed in the original index are not in the folder and were left out")
    if args.dry_run:
        for n, s in items[:60]:
            log(f"    {n.decode('latin-1')}  <-  {s.label()}")
        if len(items) > 60:
            log(f"    ... {len(items) - 60} more")
        return
    run_write(args.output, items, args, log=log)


def cmd_patch(args, log=print):
    base = Arc(args.base)
    mods = []
    if args.folder:
        for rel, full in collect_files(args.folder):
            mods.append((((args.prefix or "") + rel).encode("utf-8"), FileSource(full)))
    mods += [(n.encode("utf-8"), FileSource(f)) for n, f in parse_adds(args.add)]
    if not mods:
        raise ArcError("nothing to patch: give a mod folder and/or --add LOCAL=NAME")
    if args.version is None:
        args.version = base.version
    items, replaced, added = plan_patch(base, mods, log)
    log(f"patching {args.base} -> {args.output}")
    report_plan(replaced, added, log)
    if added:
        log("  note: the game only loads an added file if something asks for that exact name")
    if args.dry_run:
        base.close()
        return
    run_write(args.output, items, args, open_arcs=[base], log=log)


def cmd_override(args, log=print):
    base = Arc(args.base)
    game_dir = args.game_dir or os.path.dirname(os.path.abspath(args.base))
    langs = list(LANGS) if args.lang == "all" else [x.strip().lower() for x in args.lang.split(",")]
    for l in langs:
        if l not in LANGS:
            raise ArcError(f"unknown language {l!r}; use all or a list of {', '.join(LANGS)}")
    known = known_game_names(game_dir, skip_path=args.base)
    base_keys = {normkey(e.text) for e in base.entries}
    sources = []
    if args.folder:
        sources += collect_files(args.folder)
    sources += parse_adds(args.add)
    if not sources:
        raise ArcError("nothing to override: give a mod folder and/or --add LOCAL=NAME")
    mods, new_names, direct = [], [], 0
    for rel, full in sources:
        src = FileSource(full)
        k = normkey(rel)
        if k in base_keys:                       # a file that already lives in d.arc (lang/...)
            mods.append((rel.encode("utf-8"), src))
            direct += 1
            continue
        if k in known:
            exact = known[k][0]                  # keep the game's exact spelling
        else:
            exact = rel
            new_names.append(rel)
        for l in langs:
            mods.append(((LANGS[l] + exact).encode("latin-1" if k in known else "utf-8"), src))
    if args.version is None:
        args.version = base.version
    items, replaced, added = plan_patch(base, mods, log)
    log(f"building override archive from {args.base} -> {args.output}, languages: {', '.join(langs)}")
    report_plan(replaced, added, log, extra=f" (one copy of each file's data is shared by all languages)")
    if new_names:
        log(f"  warning: {len(new_names)} files do not exist in the game's archives, so nothing will load them:")
        for n in new_names[:20]:
            log(f"    {n}")
    if args.dry_run:
        base.close()
        return
    run_write(args.output, items, args, open_arcs=[base], log=log)
    if os.path.basename(args.output).lower() != "d.arc":
        log("  to use it: back up the game's d.arc, then copy this file over it as d.arc")


def cmd_mod(args, log=print):
    game_dir = args.game_dir or default_game_dir()
    sources = collect_files(args.folder) if args.folder else []
    sources += parse_adds(args.add)
    if not sources:
        raise ArcError("nothing to pack: give a mod folder and/or --add LOCAL=NAME")
    out = args.output or os.path.join(
        game_dir, "mods", os.path.basename(os.path.normpath(args.folder or sources[0][1])) + ".arc")
    originals = {os.path.normcase(os.path.abspath(os.path.join(game_dir, n))) for n in ORIGINAL_ARCHIVES}
    if os.path.normcase(os.path.abspath(out)) in originals:
        raise ArcError(f"{out} is one of the game's own archives; mod archives go in the mods folder")
    known = known_game_names(game_dir, names=sorted(ORIGINAL_ARCHIVES))
    chosen, unknown, renamed = {}, [], 0
    for rel, full in sources:
        exact = match_game_name(rel, known)
        if exact is None:
            exact = posixpath.normpath(rel.replace("\\", "/").lstrip("/"))
            unknown.append(exact)
            name = exact.encode("utf-8")
        else:
            renamed += exact != rel.replace("\\", "/")
            name = exact.encode("latin-1")
        chosen[normkey(exact)] = (name, FileSource(full))   # a later --add replaces a folder file
    items = list(chosen.values())
    if args.version is None:
        args.version = 1
    log(f"building mod archive {out} from {len(items)} files")
    log(f"  {len(items) - len(unknown)} replace game files"
        + (f" ({renamed} renamed to the game's spelling)" if renamed else ""))
    if unknown:
        log(f"  warning: {len(unknown)} files are not in the game's archives, so the game won't ask for them:")
        for n in unknown[:20]:
            log(f"    {n}")
        if len(unknown) > 20:
            log(f"    ... {len(unknown) - 20} more")
    if args.dry_run:
        for n, s in items[:60]:
            log(f"    {n.decode('latin-1')}  <-  {s.label()}")
        if len(items) > 60:
            log(f"    ... {len(items) - 60} more")
        return
    run_write(out, items, args, log=log)
    log("  the mod loader (version.dll) mounts mods/*.arc in alphabetical order; later names win")


# ----------------------------------------------------------------------------- CLI

EPILOG = """examples
  extract everything:          python aczarc.py extract a.arc b.arc c.arc d.arc e.arc -o extracted
  list one archive:            python aczarc.py list a.arc --filter plmodel/zoe
  check an archive:            python aczarc.py verify a.arc --deep
  rebuild a folder:            python aczarc.py pack extracted/a -o a_new.arc
  replace files in a copy:     python aczarc.py patch a.arc mymod -o a_new.arc
  add one file under a name:   python aczarc.py patch e.arc --add test.png=launcher/test.png -o e_new.arc
  small mod over a.arc/b/c:    python aczarc.py override mymod --base d.arc -o d_mod.arc
  mod for the mod loader:      python aczarc.py mod mymod          (writes mods/mymod.arc)

A mod folder mirrors the game's paths, e.g. mymod/data/plmodel/zoefk.mpl; files under
mymod/lang/1us/... only apply when the game runs in that language.
With the mod loader (version.dll) installed, the game mounts every mods/*.arc after its own
archives, so the game's archives never need to be modified.
Output archives are checked with the game's own checksum before they are kept.
Replacing a game archive needs --force; the first time, the original is kept as <name>.bak.
"""


def add_write_options(p, version_default=None, output_help=None):
    p.add_argument("-o", "--output", required=output_help is None, help=output_help or "archive to write")
    p.add_argument("--version", type=int, choices=(0, 1), default=version_default,
                   help="index version: 1 allows LZ4 (default: same as the base/original)")
    p.add_argument("--compress", choices=("auto", "all", "none", "original"), default="auto",
                   help="LZ4 for new files: auto skips .webm/.pac/.png and files over 64 MiB")
    p.add_argument("--hc", action="store_true", help="use LZ4 high compression (slower, smaller)")
    p.add_argument("--plain", action="store_true",
                   help="write the body unencrypted (the game accepts it; easy to inspect in a hex editor)")
    p.add_argument("--add", action="append", metavar="LOCAL=NAME",
                   help="add or replace one file under an explicit archive name (repeatable)")
    p.add_argument("--dry-run", action="store_true", help="show what would be written and stop")
    p.add_argument("--force", action="store_true", help="allow replacing an existing output file")


def build_parser():
    ap = argparse.ArgumentParser(prog="aczarc.py", description="Ace Combat Zero (PC) .arc tool",
                                 epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("extract", help="unpack archives")
    p.add_argument("archives", nargs="+")
    p.add_argument("-o", "--out", default="extracted", help="output folder (default: extracted)")
    p.add_argument("--manifest-only", action="store_true",
                   help="only write the manifest into an existing extraction")
    p.set_defaults(func=cmd_extract)

    p = sub.add_parser("list", help="print an archive's index")
    p.add_argument("archives", nargs="+")
    p.add_argument("--filter", help="only show names containing this text")
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("verify", help="run the game's integrity check")
    p.add_argument("archives", nargs="+")
    p.add_argument("--deep", action="store_true", help="also decompress every entry")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("pack", help="build an archive from a folder")
    p.add_argument("folder")
    p.add_argument("--base", help="take names and order from this archive (for folders without a manifest)")
    p.add_argument("--prefix", default="", help="prefix for files that are not in the manifest/base")
    p.add_argument("--ignore-manifest", action="store_true", help="name files by folder path only")
    add_write_options(p)
    p.set_defaults(func=cmd_pack)

    p = sub.add_parser("patch", help="copy an archive with files replaced or added")
    p.add_argument("base", help="archive to start from")
    p.add_argument("folder", nargs="?", help="mod folder that mirrors archive paths")
    p.add_argument("--prefix", default="", help="prefix added to every mod file name before matching")
    add_write_options(p)
    p.set_defaults(func=cmd_patch)

    p = sub.add_parser("override", help="build a d.arc that overrides files in a/b/c.arc")
    p.add_argument("folder", nargs="?", help="mod folder that mirrors game paths (data/...)")
    p.add_argument("--base", default="d.arc", help="the game's d.arc (default: ./d.arc)")
    p.add_argument("--lang", default="all",
                   help="languages to cover: all, or a list like us,jp (codes: " + ", ".join(LANGS) + ")")
    p.add_argument("--game-dir", help="folder with a.arc/b.arc/c.arc (default: next to --base)")
    add_write_options(p)
    p.set_defaults(func=cmd_override)

    p = sub.add_parser("mod", help="build an archive for the mod loader's mods folder")
    p.add_argument("folder", nargs="?", help="mod folder that mirrors game paths (data/...)")
    p.add_argument("--game-dir", help="folder with the game's archives (default: here or next to this script)")
    add_write_options(p, output_help="archive to write (default: <game>/mods/<folder name>.arc)")
    p.set_defaults(func=cmd_mod)
    return ap


def legacy_args(argv):
    """Older usage: aczarc.py [-l] [--verify] [-o DIR] archives..."""
    lp = argparse.ArgumentParser(prog="aczarc.py")
    lp.add_argument("archives", nargs="+")
    lp.add_argument("-o", "--out", default="extracted")
    lp.add_argument("-l", "--list", action="store_true")
    lp.add_argument("--verify", action="store_true")
    a = lp.parse_args(argv)
    if a.verify:
        cmd_verify(argparse.Namespace(archives=a.archives, deep=False))
    if a.list:
        return cmd_list(argparse.Namespace(archives=a.archives, filter=None))
    if not a.verify:
        return cmd_extract(argparse.Namespace(archives=a.archives, out=a.out, manifest_only=False))


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    commands = {"extract", "list", "verify", "pack", "patch", "override", "mod"}
    try:
        if argv and argv[0] not in commands and argv[0] not in ("-h", "--help"):
            return legacy_args(argv) or 0
        args = build_parser().parse_args(argv)
        return args.func(args) or 0
    except (ArcError, OSError, lz4.block.LZ4BlockError) as ex:
        print(f"error: {ex}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
