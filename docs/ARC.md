# `ACZero/*.arc` — container format

**Status:** fully decoded and verified against every shipped archive.
**Tool:** [`../tools/arc/arc_tool.py`](../tools/arc/arc_tool.py) — read-only, dependency-free
(no `lz4` module needed: it contains a pure-Python LZ4 block decoder).
**Verified by:** SHA-identical header/index round-trip on all 5 shipping archives plus 2
community-mod archives; the game's own body checksum reproduced bit-exactly for every one.

---

## 1. Why this looked unbreakable

The archives are high-entropy from byte 0 to the last byte and contain **no plaintext
index**, which is why a naive `strings`/entropy survey finds nothing. Two independent
reasons:

1. **The whole body is XOR-obfuscated** with a 32-bit dword keystream — including the
   index, which lives at the *front* of the body, not the end.
2. Data blocks are **LZ4-compressed** per entry (`acz.exe` imports `LZ4_initStream`,
   confirming LZ4), so even after de-XOR'ing, most bytes look random.

The first dword of every file is `C3 58 47 45` = `0x454758C3`, which reads as the ASCII
letters `XGE` with a leading `0xC3` — easy to dismiss as noise, but it is the outer magic.

## 2. Layout

All offsets below are **file offsets**. "Stream position" means *file offset − 12*; the
XOR keystream is indexed by stream position, so the index at file offset 12 starts at
stream position 0 and de-obfuscates cleanly.

```
+0x0000  u32   outer_magic   0x454758C3
+0x0004  u32   k1            key material
+0x0008  u32   k2            key material
+0x000C  ...   BODY          XOR'ed with the keystream, starts with the index
          ...
size-8  u32   k3            key material
size-4  u32   enc_checksum  body checksum, itself XOR'ed
```

### 2.1 Keystream

```
ks[i] = (A + B * (stream_pos/4 + i)) mod 2^32          applied little-endian per dword
A = k1 ^ k3 ^ 0x5DF38C35
B = k2 ^ k3 ^ 0x4CE49F4D
```

`A == 0 and B == 0` means an identity keystream (body not obfuscated); it never happens in
the shipping archives. Because `A` and `B` derive from *both* the header and the trailer,
a single flipped byte in either place "decrypts" to pure garbage — useful as a
self-check, and the reason corrupting an archive breaks it completely.

### 2.2 Index (at stream position 0)

```
u32  inner_magic   0x414758E1
u32  count         number of entries
u32  names_len     byte length of the name table
u32  version       0 or 1
[names_len bytes]  `count` NUL-terminated names, concatenated
[count * esz]      entry table, esz = 12 if version else 8
```

Entry table row:

| version | fields |
| --- | --- |
| 0 | `u32 off`, `u32 raw` — stored == raw, i.e. **never compressed** |
| 1 | `u32 off`, `u32 raw`, `u32 stored` — compressed iff `raw != stored` |

`off` is a **stream position**, not a file offset. Entry data starts immediately after the
index (`off` of the first entry == index length). Entries are stored contiguously with no
alignment padding.

### 2.3 Compression

Per entry, independent **LZ4 block** (no frame header, no per-block size prefix — the size
is already in the index as `raw`). If `stored < raw` the bytes are an LZ4 block that
decompresses to exactly `raw` bytes; otherwise the bytes are stored verbatim. Names,
`data/../stream/pack/*.PAC` and `.webm` payloads are stored (v0 archives are entirely
uncompressed).

### 2.4 Checksum

```
body_len = size - 12 - 8
checksum = sum over dwords d of (ror32(d, 7))   mod 2^32, over body bytes [12, size-8)
expected = deobfuscate(enc_checksum, stream_pos = body_len)
```

`arc_tool.py info` recomputes it; it matches for all 7 archives tested.

## 3. Inventory of the shipping archives

| file | size | idx ver | files | index bytes | LZ4 entries | raw total | content |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `a.arc` | 502,706,872 | 1 | 4816 | 191,999 | 4075 | 862,430,089 | all gameplay data: aircraft, maps, weapons |
| `b.arc` | 1,736,730,816 | 0 | 72 | 2,652 | 0 | 1,736,728,142 | `movie/work/**/*.webm` briefing movies |
| `c.arc` | 1,185,351,144 | 0 | 6 | 250 | 0 | 1,185,350,872 | `stream/pack/{BGM,RADIOJP,RADIOUSA}.PAC` |
| `d.arc` | 36,458,028 | 1 | 185 | 8,258 | 137 | 61,489,249 | UI, menu, per-language text/manual art |
| `e.arc` | 1,003,856 | 0 | 3 | 88 | 0 | 1,003,745 | 3 launcher PNGs only |

`e.arc` contains exactly `launcher/{eu,jp,us}.png` — it is *not* a data archive.

### 3.1 The `.bak` files are the ACE COMBAT 8 copy

Verified by MD5:

| our file | MD5 | identical to |
| --- | --- | --- |
| `ACZero/a.arc.bak` | `5c44f33c442af6707326dd0f429f3da6` | `ACE COMBAT 8/ACZero/a.arc` |
| `ACZero/d.arc.bak` | `a82279f97426d3aa9f00c5c36e315321` | `ACE COMBAT 8/ACZero/d.arc` |
| `ACZero/b.arc` | `dbdb216407e8d4f10d33085934b252e0` | `ACE COMBAT 8/ACZero/b.arc` |
| `ACZero/c.arc` | `7509bdc2bebd10754c424d60bdf25eb1` | `ACE COMBAT 8/ACZero/c.arc` |
| `ACZero/e.arc` | `58ffa2cd0cf5f122754afabc91095238` | `ACE COMBAT 8/ACZero/e.arc` |

