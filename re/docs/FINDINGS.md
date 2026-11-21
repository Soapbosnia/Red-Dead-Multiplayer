# Findings — RDR.exe 1.0.42.49788435

Verified by **read-only static analysis** on 2026-11-01 with
[`tools/pe_info.py`](../tools/pe_info.py) and `objdump` (mingw64). Nothing in the
game install was modified. Machine-readable output: [findings.json](findings.json).

## Build identity

| Property | Value |
|---|---|
| Path | `D:\Games\Red Dead Redemption\RDR.exe` |
| Size | 39,745,512 bytes |
| SHA-256 | `4D3F7A26CD22B3EAD541F37F278F2D8AD905F0321D21E33E696BE62C475913D7` |
| File version | `1.0.42.49788435` |
| Product | Red Dead Redemption, Rockstar Games |
| Machine | `0x8664` (x64), PE32+ |
| Image base | `0x140000000` |
| Entry point | RVA `0xF4A2D8` |
| SizeOfImage | `0x5A5EC600` ≈ 1.41 GiB |
| PE timestamp | `0x673783F3` (mid-November 2024) |

Sibling files of interest: `PlayRDR.exe` (launcher stub), `RDRMessage.exe`,
`sl.interposer.dll` + `nvngx_dlss*.dll` (Streamline/DLSS), `v1.0.42.blake3`.

## Sections

| Name | Virtual size | RVA | Raw size |
|---|---:|---:|---:|
| `.text` | `0x0104A140` | `0x00001000` | `0x0104A200` |
| `.rdata` | `0x00F0E42C` | `0x0104C000` | `0x00F0E600` |
| `.data` | `0x5853A9F0` | `0x01F5B000` | `0x00538400` |
| `.pdata` | `0x000D6EFC` | `0x5A496000` | `0x000D7000` |
| `_RDATA` | `0x00002208` | `0x5A56D000` | `0x00002400` |
| `CPADinfo` | `0x00000038` | `0x5A570000` | `0x00000200` |
| `.rsrc` | `0x0003276B` | `0x5A571000` | `0x00032800` |
| `.reloc` | `0x00047AB4` | `0x5A5A4000` | `0x00047C00` |
| `.rkstr` | `0x00000457` | `0x5A5EC000` | `0x00000600` |

**Interpretation.**

- Sections are page-aligned with ordinary slack; there is no overlap. An earlier
  concern about `.text`/`.rdata` overlap was an arithmetic error — both parsers
  agree the layout is contiguous.
- `.data` is the anomaly: **1.48 GiB of virtual address space backed by 5.5 MB of
  file data**. The loader zero-fills the rest. Any signature scanner must bound
  itself to `PointerToRawData..PointerToRawData+SizeOfRawData`; scanning the
  virtual range means reading ~1.4 GB of zeros per pass.
- `CPADinfo` (0x38 bytes) and `.rkstr` (0x457 bytes of `CODE`) are non-standard
  sections. `CPADinfo` looks like a copied controller-layout struct; `.rkstr` may
  be a Rockstar signature/integrity blob. Both are worth a look before trusting
  the loader to leave them alone.

## Imports (28 DLLs)

Notable: `d3d12.dll`, `dxgi.dll`, `D3DCOMPILER_47.dll`, `DINPUT8.dll`,
`XINPUT1_4.dll`, `bink2w64.dll`, `sl.interposer.dll`, `steam_api64` (loaded
dynamically — not in this list), `MSVCP140`/`VCRUNTIME140` (MSVC 2015-2022 runtime).

Two practical consequences:

- The game is **D3D12**; there is no D3D11 path to hook.
- `DINPUT8.dll` is imported directly, so a proxy-DLL injection is technically
  available as a fallback if the PE-loading launcher turns out to be blocked.

## Renderer and engine lineage

- D3D12 confirmed by both imports and the `D3D12_AUTO_BREADCRUMB_OP_*` strings.
- Streamline (`sl.interposer.dll`) and DLSS are present — overlay hooks must
  survive the interposer, and should be tested with DLSS both on and off.
- Engine lineage is RDR2-derived. Compile-time strings include
  `t:\rdr2\src\d11-engine\inc\crunch\inc\crn_decomp.h` and `d11-engine`; shader and
  asset paths use `win64gdk/rage_*.fxc`; runtime strings include `[RAGE] DVD Streamer`,
  `[RAGE Audio] - Engine Thread` and `RDR2SAVE0.SAV`.

  This is **not dead data**. `tools/xrefs.py` finds dozens of RIP-relative references
  to `\RDR2SAVE0.SAV` in `.text` (first at RVA `0x20D47B`), inside a function that
  selects between save-file names before opening one, and a code reference to
  `d11-engine` at RVA `0xEB2A06`. The same function branches to
  `RDR2ZOMBIESAVE0.SAV` for Undead Nightmare, so this is RDR2's save manager with
  both save slots, not a coincidentally named file.

  Precise wording: the port **reuses RDR2 engine modules** (renderer, save system,
  resource pipeline) while content, scripts and gameplay remain RDR1's. It is not an
  RDR2 build with RDR1 assets bolted on — it is RDR1 running on RDR2-era engine code.
  For RE that means RDR2's modding corpus is the right reference for *engine
  internals*, and no guide at all for RDR1 gameplay classes.

## RTTI and symbols

- 57 MSVC RTTI type descriptors, all standard library / `d11` / audio, e.g.
  `rage::AudioDecoderHelper_c`, `d11::OpusAudioDecoder_c`, `std::filesystem`.
- **No gameplay classes**: no `CPed`, `CVehicle`, `CWorld`, `CTheScripts`, `scrThread`.
- Raw string probes: `scrThread` ×1, `CControl` ×1, `CAMERA` ×16, `rage::` ×0
  (mangled names only).

**Consequence:** the usual RTTI-vtable shortcut is unavailable. Class discovery
has to come from dynamic analysis: find data, watch what touches it, walk the call
stack, then name the class from behaviour.

## Integrity manifest

`v1.0.42.blake3` (16,940 bytes) is a text manifest of `BLAKE3  <filename>` lines —
one hash per shipped file (`1911.dll`, `bink2w64.dll`, `crashpad_handler.exe`, ...).

Unknowns: what consumes it, and whether it validates at launch. If something does
verify these hashes, replacing a shipped DLL (e.g., a `dinput8.dll` proxy) could be
detected, while *adding* a new DLL next to the exe would not change any listed
hash. This must be tested before committing to an injection strategy.

## What is still unknown

- Whether the game has anti-debug or anti-tamper beyond the manifest above.
- Whether RedHook (the ScriptHook) loads on this exact build — useful as an interop
  layer, and as proof that DLL injection is viable at all.
- Entity/streaming architecture and the per-frame update call site.
- Whether RDR2 signatures transfer verbatim; they should be treated as hypotheses
  to verify per build, never as constants.
