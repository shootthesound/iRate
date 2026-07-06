# CLAUDE.md — iRate project knowledge

Fast fullscreen raw culling app for Windows. Single static exe, native Win32, no
dependencies beyond stock Windows 10/11 x64. Owner: Peter (peter@shootthesound.com),
shoots motorsport on a Sony a7R5. The name says it all: look at frame, press
number, next frame. Nothing that slows that down gets added.

Renamed from **JustRate** to **iRate** in July 2026. The code carries one-shot
migration shims for JustRate-era state (see "Where state lives") — don't remove
them until the owner says all machines/shoots have been opened once under iRate.

## Design principles (agreed with owner, in order)

1. SPEED is king. Never process raw data — only the embedded JPEG previews.
2. No browser tech of any kind. Pure Win32 + WIC.
3. Everything with a keyboard shortcut is rebindable and appears in the H overlay
   and the Prefs rebind list — both are generated from one table (kActRows), never
   hardcoded twice.
4. Held-arrow cycling must never show black: repeats are swallowed while the current
   preview decodes (stateless per-repeat check → speed recovers instantly when the
   machine frees up), last frame stays up.
5. Don't add speculative features. Every feature here was explicitly requested.

## Files

- `source/core.h` — pure C++ (no Windows deps): raw parsing + XMP sidecar logic.
  Unit-testable anywhere via `source/test_core.cpp` (run against sample raws).
- `source/irate.cpp` — the entire Win32 app (~2600 lines, single TU).
- `source/build.bat` — exact build command. Cross-compiles from anywhere with zig:
  `zig c++ -target x86_64-windows-gnu -O2 -municode -DUNICODE -D_UNICODE -w
  irate.cpp -o iRate.exe -Wl,--subsystem,windows -lole32 -luser32 -lgdi32
  -lshell32 -lshlwapi -lmsimg32`  (pip install ziglang → `python -m ziglang c++`).
- `README.md` — user-facing docs. Keep in sync with behaviour changes.
- Test data: `other raws/` = 12-file regression suite — owner's a7R5 ARW plus
  11 cameras (2016–2023, all supported formats, CC0 from raw.pixls.us). Git-ignored.

## Format knowledge (hard-won, verified against real files)

