# Code-review findings — keyword sets / key modifiers (2026-07)

An adversarial multi-agent review of the Windows port of keyword sets + key
modifiers found the bugs below. All are **fixed on Windows** (`source/irate.cpp`).
The feature shipped Mac-first, so several are likely present in `mac/main.mm` —
**Mac side: audit each "Mac check" below and fix where it applies.**

## Cross-platform logic bugs (high priority for the Mac audit)

1. **Sidecar clobber on failed read (DATA LOSS).** The keyword-toggle and
   rating-write paths read the sidecar, patch it, and atomically write it back.
   If the read FAILS TRANSIENTLY on an existing file (Lightroom/AV/Dropbox
   briefly holding it, short read), the patch functions receive an empty string,
   build a **fresh minimal sidecar**, and the write **replaces the user's real
   sidecar** — destroying crs:* develop settings, rating, label and keywords.
   *Windows fix:* `readSidecarForPatch()` — retry ×3 with 50ms sleeps; if the
   file exists but still can't be read, DROP the patch op (log it), never write.
   A missing file is fine (fresh sidecar is correct then).
   *Mac check:* whatever reads the sidecar before `xmpApply`/`xmpApplyKeywords`
   on the serial writer queue — does a failed read on an existing file fall
   through to a write?

2. **Keyword toggles fire on key auto-repeat.** Toggles are not idempotent:
   holding the set's key a beat too long flips keywords on/off per typematic
   repeat and the final state depends on repeat parity.
   *Windows fix:* keyword-slot dispatch ignores repeats (`lParam` bit 30).
   *Mac check:* the key monitor's slot dispatch must skip `NSEvent.isARepeat`.

3. **Keyword editor accepts keys the grid hardcodes.** Grid mode handles
   Up/Down/PgUp/PgDn/Enter before slot dispatch, so a set bound to one of those
   silently never fires in grid view (spec says sets work in grid).
   *Windows fix:* capture refuses those base keys with "That key navigates the
   grid — pick another."
   *Mac check:* whichever keys the Mac grid consumes unconditionally need the
   same refusal in its keyword editor.

4. **The new default binding (K) steals a user's existing K on upgrade.** An
   ini from before the feature has no `keywordsets=` entry; binding the default
   AFTER the user's `[keys]` silently overwrites whatever they had on K,
   with no conflict notice (load-time bypasses the rebind machinery).
   *Windows fix:* the K default is applied only when the ini has no
   `keywordsets` entry AND K is still unbound.
   *Mac check:* same load-order hazard if the Mac ini predates the feature.

5. **Quit shortcut capturable during key capture.** The capture path swallows
   everything, so the platform quit chord gets bound instead of quitting.
   *Windows fix:* Alt+F4 is exempted from capture and always reaches the OS.
   *Mac check:* can prefs/keyword capture swallow **Cmd+Q**? Exempt it.

6. **Keyword list truncation.** Words fields read/written through fixed
   512-char buffers truncated long keyword lists (possibly mid-keyword) and
   persisted the truncation.
   *Mac check:* any fixed buffer on the ini round-trip or editor field.

## Windows-specific (fixed; listed for completeness)

7. **Numpad twins vs keyword slots.** Windows auto-binds a digit's numpad twin
   for actions; slots didn't participate: rebinding an action to a digit left a
   slot on the numpad twin silently dead (no notice), and a slot bound to a
   plain digit didn't fire from the numpad. Fixed: twin-aware unbind + notice in
   rebindAction; kwSlotForKey mirrors unmodified numpad digits to the plain digit.
8. **Numpad keys dropped from ini serialization.** keysOfAction hid ALL numpad
   VKs (meant to hide auto-twins), so an explicitly captured numpad key was not
   persisted and the action came back keyless after restart. Fixed: hide a
   numpad VK only when the matching digit is bound to the same action.
9. **WM_SYSCHAR beep** on Alt+bound keys (no menu to lose) — now swallowed.

## Process note

The review also confirmed the WM_SYSKEYDOWN fall-through design (Alt combos
reach the shared handler only for keys the app owns) and refuted two suspected
races (bulk sidecar loader vs sidecar thread — serialized by the items mutex).