So `a.arc` and `d.arc` were **replaced** by the 2026-10-07 Steam update (a: 479,214,608 →
502,706,872; d: 25,167,040 → 36,458,028) and the old ones kept as `.bak`. b, c and e are
bit-identical between the two installs. This is a useful A/B pair for future diffs: the
`.bak` is a valid, complete archive in the same format.

## 4. Usage

```sh
PY=python                       # bundled python is fine; no pip installs required
T=ACZPropNav/tools/arc/arc_tool.py

python $T info    ACZero/e.arc                 # header, keys, checksum, totals
python $T list    ACZero/a.arc --long          # whole index (4816 rows)
python $T find    ACZero/a.arc --name mis.wpb  # substring search on names
python $T json    ACZero/a.arc --out a.index.json
python $T extract ACZero/a.arc --grep '^data/mis/'  --out work/a
python $T extract ACZero/a.arc --grep '\.wpb$'      --out work/w
```

`extract` writes entries under `--out` preserving the in-archive path, refuses to
overwrite unless `--force`, and supports `--max-file N` (skip big entries) and
`--limit-n N`. Nothing ever writes to the archive itself; the tool opens it `"rb"` only.

Decoding a weapon record:

```sh
python ACZPropNav/tools/arc/wpb_dump.py work/a/data/data/mis/mpl/mis.wpb
python ACZPropNav/tools/arc/wpb_dump.py --compare A.wpb B.wpb
python ACZPropNav/tools/arc/wpb_dump.py --scan some_blob.bin
```

## 5. Known limitations / WIP

* **`extract` is single-threaded pure Python for the LZ4 decode** (~10–20 MiB/s). Fine for
  `data/mis/**` (67 MiB) but extracting all of `a.arc` (862 MiB raw) takes minutes.
* **No repacking.** This tool is read-only by design; repacking is out of scope for the
  mod (we never modify the game install). The community script
  [`upstream/aczarc.py`](../tools/arc/upstream/aczarc.py) vendored here can repack if ever
  needed; it requires `numpy` + `lz4` and needs a bump of the trailer/checksum bookkeeping
  which it implements.
* **Index is assumed at stream position 0.** True for all shipping + community archives
  seen. Nothing verifies an index offset field, because none exists.
* **Names are latin-1**, stored with `/` separators; the archive grammar has no directories
  — `data/../stream/...` appears literally in `c.arc` names and is *not* normalised by the
  game. Path traversal is guarded in `safe_join`.
* `verify()` reads the whole body, so it costs one full sequential pass (fast on SSD,
  ~1.6 GiB for `b.arc`).

## 6. Appendix — `a.arc` data tree (proves the layout)

4816 entries, 822.5 MiB raw, grouped by the first two path components:

| directory | files | raw MiB |
| --- | ---: | ---: |
| `data/map` | 1145 | 270.6 |
| `data/plmodel` | 208 | 164.3 |
| `data/selaircraft` | 263 | 126.1 |
| `data/caption` | 860 | 82.1 |
| `data/mis` | 940 | 67.3 |
| `data/radio` | 275 | 32.5 |
| `data/select` | 37 | 31.7 |
| `data/sound` | 157 | 11.0 |
| `data/lodaircraft` | 193 | 10.8 |
| `data/effect` | 192 | 8.7 |
| `data/mistitle` | 19 | 4.2 |
| `extratex/manual` | 2 | 3.7 |
| `data/pausetxt` | 38 | 3.6 |
| `data/dmap` | 71 | 2.0 |
| `data/selmap` | 22 | 1.2 |
| `data/supply` | 36 | 1.1 |
| `data/hud` | 75 | 0.5 |
| `data` (root) | 2 | 0.4 |
| `shader` | 59 | 0.2 |
| `data/debrief` | 6 | 0.2 |
| `extratex/hudfont` | 1 | 0.1 |
| `data/lodweapon` | 5 | 0.1 |
| `data/mapset` | 78 | 0.1 |
| `data/brdata` | 33 | 0.1 |
| `data/spheredata` | 39 | <0.1 |
| `data/testdata` | 1 | <0.1 |
| `extratex/texture` | 8 | <0.1 |
| `data/planim` | 38 | <0.1 |
| `data/object` | 1 | <0.1 |
| `data/decision` | 5 | <0.1 |
| `data/cam` | 7 | <0.1 |
| **total** | **4816** | **822.5** |

Extension census inside `data/mis/mpl/` (the aircraft/weapon parameter directory, 508
files in `a.arc`) reveals the game's convention **`Xxx` binary + `Xxx` text twin**:

| ext | count | meaning |
| --- | ---: | --- |
| `.bin` | 97 | misc tables (`*ex.bin`, `*wpic.bin`) |
| `.pmb` | 74 | aircraft flight model, binary (400 B) |
| `.pmt` | 62 | **text twin of `.pmb`** (`pitch/roll/yaw` `rotRate`, speeds) |
| `.pvb` | 66 | cockpit/camera model, binary (160 B) |
| `.pvt` | 58 | **text twin of `.pvb`** (`Cockpit.*` angles/offsets) |
| `.wxb` | 58 | weapon placement, binary (48 B) |
| `.pxb` | 37 | placement, binary (32 B) |
| `.ppb` | 37 | aircraft params, binary (48 B) |
| `.wpb` | 10 | **weapon/ordnance records, binary (96 B)** — see `VANILLA.md` |
| `.wpt` | 8 | **text twin of `.wpb`** — the schema *and* the values, plain text |
| `.txt` | 1 | `amparam.txt` |

The `.wpt`/`.pmt`/`.pvt` text twins are compiled-out developer source: they give field
**names, types and order** for the matching binary file, which is exactly what makes the
`.wpb` layout provable rather than guessed. Note the twins are *not* byte-exact mirrors of
the binary — see `VANILLA.md` §5.
