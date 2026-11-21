# RPF6 (RDR1 PC) — container notes

State: **TOC decryption solved.** Payload compression/codec still open.

## TOC decryption (solved)

The PC port applies the AES-256 block cipher **16 times per 16-byte block** to the
TOC — not plain AES-256-ECB. With the key below and 16 iterations, the table is
valid: root directory at entry 0, 15 directories, 207 files for `flash.rpf`.

```
key (32 bytes, .data va 0x1422A4700):
B7 62 DF B6 E2 B2 C6 DE AF 72 2A 32 D2 FB 6F 0C
98 A3 21 74 62 C9 C4 ED AD AA 2E D0 DD F9 2F 10
```

Header: `"RPF6" | entryCount u32 | debugOffset u32 | tag u32` (big-endian).
TOC at `0x10`, `entryCount*20` bytes padded to 16, fields big-endian, no byte swap
after decryption.

## What the archive contains

`sagHudData` manifest (entry 7, the only zstd-framed file) lists every UI movie:

```
/networking/networking        <- legacy RDR1 multiplayer menu, still shipped
/playerlist/playerlist
/mp_HUD/mp_HUD
/net_HUD/net_HUD
/pause_main/pause_main
/startscreen/startscreen      <- title screen with Play / Settings / Quit
/loading/loading  /credits/credits  /satchel/satchel  /wares/wares ...
```

The original multiplayer UI is still in the PC build. That is the natural host for
our Online menu instead of authoring one from scratch.

## Open: payload codec — SOLVED

Earlier note said "opaque, probably XCompress LZX". That was wrong, and the error
was mine: resource entries were being read at the wrong offset.

`flag1` bit 31 marks a resource entry, and its offset is masked differently:

```
file_offset = (flag1 & 0x80000000) ? (offset & 0x7FFFFF00) * 8
                                   : (offset & 0x7FFFFFFF) * 8
```

With that fix every payload begins `52 53 43 05` (`RSC\x05`) + 8 flag bytes, then a
**zstd frame** (`28 B5 2F FD`) at +12. `tools/rpf6.py` now implements `payload()`:
RSC5 container -> zstd -> usable bytes.

Coverage after the fix (sampled 60 entries/archive, `tools/rpf_census.py`):

```
24 archives, 46,351 entries - 100% of sampled payloads decompress
fonts 59/59, textures 40/40, shaders 146/146, flash 207/207
```

## Milestone: full asset extraction + UI map

`flash.rpf` extracts completely (207/207). Known movie payloads, from
`docs/ui-assets.txt` (109 of 207 carry a recognisable name):

| payload | size | contains |
|---|---|---|
| `0007_7C5C72CA` | 9,376 | sagHudData manifest (all movie names) |
| `0023_4D8DB2A4` | 10.9 MB | `startscreen` (title menu) |
| `0021_2B789D2A` | 3.2 MB | `pause_main` |
| `0015/0028_...` | 479 KB | `LegalsCompleted` movie |
| `0022_36890B4D` | 155 KB | `multiplayer` |
| several | 0.5–11 MB | `hud`, `networking`, `playerlist` |

Payloads are Scaleform resource data (textures referenced by name, e.g.
`textures/_brs_startscreen_0110.dds`), not plain SWF — so editing a menu means
parsing the GFx resource, not rewriting an SWF.



## Ghidra environment (for the next session)

Ghidra 12.2 requires **JDK 25** (`application.java.min=25`). Its launcher writes
under `%APPDATA%`/`%TEMP%`, which the harness denies to child JVMs, so run it with:

```
APPDATA=<workspace>\ghenv\Roaming
LOCALAPPDATA=<workspace>\ghenv\Local
TEMP/TMP=<workspace>\ghenv\Temp
JAVA_HOME=<workspace>\_tools\jdk25\jdk-25.0.5+8
```

Project: `_tools/ghproj` (RDR.exe, full analysis complete). Decompile script:
`_tools/ghidra_scripts/DecompileAt.java`.


## Next

1. Identify the payload codec: inspect `FlagInfo` bits, or hook the game's
   decompressor (cold path, only on asset load) and dump decoded buffers.
2. With `.gfx` extracted: edit `startscreen` (Play -> Online) and `networking`
   (nickname/settings), or load `networking` at runtime for the Online entry.
3. Runtime alternative: since `networking` ships, driving the game to load it may
   be enough for a first Online screen, before any asset edit.

