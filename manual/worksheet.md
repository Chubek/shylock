# Poindexter worksheet TUI/GUI

The worksheet is a typed view of reviewed price observations, not a general
accounting workbook. Both frontends use the same `shylock.worksheet/v1` model
and On1x controller in `src/worksheet.on1x`. MuLIB TUI/vterm/termkey handle
terminal drawing and key decoding. MuLIB ImmGTK/Mune/GLX handle graphical
cells, pointer capture and software rendering; SDL2 supplies only the desktop
window, input events and framebuffer presentation.

## Launch and build

After the offline workflow in [Poindexter](poindexter.md):

```sh
./shylock tui prices.db cottages-eur alice
./shylock gui prices.db cottages-eur alice
./shylock sheet prices.db cottages-eur
```

`alice` is the reviewer label for new revisions, not an authenticated login.
`sheet` emits a JSON snapshot without starting an interface or changing reviews.
An empty cohort argument selects all review heads. Large selections are bounded
to 16384 rows. `tui` requires a UTF-8 ANSI-compatible terminal unless an event
script is explicitly supplied.

The default GNU Make build detects SDL2 with `pkg-config`. `./build.sh GUI=1`
requires SDL2 development files; `GUI=0` omits the window adapter. The CLI, TUI,
and headless graphical renderer still build without SDL2. Native option changes
invalidate the native build configuration automatically. Interactive text uses
MuLIB fontdraw and runtime FreeType when a supported system monospace font is
available; set `SHY_UI_FONT=/path/to/font.ttf` to select another font. The small
ASCII fallback displays unknown glyphs as `?`; it never changes stored text.
Headless renders use that fallback for reproducible pixels.

## Cells and navigation

| Cell | Type | Editable |
| --- | --- | --- |
| Listing identity | Text | Yes |
| Amount in minor units, scale | Integer | No |
| Currency | Supported explicit ISO code | Resolve unknown source currency; cannot contradict known evidence |
| Stay start/end | Valid `YYYY-MM-DD` dates | Yes |
| Guests | Integer 1..64 | Yes |
| Taxes, fees | `included`, `excluded`, `unknown` | Yes |
| Basis, qualifier | Evidence meaning | Resolve unknowns; cannot contradict known evidence |
| Conflict resolution | Explicit rationale, 1..256 bytes | Yes |
| Review decision | `accepted`, `rejected` | Yes |
| Capture time, provenance, review/observation IDs, reviewer, published reason | Audit/evidence | No |
| Cohort, source URL, extraction/capture IDs, collection/find IDs | Lineage | No |

Money remains integer minor units. The grid shows scale explicitly and never
converts a nightly quote, stay total or fee into another meaning. Pre-revision
flat legacy rows are readable but not editable; re-import their evidence to
create revision heads. The current amount is never an editable scratch value.
Resolving a missing currency reparses the **original amount token** with checked
MuLIB money at save; it does not convert or replace an existing quote. An earlier
reviewer interpretation of an originally unknown currency can be corrected in a
new revision. A source's explicit known currency cannot.

Basis supports `nightly`, `weekly`, `monthly`, `per_day`, `stay_total`,
`cleaning_fee`, `service_fee`, `tax`, `refundable_deposit` and `unknown`.
Qualifiers support `unspecified`, `from`, `original`, `discounted` and `unknown`.
Comparable acceptance still requires `nightly` and `unspecified`; other
interpretations can be retained on rejected evidence. Conflicting NaiveFinder
evidence needs a newly staged `conflict_resolution` when accepting a revision.
This is separate from the required audit reason.

- **Arrows / h j k l:** select a cell. Page Up/Down move ten rows; Home/End go
  to the first/last row. The view scrolls to keep the selection visible.
- **Enter:** edit the selected context/decision cell. Backspace removes a UTF-8
  scalar. Enter stages the value; Escape cancels the text editor.
- **n:** edit the required reason for the next review revision.
- **s / Ctrl-S:** validate and save the selected row as a new review revision.
- **/:** filter across displayed fields (ASCII case-insensitive, other UTF-8
  bytes matched exactly). Filtering does not change stored observations.
- **o:** sort by the selected column; repeat to reverse. Stable ID tie-breaking
  keeps equivalent rows deterministic.
- **r:** refresh from the current database snapshot.
- **d:** explicitly discard staged edits and the pending reason.
- **q:** quit. With staged changes, the first quit warns; a second quit discards
  them. EOF/termination also closes without saving.

Input is validated as UTF-8 and cannot contain terminal control characters.
Editor overflow/invalid paste is reported, never silently truncated or committed.
Enter refuses rejected input until Backspace explicitly corrects it or Escape
cancels. Finish or cancel an active editor before a scripted save, selection,
sort or other model action. Unsubmitted editor text triggers a first-quit warning.
- **?:** show the key summary in the terminal status area.

The GUI supports cell selection by mouse and Save/Reason/Filter/Refresh/Discard/
Quit toolbar buttons. Keyboard entry shares the same editor/controller as the
TUI. Columns beyond the window are available with left/right navigation.
Rendering is bounded to 25x8 graphical cells per frame, below ImmGTK's 256
interactive-widget limit. Cell identities derive from observation IDs rather
than screen positions. The TUI restores termios, cursor, bracketed-paste and
alternate-screen modes on normal exit, input failure and handled termination.

