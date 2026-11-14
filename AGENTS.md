# Agent Guidelines for hyprwindows

## Project Overview

`hyprwindows` is a small C utility for managing Hyprland window rules. It provides a Qt 6 Widgets window for viewing, editing, and saving window rules. It can also check active windows against rules and find missing rules via an appmap.

**Philosophy:** This is a simple utility — don't over-engineer it. Keep it lean.

## Build

Unity build — the entire project compiles as a single translation unit:

```bash
make            # builds ./hyprwindows
make clean      # removes the binary
make debug      # builds with -g -O0 -DDEBUG
make test       # parser/match roundtrip tests, no Qt
```

The C core is still one translation unit. The window is a separate C++ file. This Qt build needs `-fPIE` and `-mno-direct-extern-access` (the installed Qt 6 uses `no_direct_extern_access`). Tests stay C and do not link Qt.

```bash
cc -Wall -Wextra -Werror -O2 -fPIE -c -o unity.o unity.c
moc -o src/window.moc src/window.cpp
g++ -Wall -Wextra -Werror -O2 -fPIE -mno-direct-extern-access $(pkg-config --cflags Qt6Widgets) -c -o src/window.o src/window.cpp
g++ -o hyprwindows unity.o src/window.o $(pkg-config --libs Qt6Widgets)
```

## Usage

```bash
./hyprwindows          # Open the window
./hyprwindows --help   # Show help
```

There is no CLI mode. The window is the interface.

## File Structure

```
unity.c              # Unity build entry — #includes all src/*.c files
Makefile             # Single-target build
data/appmap.json     # App-to-window-class mapping
doc/hyprwindows.1    # Man page
tests/roundtrip.c    # Parser, save-safety, and match tests
src/
  main.c             Entry point, arg parsing
  util.c/h           regex_match, read_file, expand_home, json_unescape
  rules.c/h          Rule data model, load/save, rule_write, rule_match_equal
  hyprconf.c/h       Lua hl.window_rule() parser, rules-only check
  hyprctl.c/h        IPC with hyprctl — reads active window list
  appmap.c/h         Parses appmap.json (inline string extraction, no JSON lib)
  history.c/h        Undo/redo stack (CHANGE_EDIT and CHANGE_DELETE, plus file_order)
  actions.c/h        rule_matches_client, find_missing_rules, cached pacman check
  window.cpp     Qt 6 Widgets window — rules, windows, review, pipeline, actions
  ui.h           Declares run_gui()
```

## Architecture

The C sources are compiled as one translation unit via `unity.c`. The window is `src/window.cpp` and is not part of that unit. Dependencies are libc and Qt 6 Widgets. JSON parsing (for hyprctl output and appmap.json) is done with inline string extraction — no JSON library.

**Dependency flow:**
```
main.c → window.cpp → actions.c → { rules.c, hyprctl.c, appmap.c, history.c }
                             rules.c → hyprconf.c
                             everything → util.c
```

Views: Rules, Windows, Review, Pipeline, Actions.

## Key Data Structures

- `struct rule` / `struct ruleset` — window rule with match patterns (regex) and actions (workspace, tag, float, etc.)
- `struct client` / `struct clients` — active Hyprland windows from `hyprctl clients -j`
- `struct appmap` / `struct appmap_entry` — maps dotfile/package names to window class names
- `struct history_stack` / `struct change_record` — undo/redo with full rule snapshots and the rule's `file_order`

## Key APIs

- `ruleset_load(path, &ruleset)` — loads rules from a hyprland config file
- `rule_write(FILE*, rule)` — writes a rule back in hyprland format
- `rule_match_equal(a, b)` — duplicate check; compares match fields, not display names
- `hyprconf_file_is_rules_only(path)` — 1 if the file may be fully rewritten
- `hyprctl_clients(&clients)` — gets active windows via hyprctl IPC
- `rule_matches_client(rule, client)` — regex matching of rule against window
- `package_installed(pkg)` / `package_cache_clear()` — cached `pacman -Qi` via fork/exec, no shell
- `find_missing_rules(ruleset, appmap, dotfiles, &out)` — uses the in-memory ruleset
- `history_record(stack, type, index, file_order, old, new, description)` — record a change
- `history_undo(...)` / `history_redo(...)` — returns restored rule, index, type, and file_order
- `run_gui()` — opens the Qt 6 window

## Conventions

- **Naming:** `snake_case` for everything. `UPPER_SNAKE` for macros.
- **Structs:** `struct name`, never typedef'd.
- **Static:** Internal functions are `static`. All static names are unique across files (required for unity build).
- **Memory:** Manual malloc/free. Every struct with allocations has a corresponding `_free` function.
- **Errors:** Return 0 for success, -1 for failure. Errors go to stderr, or into the window status line.

## Gotchas

- **Unity build:** All .c files share one translation unit. Static function/variable names must be globally unique across all files.
- **Save is destructive if you let it be.** `save_rules()` rewrites the whole file as `hl.window_rule()` calls. It refuses unless `hyprconf_file_is_rules_only()` is true, and it writes to a temp file before `rename`. Do not weaken that check. Do not write hyprlang `windowrule { }` blocks.
- **No JSON library:** hyprctl.c and appmap.c parse JSON with simple string scanning. `json_unescape()` handles the escapes those scanners see. Don't try to generalize the parser.
- **Regex caching:** util.c maintains a 64-entry compiled regex cache. Regexes are case-sensitive, matching Hyprland. Do not add `REG_ICASE`.
- **Tag-only rules do not match clients.** hyprctl does not expose tags. A rule whose only match is `match:tag` returns no client match. A rule with no match fields still matches every client (a default rule).
- **Duplicates are match-field equality**, not display-name equality. The merge action uses that.
- **Appmap path:** `data/appmap.json` in the cwd, then `../share/hyprwindows/appmap.json` beside the binary, then the install prefixes. `make install` copies the file to `$(PREFIX)/share/hyprwindows/`.
- **Package scan is cached** for the session and cleared on reload. Do not shell out to `pacman` with `system()`.
- **Rule format:** Rules are Hyprland Lua. Direct calls are `hl.window_rule({ match = { class = "..." } })`. This config keeps them in a `rules = { { ... }, ... }` table and applies them with `hl.window_rule(rule)`. The parser harvests both, including `[[long strings]]`. Save splices that table instead of rewriting the module. It is not a Lua interpreter.

## Next Steps (Planned)

**Visuals (remaining):**
- Polish color scheme and contrast across all views (especially selected/highlighted rows)
- Cleaner status bar styling (truncation, key highlighting)

**Refactor (remaining):**
- Deduplicate the unused/missing/window detail popup functions — they follow the same pattern
- Keep `window.cpp` a window over the existing rule API. Don't grow a second data model in the UI. Don't add a custom palette; the window uses the system Qt style.

**Done:**
- Save refuses mixed configs, writes via temp file + rename, and does not quit on a failed save
- Tag-only rules no longer match every window
- Duplicates compare match fields; merge and bulk rename record history
- Package checks are cached and do not use a shell
- Review uses the in-memory ruleset
- `file_order` is assigned without collisions and restored on undo of delete
- Sort no longer uses a global `sort_ctx`
- `$HOME` source paths expand; unknown `match:*` keys round-trip through extras
- README, man page, and this file describe the program that builds
