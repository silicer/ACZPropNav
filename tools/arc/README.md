# `tools/arc/` — ACZ `.arc` tooling

Read-only tooling for the ACE COMBAT ZERO (Steam) archives. **Nothing here ever writes to
`ACZero/`**; `extract` only writes under the `--out` directory you pass.

| file | purpose | deps |
| --- | --- | --- |
| `arc_tool.py` | list / json / extract / find / info on any `.arc` | **none** (pure Python 3.8+; `numpy` used only to speed up the XOR if present) |
| `wpb_dump.py` | decode the 96-byte weapon records (`*.wpb`), `--compare` two records, `--scan` a blob | none |
| `upstream/aczarc.py` | vendored community script (unpack **and repack**) — reference/fallback only | `numpy`, `lz4` |
| `dl.py` | one-off recon helper used to fetch the community tool through a blocked-TLS sandbox (Python's own OpenSSL); not used by the extractor | none |

Full format documentation: [`../../docs/ARC.md`](../../docs/ARC.md).
Vanilla parameter values and field semantics: [`../../docs/VANILLA.md`](../../docs/VANILLA.md).

## Quick start

```sh
# from the workspace root (the folder containing ACZero/ and ACZPropNav/)
PY=python

$PY ACZPropNav/tools/arc/arc_tool.py info    ACZero/a.arc
$PY ACZPropNav/tools/arc/arc_tool.py list    ACZero/a.arc --grep '\.wpb$' --long
$PY ACZPropNav/tools/arc/arc_tool.py find    ACZero/a.arc --name mis

# extract just the aircraft/weapon parameter directory (67 MiB)
$PY ACZPropNav/tools/arc/arc_tool.py extract ACZero/a.arc --grep '^data/mis/' --out ACZPropNav/work/a

# decode a weapon record, and diff two of them
$PY ACZPropNav/tools/arc/wpb_dump.py ACZPropNav/work/a/data/data/mis/mpl/mis.wpb
$PY ACZPropNav/tools/arc/wpb_dump.py --compare <a.wpb> <b.wpb>
```

`arc_tool.py` exit codes: `0` ok, `2` bad archive / format error, `3` checksum mismatch,
`4` some entries failed to decompress.

## Why not just use `upstream/aczarc.py`?

It is the tool the community tutorial points at, and it is good — but it needs `numpy` +
`lz4`, and `lz4` was not installable in this environment. More importantly its binary
`.pyc`-free script assumes a *trusted, known* archive; `arc_tool.py` was written from the
format up so the layout itself is documented and testable (`info` recomputes the game's own
body checksum, which is an independent correctness check).

Keep `upstream/aczarc.py` for **repacking** if that is ever needed — writing archives is
deliberately out of scope here (the mod must not modify the install).

## Provenance / safety

* `upstream/aczarc.py` was downloaded from the "ACZ Enhanced Gunplay" addon
  (`ACZ_Enhanced_Gunplay_Mod_A6.zip`, MD5 `bd226c83426b8e1c856737aae3a5d25b`) published at
  <https://www.moddb.com/games/ace-combat-zero-the-belkan-war/addons/acz-enhanced-gunplay-mod>.
  It is Python source; it was **read**, not executed (its `lz4` dep was unavailable).
* No untrusted binary from that archive was executed. The `.dll` files in it were not
  touched.