## Staging, validation and concurrency

Only one row can have staged edits at a time. Moving to another row, sorting,
filtering or refreshing while dirty is refused; save or explicitly discard
first. Column navigation remains available. A reason is required even when
changing only the decision or adding an audit note.

Saving reacquires the ledger writer lock, loads the expected immutable review,
applies typed context, runs the existing On1x admission policy and appends a new
review revision. It does not overwrite a capture or an older review. Accepted
observations still require a comparable amount, currency/scale and stay context;
staging `unknown` tax/fee scope does not bypass admission. Rejecting incomplete
context remains allowed. Correcting context can make a previously assigned
cohort incompatible; cohort export continues to reject that inconsistency.

No database lock is held while the user navigates, types or waits. If another
writer advances the head, save returns a stale-review conflict. The original
snapshot, pending cells and reason remain available. Explicitly discard and
refresh before rebasing; no automatic merge silently replaces another review.
An uncertain commit is not automatically retried; inspect the current ledger
before deciding what to do next.

After a known successful commit, failed refresh sets `refresh_required:true`:
the old snapshot remains inspectable but no new edit/save can use it. Successful
refresh is required before editing resumes. Published changes are not described
as an unsaved draft. Admission reports the first unresolved field, retaining
staged edits on failure.

Configured extensions receive `worksheet.opened`, committed
`observation.reviewed` and `worksheet.saved` events outside the ledger lock.
The UI does not give an extension authority to edit cells or approve evidence.
Extension logs/diagnostics are separate from worksheet data.

## Scripted input and rendering

Both frontends accept explicit, bounded event scripts for offline testing and
automation. A script is **not** Lua and cannot execute shell commands.

```json
{
  "schema": "shylock.ui-events/v1",
  "events": [
    {"action": "select", "row": 0, "column": "guests"},
    {"action": "edit", "text": "3"},
    {"action": "reason", "text": "Guest count checked against original offer"},
    {"action": "save"},
    {"action": "quit"}
  ]
}
```

```sh
./shylock tui prices.db cottages-eur alice events.json
./shylock gui prices.db cottages-eur alice events.json frame.ppm
```

Scripts run without a terminal/display. They use the actual MuLIB renderers and
the same On1x save path, not an alternative database writer. Row indexes are
zero-based positions in the currently filtered/sorted snapshot; returned row
IDs identify the stable observations. A maximum of 4096 events and 2 MiB of
script input is accepted. File-backed scripts may publish revisions only with
explicit `save` events and reasons. At EOF, unsaved pending values are reported,
never automatically committed.

The complete root and **every** event are admitted by On1x before opening the
ledger or executing the first action. Objects are closed: unknown fields,
internal actions (`fatal`, `end`, `stop`), bad types, invalid/control text and
backend-incompatible geometry fail with exit 2 and no worksheet actions. The
adapter executes the retained inspected tree, not a reopened mutable script file.
Domain/stale-writer/I/O failures remain per-action failures: a valid script is
not a multi-save database transaction.

Actions include navigation names (`up`, `down`, `left`, `right`, `page_up`,
`page_down`, `home`, `last`), `select`, `edit`, `reason`, `save`, `discard`,
`refresh`, `filter`, `sort`, `quit`, and `key` with a key name and optional UTF-8
`text`. `resize` takes width/height (TUI: 40..240 x 12..80; GUI: 640..1920 x
400..1200). GUI `pointer` takes x/y and a Boolean `down`, exercising actual
ImmGTK press/capture/release interactions.

The final JSON contains column metadata, published rows, selection, filter/sort,
pending edits, action statuses and renderer/frame digests. Exit 2 means at least
one scripted action or rendering operation failed; the action list explains
which. Normal interactive quit exits 0 after displaying any operation errors.
The optional GUI frame path writes a P6 PPM image only when explicitly supplied.
No default screenshot or workbook file is written.

The additive `editor` object contains `active`, `prompt`, unsubmitted `text` and
`input_rejected`, so failures do not conceal the retained input buffer. `sheet`
snapshots and the final row list remain published values, not staged/editor data.

`make test` covers frontend parity, read-only evidence, schema/type failures,
UTF-8 input, rendering and resizing, pointer capture, append-only saves, hooks,
stale writers, real PTY input/restoration, and SDL dummy-driver window creation
when SDL2 was enabled. This is not a claim of testing every desktop/compositor.

Typed staging, source-meaning invariants, script admission and review outcomes
live in On1x (`src/worksheet_policy.on1x`, `src/worksheet.on1x` and
`src/poindexter.on1x`). C11 owns snapshots, JSON scalars and frontend resources.
The completion patch is approximately 41% C11 / 59% On1x (107/154 nonblank,
noncomment implementation lines), excluding headers, imports, FFI declarations,
tests, documentation and generated/vendor code. No new dependency or ledger
migration is required; valid documented v1 event scripts remain usable.
