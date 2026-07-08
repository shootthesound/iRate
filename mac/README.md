# iRate — macOS port

The macOS shell for iRate. Shares `../source/core.h` (raw preview + EXIF parser,
XMP sidecar logic) verbatim with the Windows build — that file stays platform-
clean; **do not** add Mac or Windows specifics to it. Everything Mac lives here
in `mac/`; the canonical Windows source in `../source/` is untouched.

## Build & run

```sh
cd mac
./build.sh            # -> build/iRate.app  (ad-hoc signed)
open build/iRate.app  # or double-click it in Finder
```

Requires the Xcode command line tools (`xcode-select --install`). No Xcode
project, no dependencies beyond system frameworks (Cocoa, ImageIO).

On first launch iRate asks you to pick the folder / card / drive of raws. macOS
may then show its own permission prompt (TCC) the first time it reaches a
Desktop/Documents/removable/network location — that's expected. The choice is
saved as a **security-scoped bookmark** in
`~/Library/Application Support/iRate/folder.bookmark`, so the next launch goes
straight to the images. Delete that file to be asked again.

## Status

Done (Milestone 1 — the cull loop):
- Folder-permission flow: `NSOpenPanel` grant + security-scoped bookmark persistence.
- Recursive scan of the chosen folder (skips `*.lrdata`, same as Windows).
- ImageIO decode of the embedded preview `core.h` locates, with EXIF rotation
  baked into materialized pixels (the WIC-TRAP lesson: no lazy orientation chain).
- Fullscreen borderless display + info bar (filename, index, ISO / shutter /
  aperture / focal, stars / reject / label).
- Navigation: ← → / Space, Home, End.
- Rate 0–5, labels (6–9 + P), reject (X), all written to XMP sidecars through
  `core.h`; every sidecar read/write is on a dedicated serial queue so the UI
  thread never touches the photo drive.
- Held-arrow repeats swallowed while the current preview decodes (never black).
- Caps-Lock–gated auto-advance after rating (matches the Windows default).

