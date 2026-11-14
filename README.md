# hyprwindows

A small Qt 6 window for managing Hyprland window rules.

## Features

- Browse, edit, create, and delete window rules in a native window
- See which open windows match which rules
- Review unused rules and apps that have no rule yet
- Pipeline view for default, tag, and override stages
- Rename rules from their class or title, merge identical matches, and reload Hyprland
- Undo/redo, and a save that refuses to rewrite a mixed Hyprland config

## Installation

```bash
make
sudo make install
```

Requires `gcc` or `clang`, `g++`, Qt 6 Widgets, and `hyprctl` (Hyprland). The missing-rule scan uses `pacman` when it is available.

## Usage

```bash
hyprwindows           # open the window
hyprwindows --help
```

There is no CLI mode. The window has five pages: Rules, Windows, Review, Pipeline, and Actions.

| Key | Action |
|-----|--------|
| `Ctrl+S` | Save |
| `Ctrl+N` | New rule |
| `Ctrl+Z` / `Ctrl+Y` | Undo / redo |
| `Ctrl+R` | Reload from disk |
| `Ctrl+F` | Filter the rule list |

## Rule format

Rules use Hyprland's Lua config, not the old hyprlang blocks:

```lua
hl.window_rule({
    name = "float-pavucontrol",
    match = { class = "^(pavucontrol)$" },
    tag = "+settings",
    float = true,
    center = true,
    size = "800 600",
})
```

Save updates a `rules = { ... }` table in place, writing default rules, then tag rules, then overrides. If the file has no such table, save rewrites it only when it contains `hl.window_rule()` calls, blanks, and comments. A mixed `hyprland.lua` is left untouched. Put the rules in their own module, usually `rules.lua`, and `require()` it.

## Config detection

1. Read `~/.config/hypr/hyprland.lua` if it exists
2. Follow `require()` and `dofile()` (`~` and `$HOME` are expanded; `config.rules` becomes `config/rules.lua`)
3. Use the first required file that contains `window_rule`
4. Otherwise use `hyprland.lua` itself if it contains `window_rule`
5. Otherwise look for `rules.lua`, `windowrules.lua`, or `config/rules.lua`

## Appmap

`data/appmap.json` maps dotfile and package names to window classes. The installed copy lives at `/usr/local/share/hyprwindows/appmap.json`. A `data/appmap.json` next to the working directory still wins, so the repo copy is used during development.

## License

MIT
