# iRate

**Cull a shoot at the speed you can look at it.**

Fullscreen raw culling for **macOS** and **Windows**. Look at the frame, press a number,
next frame. No import step, no catalog, no waiting — just you and the pictures.

### ⬇ Download

|  |  |
|---|---|
| 🍎 **[iRate for macOS](https://github.com/shootthesound/irate/releases/download/macos-v1.0/iRate-macos-1.0.zip)** | Universal (Apple Silicon + Intel), macOS 11+ |
| 🪟 **[iRate for Windows](https://github.com/shootthesound/irate/releases/download/windows-v1.0/iRate-windows-1.0.zip)** | Windows 10/11 x64, single portable exe |

All releases: **[github.com/shootthesound/irate/releases](https://github.com/shootthesound/irate/releases)**

---

## Why it's fast

- Shows the **full-quality JPEG preview embedded in every raw** — it never processes raw
  data, so a 61MP file opens as fast as a snapshot.
- **Native code, one binary, zero dependencies.** AppKit + ImageIO on the Mac, Win32 + WIC
  on Windows. No Electron, no browser tech, no runtime to install.
- **Hold an arrow key and fly.** Frames advance exactly as fast as they decode, the last
  image stays up while the next arrives — you never see a black frame.
- **Neighbours prefetch in your direction of travel**; revisited frames paint instantly
  from cache.
- **Drive-aware.** On a USB spinning disk, slow card reader or network share, background
  work drops to a single reader so it never fights your browsing for the disk. SSDs get
  full parallel decoding.
- **Grid thumbnails persist per shoot** — reopen a folder and the contact sheet is just
  *there*, even for thousands of images. Portrait frames decode as fast as landscape.

## Made for a culling workflow

- **1–5** rate · **0** clears · **X** rejects · **6–9 / P** colour labels — and with
  **Caps Lock on, rating auto-advances** to the next frame. A thousand frames becomes:
  look, press, look, press.
- Ratings and labels are written as **XMP sidecars** — the same `xmp:Rating` /
  `xmp:Label` that **Lightroom, Bridge and Photo Mechanic** read. Existing sidecars are
  patched in place, so develop settings survive. Your raw files are **never modified**.
- **Grid view** (G) with rating/label badges, **filter** (F) by rating, label and filename,
  **100% zoom** (/) for focus checks, sort by name/date/size — everything a cull needs and
  nothing that slows one down.
- **RAW+JPG pairs** collapse to one entry (shown as `+JPG`), toggleable.
- Per-shoot resume: reopen a folder and you're back on the exact frame, same sort, same view.

## Supported formats

Sony ARW · Canon CR2/CR3 · Nikon NEF/NRW · Pentax PEF · Adobe DNG · Fuji RAF ·
Panasonic RW2 · Olympus ORF · JPEG — all verified against real camera samples.

## Keys

Press **H** in-app for the live list; every key is rebindable in Preferences (**Shift+P**).

| Key | Action |
|---|---|
| ← → / Space / wheel | previous / next |
| 1–5 · 0 | rate · clear rating (Caps Lock on = auto-advance) |
| X | reject toggle (`xmp:Rating="-1"` — Bridge/Photo Mechanic convention) |
| 6 7 8 9 P | Red / Yellow / Green / Blue / Purple label |
| G | grid ⇄ fullscreen |
| / | 100% zoom |
| F | filter (rating / label / filename) |
| [ ] ; | sort mode / direction |
| I · Z | info bar on-off · top-bottom |
| Home / End | first / last |
| H · Shift+P | help · preferences |
| Esc | quit (position saved) |

## macOS

- **Requires macOS 11 (Big Sur) or later.** Universal binary.
- Unzip, drop **iRate.app** in Applications, **right-click → Open** the first time
  (the build is ad-hoc signed, not notarized). If macOS calls it "damaged":
  `xattr -dr com.apple.quarantine /path/to/iRate.app` — once.
- On first launch, pick the folder / card / drive of raws. macOS may ask permission for
  that location; the grant is remembered (security-scoped bookmark).
- Settings: `~/Library/Application Support/iRate/irate.ini` (a Preferences UI covers
  everything — hand-editing optional).
- Build from source: `cd mac && ./build.sh` (Xcode command line tools only). See
  [`mac/README.md`](mac/README.md).

## Windows

- **Requires Windows 10 or 11, x64.** No install — unzip anywhere and run; it's portable.
- SmartScreen may object once (unsigned build): **More info → Run anyway**.
- Launch and pick a folder, drag a folder onto the exe, or `iRate.exe "E:\Shoot"`.
- Settings: `%LOCALAPPDATA%\iRate\irate.ini` (Preferences UI in-app; the ini stays
  hand-editable). If anything ever feels slow, set `debuglog=1` there and the app writes
  timing diagnostics to `irate.log` beside it.
- Build from source: `source\build.bat` (zig c++, cross-compiles from anywhere:
  `pip install ziglang`).

## Where things live

- **Per machine** — one ini (locations above): keys, options.
- **Per shoot**, in the folder you browse — `irate.session` (resume point, sort, view) and
  a thumbnail cache. Both use relative paths, so a shoot folder can move drives or
  machines and stay resumable. Prefer photo folders untouched? Flip one preference and
  both live in the app's settings folder instead.
- **Per image** — `.xmp` sidecars next to the raws. Nothing else is ever written into
  your photo folders.

## Lightroom

Import the folder and ratings/labels are simply there — or for already-imported photos:
**Metadata → Read Metadata from Files**. (Note: Lightroom ignores XMP *rejects* by design —
its flags are catalog-only. Bridge and Photo Mechanic read them.)
