# Agent Guidelines for hyprwindows

## Project Overview

`hyprwindows` is a small C utility for managing Hyprland window rules. It provides a TUI (notcurses) for viewing, editing, and saving window rules defined in Hyprland config format. It can also check active windows against rules and find missing rules via an appmap.

**Philosophy:** This is a simple utility — don't over-engineer it. Keep it lean.

## Build

Unity build — the entire project compiles as a single translation unit:

```bash
make            # builds ./hyprwindows
make clean      # removes the binary
make debug      # builds with -g -O0 -DDEBUG
```

The build is just: `cc -Wall -Wextra -Werror -O2 $(pkg-config --cflags notcurses-core) -o hyprwindows unity.c $(pkg-config --libs notcurses-core)`

## Usage

```bash
./hyprwindows          # Launch TUI (default)
./hyprwindows --tui    # Launch TUI (explicit)
./hyprwindows --help   # Show help
```

There is no CLI mode — all interaction is through the TUI.

## File Structure

```
unity.c              # Unity build entry — #includes all src/*.c files
Makefile             # Single-target build
data/appmap.json     # App-to-window-class mapping (378 lines)
src/
  main.c       (31 lines)   Entry point, arg parsing
  util.c/h     (136 lines)  regex_match, read_file, expand_home, regex cache
  rules.c/h    (197 lines)  Rule data model, load/save, rule_write, rule_copy
  hyprconf.c/h (321 lines)  Hyprland config file parser (windowrule blocks)
  hyprctl.c/h  (203 lines)  IPC with hyprctl — reads active window list
  appmap.c/h   (187 lines)  Parses appmap.json (inline string extraction, no JSON lib)
  history.c/h  (99 lines)   Undo/redo stack for rule edits (supports edit + delete change types)
  actions.c/h  (159 lines)  rule_matches_client, find_missing_rules
  ui.c/h       (3322 lines) notcurses TUI — rules view, windows view, review view, modals, sort
```

Total: ~4,655 lines of C across 9 .c files, 8 .h files.

## Architecture

All source files are compiled as one translation unit via `unity.c`. There are no external dependencies beyond libc and notcurses-core. JSON parsing (for hyprctl output and appmap.json) is done with inline string extraction — no JSON library.

**Dependency flow:**
```
main.c → ui.c → actions.c → { rules.c, hyprctl.c, appmap.c, history.c }
                             rules.c → hyprconf.c
                             everything → util.c
```

## Key Data Structures

- `struct rule` / `struct ruleset` — window rule with match patterns (regex) and actions (workspace, tag, float, etc.)
- `struct client` / `struct clients` — active Hyprland windows from `hyprctl clients -j`
- `struct appmap` / `struct appmap_entry` — maps dotfile/package names to window class names
- `struct history_stack` / `struct change_record` — undo/redo with full rule snapshots, supports `CHANGE_EDIT` and `CHANGE_DELETE` types

## Key APIs

- `ruleset_load(path, &ruleset)` — loads rules from hyprland config file
- `rule_write(FILE*, rule)` — writes a rule back in hyprland format
- `hyprctl_clients(&clients)` — gets active windows via hyprctl IPC
- `rule_matches_client(rule, client)` — regex matching of rule against window
- `history_record(stack, type, index, old, new, description)` — record a change (CHANGE_EDIT or CHANGE_DELETE)
- `history_undo(stack, &out_index, &out_type)` / `history_redo(...)` — returns restored rule and change type
- `run_tui()` — launches the notcurses TUI

## Conventions

- **Naming:** `snake_case` for everything. `UPPER_SNAKE` for macros.
- **Structs:** `struct name`, never typedef'd.
- **Static:** Internal functions are `static`. All static names are unique across files (required for unity build).
- **Memory:** Manual malloc/free. Every struct with allocations has a corresponding `_free` function.
- **Errors:** Return 0 for success, -1 for failure. Errors go to stderr.

## Gotchas

- **Unity build:** All .c files share one translation unit. Static function/variable names must be globally unique across all files. Currently no conflicts exist.
- **No JSON library:** hyprctl.c and appmap.c parse JSON with simple string scanning. This works because the JSON structures are predictable with known keys. Don't try to generalize it.
- **Regex caching:** util.c maintains a 64-entry compiled regex cache (`regex_cache`). Regexes are compiled once and reused.
- **Rule format:** Rules are in Hyprland's `windowrule { ... }` block format, not JSON. The parser is in hyprconf.c.

## Next Steps (Planned)

### Improve visuals and refactor for simplicity and performance

**Visuals (remaining):**
- Polish color scheme and contrast across all views (especially selected/highlighted rows)
- Cleaner status bar styling (truncation, key highlighting)

**Done (visuals):**
- ~~Popup layouts~~ — `popup_center()` / `popup_draw()` extracted for consistent sizing
- ~~Review view section separation~~ — section headers with `── Unused Rules (N)` / `── Missing Rules (N)` dividers
- ~~Tab bar~~ — dynamic click boundaries computed from drawn positions
- ~~Summary line~~ — color-coded unused/missing counts

**Refactor (remaining):**
- Deduplicate the unused/missing/window detail popup functions — they follow the same pattern
- Simplify the sort infrastructure (remove global `sort_ctx`, use a wrapper struct)

**Done (refactor):**
- ~~Extract popup boilerplate~~ — `popup_center()`, `popup_draw()`, `draw_scrollbar()` helpers
- ~~Parallel array management~~ — `remove_rule_at()`, `insert_rule_at()`, `append_rule()` helpers
- ~~Delete with history~~ — `delete_rule_with_history()` helper (3 call sites)
- ~~Dead code removal~~ — `outbuf` struct/functions removed, unreachable default case removed
- ~~Redundant update_display_name()~~ — removed from `load_rules()` (already called in `compute_rule_status()`)
- ~~Undo/redo for deletes~~ — fixed with `CHANGE_EDIT`/`CHANGE_DELETE` enum, proper insert/remove on undo/redo
- ~~file_order shift bug~~ — fixed via `remove_rule_at()` helper

**Performance (remaining):**
- Consider lazy review data loading only when the review tab is first visited (currently precomputed)

**Done (performance):**
- ~~Selective rule status recomputation~~ — evaluated, skipped (negligible gain for realistic rule counts, `load_clients` already guarded)
- ~~Redundant update_display_name()~~ — eliminated duplicate call path
