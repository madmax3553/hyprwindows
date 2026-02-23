# Agent Guidelines for hyprwindows

## Project Overview

`hyprwindows` is a small C utility for managing Hyprland window rules. It provides a TUI (notcurses) for viewing, editing, and saving window rules defined in Hyprland config format. It can also check active windows against rules and find missing rules via an appmap.

**Philosophy:** This is a simple utility — don't over-engineer it. The codebase was recently stripped from ~4,600 lines down to ~2,100 lines of C (now ~2,700 after the notcurses TUI rewrite, interactive popups, and review table). Keep it lean.

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
data/appmap.json     # App-to-window-class mapping (379 lines)
src/
  main.c       (31 lines)   Entry point, arg parsing
  util.c/h     (136 lines)  regex_match, read_file, expand_home, regex cache
  rules.c/h    (197 lines)  Rule data model, load/save, rule_write, rule_copy
  hyprconf.c/h (321 lines)  Hyprland config file parser (windowrule blocks)
  hyprctl.c/h  (195 lines)  IPC with hyprctl — reads active window list
  appmap.c/h   (187 lines)  Parses appmap.json (inline string extraction, no JSON lib)
  history.c/h  (92 lines)   Undo/redo stack for rule edits
  actions.c/h  (208 lines)  outbuf, rule_matches_client, find_missing_rules
  ui.c/h       (2578 lines) notcurses TUI — rules view, windows view, review view, modals, sort
```

Total: ~2,800 lines of C across 9 .c files, 8 .h files.

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
- `struct history_stack` / `struct change_record` — undo/redo with full rule snapshots
- `struct outbuf` — growable string buffer for text output

## Key APIs

- `ruleset_load(path, &ruleset)` — loads rules from hyprland config file
- `rule_write(FILE*, rule)` — writes a rule back in hyprland format
- `hyprctl_clients(&clients)` — gets active windows via hyprctl IPC
- `rule_matches_client(rule, client)` — regex matching of rule against window
- `history_record(stack, index, old, new, description)` — record a change
- `history_undo(stack, &out_index)` / `history_redo(stack, &out_index)` — returns restored rule
- `run_tui()` — launches the notcurses TUI

## Conventions

- **Naming:** `snake_case` for everything. `UPPER_SNAKE` for macros.
- **Structs:** `struct name`, never typedef'd.
- **Static:** Internal functions are `static`. All static names are unique across files (required for unity build).
- **Memory:** Manual malloc/free. Every struct with allocations has a corresponding `_free` function.
- **Errors:** Return 0 for success, -1 for failure. Errors go to stderr or outbuf.

## Gotchas

- **Unity build:** All .c files share one translation unit. Static function/variable names must be globally unique across all files. Currently no conflicts exist.
- **No JSON library:** hyprctl.c and appmap.c parse JSON with simple string scanning. This works because the JSON structures are predictable with known keys. Don't try to generalize it.
- **Regex caching:** util.c maintains a 64-entry compiled regex cache (`regex_cache`). Regexes are compiled once and reused.
- **Rule format:** Rules are in Hyprland's `windowrule { ... }` block format, not JSON. The parser is in hyprconf.c.

## Next Steps (Planned)

- Add ability to create new rules from the missing rules popup (pre-fill class pattern from appmap)
- Bulk actions on review items (e.g., dismiss unused warnings, batch-add missing rules)