Parser strategy: walk every IFD (IFD chain + SubIFDs 0x14A + Exif IFD), collect all
JPEG candidates, pick the LARGEST that starts FFD8FF. Candidates come from:
- 0x0201/0x0202 pairs in any IFD (Sony ARW hides the big one in a 3rd chained IFD;
  a7R5 embeds full 9504×6336, pre-2021 Sonys only 1616×1080 — that's the file).
- CR2: IFD0 strips (0x111/0x117) when Compression==6 — IFD0 ONLY. Later IFDs hold
  the lossless-JPEG raw which fakes an FFD8 signature but WIC cannot decode it.
  (This was a real bug: without it CR2 served the 160×120 thumbnail.)
- DNG: strip pairs from IFDs with NewSubfileType==1 and Compression 6/7 only —
  the subfiletype gate is what keeps raw sensor IFDs out.
- CR3 (ISO boxes, all sizes big-endian): trak→stbl→stsz/co64 first sample (the
  full-size JPEG track), PRVW inside uuid eaf42b5e…, EXIF from CMT1 (IFD0 tags) and
  CMT2 (Exif tags at ITS IFD0) inside uuid 85c0b687….
- RAF: "FUJIFILM" magic; JPEG offset/len at bytes 84/88 big-endian; full-size.
- RW2: TIFF magic II 0x55 0x00; JpgFromRaw = tag 0x2E (UNDEF blob) in IFD0.
- ORF: TIFF magic IIRO/IIRS/MMOR; preview pointer lives in MakerNote (0x927C,
  "OLYMPUS\0" header, IFD at +12) → CameraSettings sub-IFD (tag 0x2020) → 0x101
  offset / 0x102 length, ALL offsets relative to the MakerNote start.
- NEF/NRW: plain big-endian TIFF, generic walk finds it. PEF: plain LE TIFF.
- RAF/ORF keep EXIF inside the preview JPEG → finish() harvests it from there when
  the container gave none (scanJpegExif walks APP1, either endianness).
- EXIF orientation is honoured (WIC rotator); guard `visited` set prevents IFD loops.

## XMP sidecars (Lightroom interop)

`basename.xmp` next to the raw. Only xmp:Rating and xmp:Label are touched; existing
sidecars are string-patched in place (attribute OR element form), preserving crs:*
develop settings. kXmpKeep sentinel = leave field alone. Rating -1 = rejected
(Bridge/Photo Mechanic convention; Lightroom IGNORES XMP rejects — its flags are
catalog-only; this is flagged in the app's H and Prefs screens). Labels: Red/Yellow/
Green/Blue/Purple exactly as Lightroom spells them. Fresh sidecars stamp
x:xmptk="iRate 1.0".

## App architecture (irate.cpp)

- Items → filtered view: `g_view` maps view positions → item indices; `g_cur` is a
  VIEW position; `curItem()` resolves. Decode caches are keyed by ITEM index.
- Decode pipeline: WIC, IWICBitmapSourceTransform for DCT-domain downscale (the
  interface is hand-declared — mingw headers lack it; GUID 3B16811B-6A43-4ec9-B713-
  3D5A0C13B940, vtable: CopyPixels, GetClosestSize, GetClosestPixelFormat,
  DoesSupportTransform; falls back to scaler if QI fails).
- Two queues: high (fullscreen images + 100% zoom) drained before low (thumbnails).
  Generation counter invalidates in-flight decodes across sort changes.
- Thumb disk cache `irate_thumbs.jrc`: append-log of {pathHash(FNV-1a of
  RELATIVE lowercased path), mtime, fsize, w, h, len, jpeg}. "JRTC" magic (kept
  across the rename — it's a format tag, not branding). Later entries win.
  No size cap; startup compaction rewrites when >40% dead bytes and file >64MB.
- Modes: loupe (fullscreen) ↔ grid (G toggles; Enter/dbl-click also exit grid —
  ALL grid exits must ShowScrollBar(FALSE), this was a bug once).
- Keybinds: `g_keymap` UINT→Action; KM_SHIFT flag bit 0x20000 encodes Shift+key;
  digits auto-bind numpad twins (unshifted only). parseKeyName/vkName round-trip
  names incl "SHIFT+X". Rebinding steals keys from other actions and writes ini
  immediately (writeKeysToIni per affected action).
- Caps Lock gates auto-advance after rating/reject (GetKeyState(VK_CAPITAL) & 1);
  ini `autoadvance=capslock|always|never`.

## Where state lives

- Per machine: `%LOCALAPPDATA%\iRate\irate.ini` (keys/options; `[state] version`
  gates migrations, currently 4). One-shot migration at startup: if missing, copies
  `%LOCALAPPDATA%\JustRate\justrate.ini`, else an `irate.ini`/`justrate.ini` next
  to the exe.
- Per job (in browsed folder root, relative paths so folders can move):
  `irate.session` (resume file, sort, grid mode) + `irate_thumbs.jrc`.
  JustRate-era `justrate.session`/`justrate_thumbs.jrc` are renamed in place on
  first open of that folder. `sessioninfolder=0` moves both to LOCALAPPDATA
  (toggle in Prefs, live).
- Per image: .xmp sidecars. NOTHING else is ever written into photo folders.

## Environment (this is the Windows build machine)

- Claude Code runs natively on the owner's Windows 11 box; the working directory
  `W:\Peter\Documents\Development\iRate` is the canonical checkout. (Older notes
  about a Cowork Linux sandbox / flaky mount are obsolete.)
- Build: `python -m ziglang c++ ...` per build.bat (`pip install ziglang`; plain
  `zig` is not on PATH). First compile after a zig (re)install builds libc++ —
  minutes of noisy warnings, harmless, cached afterwards.
- Executables run here: build test_core.cpp with the same toolchain and run it
  directly. The owner can also launch iRate.exe immediately for UI verification —
  Claude should still not launch the fullscreen exe itself unasked (it takes over
  the screen).
- GitHub: private repo, owner's account, `gh` CLI available. The Mac port
  (see below) is developed from a clone of this repo.

## Testing

Build + run the harness against the suite (from the repo root):
`python -m ziglang c++ -O2 -w source/test_core.cpp -o <scratch>/test_core.exe`
then `test_core.exe "other raws"/*` — parses every file given, checks preview is
a decodable JPEG (and the BIG one for a7R5), EXIF present, plus XMP round-trip/
patch/reject unit tests. Run after ANY core.h change. Check chosen preview
DIMENSIONS (SOF scan), not just parse success — the CR2 thumbnail bug parsed "fine".

## Known limits / possible future work

- Sigma X3F, Hasselblad 3FR/FFF, Phase One IIQ, pre-2015 Fuji quirks: unsupported,
  add on demand (same candidate pattern).
- Sort-by-date uses file mtime, not EXIF capture time (identical for card copies).
- Nikon lens name lives in the makernote → lens field empty on some NEFs.
- macOS port: NEXT UP (July 2026) — owner pulls this repo on his Mac. core.h ports
  as-is; the Win32 shell (~1500 lines of irate.cpp) gets rewritten on
  AppKit/ImageIO. Keep core.h strictly platform-clean so both shells share it.
