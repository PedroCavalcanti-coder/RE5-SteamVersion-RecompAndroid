# SPEED2.EXE binary report

| Field | Value |
|---|---|
| Path (local) | `Need for Speed Underground 2/SPEED2.EXE` |
| Size | 4,800,512 bytes |
| SHA-256 | `f9dd86c054878ce6276beb07c1fd61874f7a1e4bf1f241b084c65b73e24168a7` |
| MD5 | `6445adbf4e8d64be51e96a69bba168a4` |
| File mtime | 2005-03-08 |
| Machine | i386, PE32, GUI subsystem 4.0 |
| ImageBase | 0x00400000 (FileHeader has RELOCS_STRIPPED set) |
| Entry point | 0x0075BCC7 (RVA 0x35BCC7) — MSVC `WinMainCRTStartup` shape (`push 0x18; push 0x7D3998; call __SEH_prolog`) |
| PE timestamp | 0x214D4C48 (1987-09-15: not a real build time); CheckSum field equals the same value |
| Linker field | 7.00 |
| Rich header | VC++ 7.1 toolchain (builds 3077 / 4035) plus objects from older toolchains (builds 8444–9466, static libraries) |
| TLS | none |
| Delay imports | none |
| Resources | 1 icon, 1 group icon, 23 RT_RCDATA |
| Version resource | none |
| Embedded build date | `Feb  9 2005` (string next to the online-protocol `VERS` field) |

## Sections

| Name | VA | VirtualSize | Raw | Flags | Notes |
|---|---|---|---|---|---|
| .text | 0x00401000 | 0x381AF1 | 0x1000 | code, RWX | raw offset == RVA (image-layout dump) |
| .rdata | 0x00783000 | 0x641DB | | R/W | original IAT at 0x783000 |
| .data | 0x007E8000 | 0xD35F8 | 0x35000 raw | R/W | |
| .rsrc | 0x008BC000 | 0x3854E | | R/W | |
| (blank name) | 0x008F5000 | 0x3AC16 | | discardable | **original base relocations**: 999 blocks, 91,105 HIGHLOW entries, all pointing in-image (71,093 in .text, 14,601 in .rdata, 5,411 in .data); data directory zeroed |
| (no name) | 0x00930000 | 0x2000 | | code+data, RWX | rebuilt import directory (data directory [1] points here) |

## Variant

Evidence says this is an **already-unpacked image**: zeroed relocation
directory, blank section names, fake timestamp, writable `.text` with raw
offsets equal to RVAs, and an import directory rebuilt in a trailing section.
No SecuROM/SafeDisc strings or stub code remain; `.text` is ordinary MSVC code
and analyses directly. The exact retail version (1.0/1.1/1.2) **cannot be
confirmed** without a reference hash. The 2005 dates are consistent with the
last patch, and the function addresses documented by
[yugecin/nfsu2-re](https://github.com/yugecin/nfsu2-re) match this file
(checked: 0x5BEA20 and 0x5BEEA0 start functions that reference
`Software\EA Games\Need for Speed Underground 2` at 0x7A30E8).

Nothing was done to the original file. `scripts/normalize_exe.py` writes an
analysis copy (`work/SPEED2.analysis.exe`) whose only differences are the
relocation data directory pointing at the existing relocation blocks and that
section's name set to `.reloc`.

## Imports

252 functions from 14 DLLs: advapi32 (6), d3d9 (1: `Direct3DCreate9`),
ddraw (1), dinput8 (1), dsound (1, ordinal 1 = `DirectSoundCreate`), gdi32 (12),
kernel32 (152), netapi32 (1), shell32 (1), shfolder (1), tapi32 (9),
user32 (38), winmm (6), ws2_32 (22). Full list in NFSU2_RECON.md.
