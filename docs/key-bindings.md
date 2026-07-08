# Key bindings — grammar, modifiers, conflicts (cross-platform spec)

Keeps the two shells' keybinding behaviour in step. Shipped Mac-first
(`mac/main.mm`); Windows (`source/irate.cpp`) should match this grammar.

## Token grammar

A key token = zero or more **modifier prefixes** followed by one **base key**.

Base keys:
- a single letter/digit (`A`, `7`) or punctuation char (`/`, `;`),
- a punctuation *name* (`SLASH`, `SEMICOLON`, `LBRACKET`, `RBRACKET`, `COMMA`,
  `PERIOD`, `MINUS`, `PLUS`, `GRAVE`, `QUOTE`, `BACKSLASH`) — matched by the
  character, so layout-tolerant,
- a named special (`LEFT RIGHT UP DOWN SPACE ESC ENTER TAB HOME END PGUP PGDN
  DELETE BACKSPACE F1`–`F12`).

Multiple keys per action are comma-separated in the `[keys]` ini
(`next=RIGHT,SPACE`). Modifiers use `+`, so there's no clash with the comma.

## Modifier prefixes  ← NEW (2026-07, Mac-first)

`SHIFT+`, `CTRL+` (or `CONTROL+`), `ALT+` (or `OPT+`/`OPTION+`), `CMD+` (or
`COMMAND+`). Any number, **any order** — `SHIFT+CTRL+R` == `CTRL+SHIFT+R`.

Cross-platform mapping of the ini tokens:

| ini token | macOS       | Windows            |
|-----------|-------------|--------------------|
| `SHIFT+`  | Shift ⇧     | Shift              |
| `CTRL+`   | Control ⌃   | Ctrl               |
| `ALT+`    | Option ⌥    | Alt                |
| `CMD+`    | Command ⌘   | Win key (or unused)|

The ini files are per-platform (macOS: `~/Library/Application Support/iRate/
irate.ini`; Windows: `%LOCALAPPDATA%\iRate\irate.ini`), so they are NOT shared —
but the grammar must be identical so behaviour and docs match.

**Serialization order (canonical):** `CTRL+ ALT+ SHIFT+ CMD+` then base, e.g.
`CTRL+SHIFT+R`, `SHIFT+CMD+F5`. Parsing accepts any order; writing is canonical.

**Display:** macOS uses the native symbols in help/prefs/keyword UIs —
`⌃ ⌥ ⇧ ⌘` (Control, Option, Shift, Command), e.g. `⌃⇧R`. Windows should use its
own convention (`Ctrl+ Alt+ Shift+ Win+`).

**Matching:** ALL modifiers are part of the key identity, so `Shift+X` and `X`
are different bindings, `Ctrl+Y` ≠ `Y`, etc. (`Shift+P` = Preferences by default,
distinct from `P` = purple label — this already worked; Ctrl/Alt/Cmd are new.)

### What was Shift-only before

macOS `KeySpec` gained `bool ctrl, alt, cmd` beside `shift`; `parseKeyToken`
strips the prefixes in a loop; `specForEvent` reads `NSEventModifierFlag{Shift,
Control,Option,Command}`; `actionForEvent` now does a full `KeySpec ==` compare
(all modifiers); `iniTokenForKey` and `keyDisplayName` emit the prefixes/symbols.

**Windows port (DONE 2026-07):** the keymap key is a `UINT` with `KM_SHIFT =
0x20000`, plus `KM_CTRL = 0x40000`, `KM_ALT = 0x80000`, `KM_WIN = 0x100000`.
`heldMods()` ORs the held modifiers into the lookup key; `parseKeyName` strips
prefixes in any order; `vkName(vk, forIni)` emits display form
(`Ctrl+Alt+Shift+Win+X`) or canonical ini tokens (`CTRL+ALT+SHIFT+CMD+X`).
Alt-combos arrive via `WM_SYSKEYDOWN`, which falls through to the shared handler
only for keys the app owns (so Alt+F4 etc. still reach DefWindowProc).

### Caveats

- The Mac app has no main menu, so `Cmd+`-combos reach the app's local key
  monitor and can be bound. A few system combos (`Cmd+Tab`, `Cmd+Space`, etc.)
  never reach the app — advise users to avoid those. Same spirit on Windows for
  `Win+`-combos owned by the shell.
- Modifier-only presses aren't captured (rebind capture is on key-down of a real
  key), so you can't bind "Shift" alone — as intended.

## Conflict warnings

When a key would clash across app controls / keyword sets, the editors warn or
refuse rather than let a key silently do two things (or nothing). Full rules and
message wording are in **`docs/keyword-sets.md` → "Key-conflict rules"**; the
shared helper is `keyUsedBy(KeySpec, ignoreAction, ignoreKwSlot)`.
