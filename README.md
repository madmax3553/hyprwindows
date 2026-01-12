# hyprwindows

A lightweight TUI and CLI tool for managing Hyprland window rules.

## Features

- **TUI**: Interactive ncurses interface for browsing and editing rules
- **Auto-detection**: Automatically finds your windowrules config from hyprland.conf
- **Rule Summarizer**: Group and display rules by tag
- **Dotfile Scanner**: Detect apps from your dotfiles and find missing rules
- **Active Window Inspector**: See which rules match currently open windows
- **Rule Editor**: Create new rule snippets interactively

## Installation

```bash
make
sudo make install
```

Requires: `clang` or `gcc`, `ncurses`, `hyprctl` (Hyprland)

## Usage

```bash
# Launch TUI (default)
hyprwindows

# Summarize rules by group (auto-detects config)
hyprwindows summarize

# Summarize with explicit path
hyprwindows summarize ~/.config/hypr/windowrules.conf

# Scan dotfiles for apps missing rules
hyprwindows scan-dotfiles ~/dotfiles

# Show active windows and matching rules
hyprwindows active
```

## Rule Format

Rules use Hyprland's native config format:

```
windowrule {
    name = float-pavucontrol
    match:class = ^pavucontrol$
    tag = +settings
    float = yes
    center = yes
    size = 800 600
}

windowrule {
    name = browser-workspace
    match:class = ^(firefox|chromium)$
    tag = +browser
    workspace = 2
}
```

### Match Fields

| Field | Description |
|-------|-------------|
| `match:class` | Regex to match window class |
| `match:title` | Regex to match window title |
| `match:initialClass` | Regex to match initial window class |
| `match:initialTitle` | Regex to match initial window title |
| `match:tag` | Match windows with specific tag |

### Action Fields

| Field | Description |
|-------|-------------|
| `tag` | Assign tag to window (prefix `+` for grouping) |
| `workspace` | Move to workspace |
| `float` | Enable floating (`yes`/`no`) |
| `center` | Center window (`yes`/`no`) |
| `size` | Set size (`width height`) |
| `move` | Set position (`x y`) |
| `opacity` | Set opacity |

## Auto-detection

The tool automatically finds your window rules by:
1. Reading `~/.config/hypr/hyprland.conf`
2. Scanning `source = <path>` lines
3. Finding files containing `windowrule` blocks

## Appmap

The `data/appmap.json` file maps dotfile directory names to window classes, enabling the dotfile scanner to detect which apps you use.

## License

MIT