Done (Milestone 2 — SPEED, principle #1):
- Decode cache keyed by item index: a revisited frame paints instantly, no re-decode.
- Neighbour prefetch (± `CACHE_RADIUS`, biased in the direction of travel) on a
  background QoS queue, so forward-nav lands on already-decoded frames.
- Concurrent decode workers (on-screen at `USER_INITIATED`, prefetch at `UTILITY`);
  an in-flight set stops duplicate decodes; the window is evicted around the cursor.

Done (Milestone 3 — config + help):
- INI config at `~/Library/Application Support/iRate/irate.ini`, written with
  defaults on first run. `[keys]` rebinds any action; `[options]` covers
  `autoadvance` (capslock/always/never) and `showinfo`.
- Data-driven keymap: named specials (arrows, Home/End, Esc, F-keys, …),
  letters/digits/punctuation, and `SHIFT+` chords, comma-separated per action.
- Help overlay (H) generated from the same `kActRows` table that drives the binds
  (design principle #3 — never list shortcuts twice). Info bar top/bottom (Z).

Done (Milestone 4 — sort + session + pairing):
- Sort by name / date / size, ascending/descending (`[` `]` `;`), natural
  (Finder-style) name ordering. Re-sort keeps the on-screen frame and never
  flashes black; a generation counter discards decodes started before the remap.
- Per-folder session resume: `irate.session` in the folder root remembers the last
  file (relative path), sort mode, and grid mode — reopen lands where you left off.
- RAW+JPG pairing: same-name twins collapse to the raw, flagged `+JPG`.

Done (Milestone 5 — grid):
- Scrollable contact sheet (`G`), thumbnails decoded on demand into their own
  cache, rating/reject/label badges per cell, arrow-key selection, PageUp/Down,
  Enter opens the selected frame in the loupe (prefetched, so it's instant).

Done (Milestone 6 — 100 % zoom):
- `/` toggles a pixel-level loupe; the current preview is re-decoded at full
  resolution for real focus-checking, arrows pan. A new frame exits zoom.

Done (Milestone 7 — filter):
- `F` cycles rating filters (all → picks ≥1★ → unrated → rejects → keepers) over a
  `g_view` indirection; keeps the current frame if it survives, else the nearest.
  All sidecars are read once up front (one sequential reader) so filters and grid
  badges reflect disk without visiting each frame.

Done (Milestone 8 — disk cache + drive-aware + grid mouse):
- Persistent thumbnail cache `irate_thumbs_mac.jrc` in the folder root: an
  append-log of pre-decoded thumbnail JPEGs keyed by FNV-1a of the relative
  lowercased path (+mtime/fsize freshness guard), `JRTC` magic. Grid and cold
  restart are instant — no re-parsing every raw. Distinct filename from the
  Windows `irate_thumbs.jrc` so a shared drive can't cross-corrupt.
- Drive-aware scheduling: on launch iRate probes the drive (network volumes are
  slow; else it times scattered uncached reads — spinning disks / slow card
  readers exceed the threshold). Background decodes (prefetch + thumbnails) pass
  through a semaphore — one sequential reader on a seek-bound drive, a small pool
  on a fast one — while the on-screen decode always bypasses it and runs at once.
- Grid mouse: click selects a cell, double-click opens it in the loupe, scroll
  wheel scrolls the sheet.

Done (Milestone 9 — in-app preferences / rebind UI):
- **Shift+P** opens a Preferences overlay generated from the same `kActRows` table.
  `↑↓` select, **Enter** captures the next key to rebind an action (stealing it
  from whatever held it), **Delete** clears an action's keys, **Esc** closes — and
  every change writes the ini and takes effect live, no relaunch. The overlay also
  toggles options (auto-advance mode, info bar, RAW+JPG pairing, session location).
  No more hand-editing the ini (though it still works if you prefer).

Done (Milestone 10 — full filter panel + cache compaction):
- **F** opens a filter panel: a filename-substring field (type to search) plus
  per-rating (reject / unrated / 1–5★) and per-label (none / 5 colours) toggles.
  `↑↓` select, **Space** toggles, everything applies live over the `g_view`
  indirection. Rating + label + text combine.
- Thumb-cache compaction: on open, if a >32 MB cache is >40 % dead bytes it is
  rewritten keeping only the live record per image (atomic rename; the old file is
  kept on any failure) — mirrors the Windows build's startup compaction.

**The port is feature-complete** — nothing from the Windows build is left out.

Done (Milestone 11 — keyword sets, Mac-first):
- Up to 10 app-level keyword sets, each a key + comma-separated keywords; press
  the key while culling to toggle those keywords on the current image (written
  to the sidecar as a Lightroom-standard `dc:subject` bag via `core.h`).
- Dedicated editor on **K**: click a key chip then press the key, ✕ unbinds,
  click the field to type keywords. Saved in the app ini (`[keywords]`), NOT per
  folder — sets follow you across shoots. Spec + Windows porting notes in
  `docs/keyword-sets.md`.

## Icon

`makeicon.mm` renders the 1024px master (a gold rating star on a dusk gradient
squircle with an aperture ring + speed streaks) via Core Graphics. To regenerate
`iRate.icns`:

```sh
clang++ -fobjc-arc makeicon.mm -o /tmp/makeicon -framework Cocoa
/tmp/makeicon /tmp/icon_1024.png
mkdir iRate.iconset
for s in 16 32 128 256 512; do
  sips -z $s $s /tmp/icon_1024.png --out iRate.iconset/icon_${s}x${s}.png
  sips -z $((s*2)) $((s*2)) /tmp/icon_1024.png --out iRate.iconset/icon_${s}x${s}@2x.png
done
iconutil -c icns iRate.iconset -o iRate.icns && rm -rf iRate.iconset
```
`build.sh` copies `iRate.icns` into the bundle; `Info.plist` references it.

## Files

- `main.mm` — the whole AppKit + ImageIO shell (single translation unit).
- `build.sh` — one-command build into `build/iRate.app`.
- `Info.plist` — bundle metadata + TCC usage strings.
- `iRate.entitlements` — user-selected files + app-scope bookmarks. Non-sandboxed
  by default; flip on the sandbox to ship via the App Store (no code change —
  bookmarks are already created/resolved with the security scope).
