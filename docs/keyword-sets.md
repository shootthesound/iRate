# Keyword sets — feature spec & porting notes

Shipped first in the macOS shell; this doc is the spec for keeping the Windows
shell in step. The shared XMP layer already lives in `source/core.h`, so the
Windows port is UI + config only.

**Status: both shells implement this spec** (macOS 2026-07, Windows 2026-07 —
`source/irate.cpp`: keyword toggles run on the serial sidecar thread, editor
overlay on K, conflict notices via `keyUsedBy`).

## What the user gets

Culling-speed keywording: up to **10 keyword sets**, each pairing a **key** with
a **comma-separated keyword list** (e.g. `F1` → `ceremony`, `F2` → `speeches,
top table`). Pressing a set's key while culling **toggles** those keywords on
the current image — if the image already has *all* of the set's keywords they
are removed, otherwise the missing ones are added. Works in loupe and grid
(applies to the selected image). No typing per image, so it never slows the
look-rate-next loop (design principle #1).

A dedicated editor screen (default key **K**, action `A_KEYWORDS`, ini name
`keywordsets`) manages the sets: per slot a key chip (click, then press the new
key), a ✕ to unbind, and a click-to-type keywords field.

**Storage is APP-LEVEL, not per folder** — the sets live in the app ini
(`[keywords]` section) so they follow the photographer across shoots. This was
an explicit owner decision.

## Sidecar format (shared, already in core.h)

Keywords are Lightroom/Bridge-standard `dc:subject`: an `rdf:Bag` of `rdf:li`
inside `rdf:Description`, with `xmlns:dc="http://purl.org/dc/elements/1.1/"`
declared on the Description. Same string-patching philosophy as rating/label:
the whole `dc:subject` block is replaced on write, everything else in the
sidecar (crs:* develop settings etc.) is preserved byte-for-byte.

New in `source/core.h` (all additive; nothing existing changed):

- `xmlEscape` / `xmlUnescape` — minimal XML entity handling for keyword text.
- `std::vector<std::string> xmpGetKeywords(const std::string& xmp)` — read the
  bag (empty vector = none).
- `std::string xmpApplyKeywords(existing, kws)` — replace the whole bag with
  the image's FULL keyword list; empty list removes the block; creates a fresh
  sidecar (`rating 0`, no label) if `existing` is empty/garbage. Handles the
  self-closing `<rdf:Description …/>` form (converts to open/close to hold the
  element child) and inserts `xmlns:dc` when missing.

Call pattern: **callers keep the image's full keyword list in memory** (parsed
at sidecar-load time via `xmpGetKeywords`) and pass the complete updated list to
`xmpApplyKeywords` on every change. Do not try to add/remove single items at
the XMP level.

Covered by round-trip tests (fresh create, escape round-trip, rating patch
preserving keywords and vice-versa, clear-removes-block, LR-style self-closing
Description with crs settings preserved, dc namespace insertion).

## Ini format (`[keywords]` section, app-level ini)

```ini
[keywords]
set1key=F1          ; key token, same grammar as [keys] (parseKeyName round-trip)
set1words=ceremony  ; comma-separated keywords; empty = slot unused
set2key=
set2words=
...up to set10
```

A slot fires only if it has both a key and non-empty words.

## Behaviour details (match these on Windows)

1. **Dispatch order**: app actions win. A keyword slot's key fires only when the
   keymap lookup misses (`A_NONE`). The editor refuses to capture a key that is
   currently bound to an action (message: rebind it in Preferences first), and
   stealing between slots is allowed (assigning slot 3 a key held by slot 7
   unbinds slot 7).
2. **Toggle semantics**: all-present → remove set's keywords; else → add the
   missing ones. Other keywords on the image are never touched.
3. **No auto-advance** after a keyword toggle (unlike rating/label) — several
   sets often apply to one image.
4. **Sidecar I/O**: reads happen with the existing rating/label sidecar load
   (one extra `xmpGetKeywords` call); writes go through the same serial sidecar
   writer as rating/label so ordering is preserved. The item's in-memory
   keyword list is the source of truth for the write.
5. **Info bar**: the image's keywords render at the end of the left-hand info
   string as `{kw1, kw2, kw3, kw4, …}` (first 4, then an ellipsis).
6. **Editor screen**: modal overlay like Prefs/Filter; rows = 10 slots
   (`Set N | [key chip] [✕] [words field]`), footer hint + `Close (Esc)`.
   Every change writes the ini immediately. Esc/Enter ends text editing;
   clicking outside a field ends it too.

## Key-conflict rules (match these on Windows)

Three things can own a key: an **app control** (action in the keymap), and any of
the **10 keyword slots**. App controls always win at dispatch time (a keyword slot
only fires when the keymap lookup returns `A_NONE`). The editors surface conflicts
rather than letting a key silently do two things — or silently nothing:

**Keyword editor — capturing a slot's key:**
- Key is bound to an **app control** → **refused**, with a message: *"That key runs
  an app control — pick another (or rebind that control in Preferences first)."*
  (Rationale: controls win, so accepting it would make the set dead on arrival.)
- Key belongs to **another keyword slot** → **stolen**, with notice: *"Moved that
  key here from Set N."* (the other slot is unbound).
- Otherwise → assigned silently.

**Preferences — rebinding an action's key:**
- Steals from **another action** if needed (rebind = replace, as before) and now
  shows what it took: *"Reassigned that key from “Next image”."*
- If the key was a **keyword set's**, that set is **unbound** (it would otherwise go
  silently dead, since actions win) and the same "Reassigned that key from keyword
  set N" notice is shown.
- Messages render in the prefs key-rebind header in gold; cleared when you arm a
  new action or reopen the panel.

Shared helper `keyUsedBy(KeySpec, ignoreAction, ignoreKwSlot)` returns a human
description ("“Next image”" / "keyword set 3" / nil) and is the single source for
these notices — worth mirroring so the Windows wording stays identical.

## macOS implementation map (for reference)

All in `mac/main.mm`: `A_KEYWORDS` in the action enum + `kActRows` (so help and
prefs-rebind pick it up automatically); `KwSlot g_kwSlots[10]` + `splitKeywords`
near the keymap; `keyUsedBy()` conflict helper beside `bindActionToKey`;
`[keywords]` load/save in `loadConfig`/`writeIni`; `Item.keywords` populated in
`loadSidecar`/`loadAllSidecars`; `writeSidecarKeywords` beside `writeSidecar`;
editor UI `drawKeywords` / `kwClickAt` on the view; `openKeywords` /
`handleKeywordsKey` / `toggleKeywordSlot` on the delegate; conflict notices via
`g_kwMsg` (keyword editor) and `g_prefsMsg` (prefs rebind); slot dispatch in the
key monitor's `A_NONE` branch.
