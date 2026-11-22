#!/usr/bin/env python3
"""wpb_dump.py -- decode ACZ weapon-parameter records (`*.wpb`, 96-byte fixed struct).

`data/mis/mpl/*.wpb` are the game's **live** weapon definition records (the game
loads these).  Each shipping file holds exactly ONE record of 96 bytes.

Layout below was derived empirically, not guessed: for each `*.wpt` schema file a
matching `*.wpb` exists, and 14 of the 16 named fields land on the offsets shown
with EXACTLY the values printed by the `.wpt` (see docs/VANILLA.md for the field
by field evidence table).

    off   type   name            note
    ----  -----  --------------  -------------------------------------------------
    0x00  u32    data_off        always 0x10 (=16): offset of the first field
    0x04  u32    reserved        always 0
    0x08  u32    reserved        always 0
    0x0C  u32    reserved        always 0
    0x10  f32    altMin          minimum altitude (0 = unconstrained)
    0x14  f32    altMax          maximum altitude (0 = unconstrained)
    0x18  f32    radius          proximity / blast trigger radius
    0x1C  f32    explPers        explosion persistence (1.0)
    0x20  f32    scope           seeker acquisition / lock-on distance  [KEY]
    0x24  f32    range           max flight distance                    [KEY]
    0x28  f32    hitRadius       hit sphere radius (1.0)
    0x2C  f32    timeStart       guidance start delay                   [KEY]
    0x30  f32    timeWait        guidance/flight phase duration         [KEY]
    0x34  f32    timeStep        phase step duration                    [KEY]
    0x38  f32    timeContinue    post-phase continuation
    0x3C  f32    lead            LEAD / pursuit-gain factor             [KEY]
    0x40  f32    dispersionAng   aim dispersion (radians) for guns
    0x44  f32    accFall         gravity/acceleration fall (9.8)
    0x48  f32    unknown0        always 0 in every shipping record
    0x4C  f32    unknown1        always 0
    0x50  f32    unknown2        always 0
    0x54  u16    speed           initial speed (m/s)
    0x56  u16    unknown3        always 0
    0x58  u8     type            1 = gun, 2 = missile
    0x59  u8     attack          damage
    0x5A  u8     startAng        seeker cone / launch gimbal angle (degrees) [KEY]
    0x5B  u8     unknown4        always 0
    0x5C  u32    pad             always 0

Usage
-----
  python wpb_dump.py <file.wpb> ...          human table
  python wpb_dump.py --json <file.wpb> ...   machine readable
  python wpb_dump.py --compare A.wpb B.wpb   list differing fields between two records
  python wpb_dump.py --scan <binary blob>    find candidate records inside a blob
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys

RECORD_SIZE = 96

FIELDS = [
    (0x00, "u32", "data_off"),
    (0x04, "u32", "reserved0"),
    (0x08, "u32", "reserved1"),
    (0x0C, "u32", "reserved2"),
    (0x10, "f32", "altMin"),
    (0x14, "f32", "altMax"),
    (0x18, "f32", "radius"),
    (0x1C, "f32", "explPers"),
    (0x20, "f32", "scope"),
    (0x24, "f32", "range"),
    (0x28, "f32", "hitRadius"),
    (0x2C, "f32", "timeStart"),
    (0x30, "f32", "timeWait"),
    (0x34, "f32", "timeStep"),
    (0x38, "f32", "timeContinue"),
    (0x3C, "f32", "lead"),
    (0x40, "f32", "dispersionAng"),
    (0x44, "f32", "accFall"),
    (0x48, "f32", "unknown0"),
    (0x4C, "f32", "unknown1"),
    (0x50, "f32", "unknown2"),
    (0x54, "u16", "speed"),
    (0x56, "u16", "unknown3"),
    (0x58, "u8", "type"),
    (0x59, "u8", "attack"),
    (0x5A, "u8", "startAng"),
    (0x5B, "u8", "unknown4"),
    (0x5C, "u32", "pad"),
]

# Fields whose semantics matter for missile guidance / the PropNav mod.
KEY_FIELDS = ("scope", "range", "timeStart", "timeWait", "timeStep", "lead", "speed", "startAng")


def decode(raw: bytes) -> dict:
    if len(raw) < RECORD_SIZE:
        raise ValueError(f"{len(raw)} bytes, need {RECORD_SIZE}")
    rec = {}
    for off, typ, name in FIELDS:
        if typ == "f32":
            rec[name] = struct.unpack_from("<f", raw, off)[0]
        elif typ == "u32":
            rec[name] = struct.unpack_from("<I", raw, off)[0]
        elif typ == "u16":
            rec[name] = struct.unpack_from("<H", raw, off)[0]
        elif typ == "u8":
            rec[name] = raw[off]
        rec.setdefault("_raw", {})[name] = raw[off:off + {"f32": 4, "u32": 4, "u16": 2, "u8": 1}[typ]].hex()
    return rec


def fmt(v):
    if isinstance(v, float):
        return f"{v:g}"
    return str(v)


def show(path, as_json=False):
    raw = open(path, "rb").read()
    if len(raw) % RECORD_SIZE == 0 and len(raw) > RECORD_SIZE:
        recs = [decode(raw[i:i + RECORD_SIZE]) for i in range(0, len(raw), RECORD_SIZE)]
    else:
        recs = [decode(raw)]
    for i, rec in enumerate(recs):
        if as_json:
            out = {k: v for k, v in rec.items() if not k.startswith("_")}
            out["_file"] = os.path.basename(path)
            if len(recs) > 1:
                out["_record"] = i
            print(json.dumps(out))
            continue
        print(f"== {path}" + (f"  [record {i}]" if len(recs) > 1 else "") + " ==")
        for off, typ, name in FIELDS:
            if typ in ("u32",) and name.startswith(("reserved", "pad", "data_off")):
                continue
            mark = "  <<<" if name in KEY_FIELDS else ""
            print(f"  +0x{off:02x} {typ:3} {name:<14} = {fmt(rec[name]):<12}{mark}")
        cmt = sibling_comment(path)
        if cmt:
            print(f"  comment(cp932) = {cmt}")
    return recs


def sibling_comment(wpb_path: str) -> str:
    """The `comment` field exists only in the `.wpt` text twin (cp932), not in the .wpb."""
    wpt = os.path.splitext(wpb_path)[0] + ".wpt"
    if not os.path.exists(wpt):
        return ""
    blob = open(wpt, "rb").read()
    m = re.search(rb'comment\s*:\s*"(.*?)"', blob, re.S)
    if not m:
        return ""
    for enc in ("cp932", "utf-8"):
        try:
            return m.group(1).decode(enc)
        except UnicodeDecodeError:
            continue
    return m.group(1).hex()


def compare(a, b):
    ra, rb = decode(open(a, "rb").read()), decode(open(b, "rb").read())
    print(f"{'field':<14} {'+off':>5} {os.path.basename(a):>14} {os.path.basename(b):>14}   delta")
    print("-" * 74)
    for off, typ, name in FIELDS:
        if ra[name] != rb[name]:
            try:
                d = f"{rb[name] - ra[name]:+g}"
            except TypeError:
                d = "-"
            print(f"{name:<14} +0x{off:02x} {fmt(ra[name]):>14} {fmt(rb[name]):>14}   {d}")
    return 0


def scan(blob_path):
    """Look for 96-byte-aligned candidates inside an arbitrary blob (e.g. a .bin)."""
    data = open(blob_path, "rb").read()
    hits = 0
    for off in range(0, len(data) - RECORD_SIZE + 1, 4):
        if struct.unpack_from("<I", data, off)[0] != 0x10:
            continue
        if data[off + 4:off + 16] != b"\0" * 12:
            continue
        rec = decode(data[off:off + RECORD_SIZE])
        if rec["type"] in (1, 2) and 0 < rec["speed"] < 20000 and rec["hitRadius"] in (0.0, 1.0, 2.0):
            print(f"{blob_path} +0x{off:x}: type={rec['type']} speed={rec['speed']} "
                  f"scope={rec['scope']:g} range={rec['range']:g} lead={rec['lead']:g} "
                  f"startAng={rec['startAng']}")
            hits += 1
    print(f"--- {hits} candidate record(s)")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("Usage")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--compare", nargs=2, metavar=("A", "B"))
    ap.add_argument("--scan", metavar="BLOB")
    a = ap.parse_args(argv)
    if a.compare:
        return compare(*a.compare)
    if a.scan:
        return scan(a.scan)
    if not a.files:
        ap.error("need at least one .wpb file")
    for p in a.files:
        show(p, a.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
