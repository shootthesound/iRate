<div align="center">

# ⭐ iRate

### Look at the frame. Press a number. Next frame.

**The fastest way from a full memory card to a rated shoot.**
Free · native · tiny · your raws are never touched.

<a href="https://github.com/shootthesound/iRate/releases/download/macos-v1.0/iRate-macos-1.0.zip"><img src="https://img.shields.io/badge/%EF%A3%BF%20Download%20for%20macOS-000000?style=for-the-badge&logo=apple&logoColor=white" alt="Download for macOS"></a>
<a href="https://github.com/shootthesound/iRate/releases/download/windows-v1.0/iRate-windows-1.0.zip"><img src="https://img.shields.io/badge/Download%20for%20Windows-0078D4?style=for-the-badge&logoColor=white" alt="Download for Windows"></a>
<a href="https://buymeacoffee.com/lorasandlenses"><img src="https://img.shields.io/badge/Buy%20me%20a%20coffee-FFDD00?style=for-the-badge&logo=buy-me-a-coffee&logoColor=black" alt="Buy Me A Coffee"></a>

*macOS 11+ (Apple Silicon & Intel) · Windows 10/11 x64 · [all releases](https://github.com/shootthesound/iRate/releases)*

</div>

---

## Why "iRate"

I shoot motorsport. A race weekend comes home as **thousands of raw frames**, and every one
of them deserves two seconds of my attention — not two seconds of *loading*.

Culling in Lightroom meant: import, wait for previews to build, watch the loupe hitch
between frames, feel the rating keys lag behind my fingers, repeat until midnight.
The tool for looking at photographs was the slowest part of looking at photographs.

**Lightroom made me irate. So I built iRate.** The name is the entire spec: ***I rate***
*photos, without becoming* ***irate***.

Everything in this app follows one rule — **nothing is allowed to be slower than you
looking**. No import step. No preview building. No catalog. Open a folder, the first
frame is on screen, and every keypress lands *now*.

## The trick that makes it instant

Every raw file already contains a **full-quality JPEG preview**, rendered by your
camera the moment you pressed the shutter. iRate finds that preview — even when the
camera hides it three headers deep — and shows you *that*. It never decodes raw sensor
data, so a 61-megapixel file opens like a snapshot, on a laptop, from a card reader.

On top of that:

- **Hold → and fly.** Frames advance exactly as fast as they decode; the current image
  stays up while the next arrives. You will never see a black frame or a spinner.
- **It prefetches in the direction you're moving**, so the next frame is usually
  already decoded before you ask for it. Going back? Instant — it's cached.
- **Drive-aware.** On a USB spinning disk, a slow card reader or a network share,
  background work drops to a single reader so it never fights your browsing for the
  disk. On an SSD it uses everything your machine has.
- **The grid remembers.** Thumbnails persist per shoot — reopen a 5,000-frame folder
  and the contact sheet is simply *there*.
- **Native code, one tiny binary, zero dependencies.** AppKit + ImageIO on the Mac,
  Win32 + WIC on Windows. No Electron, no runtime, nothing to install. The Windows
  build is a single 620KB exe.

## Built for the cull

Put **Caps Lock on** and rating auto-advances: look, press, look, press. A thousand
frames becomes a rhythm, not a chore.

- **1–5** rate · **0** clear · **X** reject · **6/7/8/9/P** colour labels
- **G** grid with rating/label badges on every cell
- **F** filter by rating, label, filename — rate a frame out of the filter and it
  leaves the view immediately
- **/** true 100% zoom for focus checks, mouse pans
- Sort by name / date / size, flip direction, all one key each
- **RAW+JPG pairs** collapse to a single entry (shown `+JPG`)
- Quit any time — reopening lands on the exact frame, same sort, same view
- Every single key is **rebindable** in-app (**Shift+P**); **H** shows your live bindings

## It speaks fluent Lightroom

Ratings and labels are written as **XMP sidecars** — the same `xmp:Rating` and
`xmp:Label` that **Lightroom, Bridge and Photo Mechanic** read. Cull in iRate at full
speed, then import into Lightroom and your stars and colours are simply *there*
(already imported? **Metadata → Read Metadata from Files**).

- Existing sidecars are **patched in place** — develop settings you've already made
  survive untouched.
- Your raw files are **never modified**. Nothing is ever written into your photo
  folders except sidecars (and an optional per-shoot resume file you can turn off).
- Rejects use the Bridge / Photo Mechanic convention (`xmp:Rating="-1"`). Lightroom
  ignores XMP rejects by design — its flags are catalog-only — so filter on ✖ in
  iRate, or on zero-star, before import.

## Supported formats

**Sony ARW · Canon CR2 / CR3 · Nikon NEF / NRW · Fuji RAF · Panasonic RW2 ·
Olympus / OM ORF · Pentax PEF · Adobe DNG · JPEG**

All verified against real camera samples — including the format quirks: Sony's
hidden full-size previews, Canon CR3's ISO boxes, Olympus previews buried in
MakerNotes. If your camera isn't served the biggest preview its files contain,
that's a bug — open an issue.

## Keys

| Key | Action |
|---|---|
| ← → / Space / wheel | previous / next |
| 1–5 · 0 | rate · clear (Caps Lock = auto-advance) |
| X | reject toggle |
| 6 7 8 9 P | Red / Yellow / Green / Blue / Purple label |
| G | grid ⇄ fullscreen |
| / | 100% zoom |
| F | filter |
| [ ] ; | sort mode / direction |
| I · Z | info bar on-off · top-bottom |
| Home / End | first / last |
| H · Shift+P | help · preferences |
| Esc | quit (position saved) |

## Install

### macOS

1. **[Download](https://github.com/shootthesound/iRate/releases/download/macos-v1.0/iRate-macos-1.0.zip)**, unzip, drop **iRate.app** into Applications.
2. First launch only: **right-click → Open** (the build is ad-hoc signed, not
   notarized). If macOS claims it's "damaged":
   `xattr -dr com.apple.quarantine /path/to/iRate.app`
3. Pick your folder / card / drive of raws. macOS may ask permission for that
   location once — the grant is remembered.

Settings live in `~/Library/Application Support/iRate/`. Build from source:
`cd mac && ./build.sh` (just the Xcode command line tools — see [`mac/README.md`](mac/README.md)).

### Windows

1. **[Download](https://github.com/shootthesound/iRate/releases/download/windows-v1.0/iRate-windows-1.0.zip)**, unzip anywhere — it's a single portable exe.
2. First launch only: if SmartScreen objects, **More info → Run anyway** (unsigned build).
3. Double-click and pick a folder — or drag a folder onto the exe, or
   `iRate.exe "E:\Shoot"`.

Settings live in `%LOCALAPPDATA%\iRate\` (there's a full Preferences UI in-app; the
ini stays hand-editable, and `debuglog=1` writes timing diagnostics if you're
curious). Build from source: `source\build.bat` (zig c++ — `pip install ziglang`
and it cross-compiles from anywhere).

## Where things live

- **Per machine** — one ini (locations above): your keys and options.
- **Per shoot**, in the folder you browse — `irate.session` (resume point, sort,
  view) and a thumbnail cache, both using relative paths so a shoot folder can move
  to another drive or machine and stay resumable. Prefer photo folders pristine?
  One preference moves both into the app's settings folder.
- **Per image** — `.xmp` sidecars beside the raws. That's it. Ever.

## Support

iRate is free. If it gives you your evenings back, you can support development with
a coffee — entirely optional:

<a href="https://buymeacoffee.com/lorasandlenses"><img src="https://img.shields.io/badge/Buy%20me%20a%20coffee-FFDD00?style=for-the-badge&logo=buy-me-a-coffee&logoColor=black" alt="Buy Me A Coffee"></a>

Found a raw file that shows the wrong preview, or a camera that isn't supported?
[Open an issue](https://github.com/shootthesound/iRate/issues) — format support is
driven by real files.
