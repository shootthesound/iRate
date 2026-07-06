# iRate

Fast fullscreen raw culling. Native Win32, single ~560KB exe, no install, no browser tech.
Shows the JPEG preview embedded in the raw — never processes raw data.
Writes Lightroom-compatible XMP sidecars next to each file.

Supported: Sony .ARW, Canon .CR2/.CR3, Nikon .NEF/.NRW, Fuji .RAF, Panasonic .RW2,
Pentax .PEF, .DNG (Leica/Ricoh/drones/phones), Olympus/OM .ORF, plus .JPG — all verified
against real samples. RAW+JPG pairs (same basename) collapse to one entry shown as "+JPG"
— toggleable in Prefs. Preview quality is whatever the camera embeds (e.g. a7R5 full
9504×6336, Fuji/Pentax full-size, older Sonys 1616×1080).

## Run

Double-click `iRate.exe` and pick a folder (scanned recursively, remembers last folder),
or drag a folder onto the exe / `iRate.exe "E:\Shoot"`.
Reopening resumes where you were: image, sort order, grid/loupe mode, info bar settings.

## Keys (defaults — all rebindable, press H in-app to see current bindings)

| Key | Action |
|---|---|
| ← / → (or mouse wheel) | previous / next |
| 1–5 | rate — advances to next image **only while Caps Lock is on** |
| 0 | clear rating |
| X | reject toggle — writes `xmp:Rating="-1"` (Bridge/Photo Mechanic convention; Lightroom ignores XMP rejects, its flags are catalog-only). Filter by ✖ in F |
| 6 / 7 / 8 / 9 | Red / Yellow / Green / Blue label, same as Lightroom (press again to clear) |
| / | zoom to 100% — move mouse to pan; press again to zoom back out |
| G | toggle grid ↔ fullscreen — opens the selected image full screen, G again returns to grid |
| F | filter panel (grid) — rating + label combos and filename text; in loupe, F jumps to grid and opens it |
| [ / ] | cycle sort mode: Name / Date / Size |
| ; | flip sort direction A→Z / Z→A |
| Z | move info bar bottom ↔ top |
| H | keyboard shortcuts overlay (shows your configured keys) |
| P | Purple label (press again to clear) |
| Shift+P | preferences panel (also the Prefs toolbar button in grid) |
| Home / End | first / last image of the (filtered) set — works in grid too |
| I | toggle info bar |
| Esc | quit (saves position + settings) |

Numpad digits work too.

## Grid view

G toggles a thumbnail grid. Arrows/PgUp/PgDn navigate, Enter or double-click opens the image,
single click selects. Rating and label keys work on the selection. The toolbar has buttons for
everything with a shortcut: Prefs, Filter, Fullscreen (G), Start/End (same as Home/End), Name/Date/Size
sort and sort direction. Mouse wheel scrolls, and there is a native scrollbar on the right.

## Prefs (Shift+P, or toolbar button in grid)

A built-in settings panel — no text file editing. Click to set: after-rating behaviour
(Caps Lock gated / always / never), which file types are scanned (rescans immediately),
RAW+JPG pairing on/off,
info bar shown/hidden and top/bottom, resume-info location, and a Clear thumb cache button
(shows the cache's current size). To rebind a key: click the action, press the new key
(Esc cancels; taking a key already in use steals it from the other action). Everything saves
to irate.ini instantly, so it survives restarts. The ini stays hand-editable if you prefer.

## Filter (F)

Chips for each rating value (0–5★), rejected (✖, key X) and each label — click or press the matching key
(1–5/0 ratings, 6–9 labels, P purple, N no-label) to include/exclude; C clears, F/Esc closes.
Text box matches against the file path. The info bar shows "n / shown (of total)" while filtering.
Rating an image out of the current filter removes it from view immediately.

## Where things live

- **Per machine** — `%LOCALAPPDATA%\iRate\irate.ini` (keys, options). Settings from the app's
  JustRate era (`%LOCALAPPDATA%\JustRate\justrate.ini` or an ini next to the exe) migrate
  automatically.
- **Per job**, in the root of the folder you browse — `irate.session` (resume image, sort
  mode/direction, grid/loupe) and `irate_thumbs.jrc` (thumbnail cache). Both use relative
  paths/keys, so they stay valid if the whole shoot folder moves to another drive or machine.
  Old `justrate.session`/`justrate_thumbs.jrc` files are renamed in place on first open.
  Prefer folders untouched? Flip "Resume info: Central ini" in Prefs and both move to
  `%LOCALAPPDATA%\iRate` instead (switches live, no restart).
- **Per image** — ratings/labels in `.xmp` sidecars next to the raws, as always.

## Rebinding & options

Use the Prefs panel (Shift+P). Every key is rebindable there — or in `irate.ini` if you
prefer (multiple keys per action, comma separated; names like SLASH, LBRACKET, SHIFT+P or
literal characters all work; restart after hand-edits). `autoadvance=` accepts `capslock` (default),
`always`, `never`. `extensions=` controls scanned file types. Old ini files migrate automatically.

## Lightroom

Ratings/labels land in `NAME.xmp` sidecars (`xmp:Rating`, `xmp:Label`). Existing sidecars are
patched in place — develop settings already in them are preserved. In Lightroom, import the
folder, or for already-imported photos: Metadata → Read Metadata from Files.

## Speed

- Grid thumbnails preload aggressively: visible cells first, then rows ahead/behind, then the whole
  (filtered) set in the background on a low-priority queue — loupe browsing is never starved.
- Thumbnails persist in one `irate_thumbs.jrc` per job (auto-invalidates when a file changes;
  no size cap — dead entries are compacted away on startup). Reopening a shoot gives a
  near-instant grid.
- Holding an arrow key cycles as fast as decoding keeps up: repeats are absorbed dynamically while
  a preview is still decoding, and the last image stays on screen — no black frames. The rate
  rises again automatically the moment the system speeds up.
- Drive-aware scheduling: on spinning/USB/network drives, background thumbnail preloading drops
  to a single reader so it never fights your browsing for the disk, and the image you're on
  always decodes first. SSDs keep full parallel decoding. On all drives, ratings are written in
  the background — keys never wait on the drive — and Lightroom `.lrdata` preview folders are
  skipped when scanning.

## Notes

- Opening the filter the first time loads all sidecar ratings in the background ("loading ratings…").
- Sort by Date uses file modification time (equals capture time for raws off a card).
- Speed: previews decode at reduced JPEG scale straight to screen size, neighbours prefetch on
  background threads; grid thumbnails decode independently and are cached.
- Source in `source/` (built with zig c++ targeting x86_64-windows-gnu — see `source/build.bat`).
