# dswm

A scrollable tiling window manager for X11, rewritten in minimal C.

Forked from [RenderHam/dswm](https://github.com/RenderHam/dswm) (originally xsoder/nwm). Rewritten following the [JotaHamWM](https://github.com/kuuki/JotaHamWM) flat C pattern.

## Features

- **Scrollable tiling**: windows arranged horizontally, scroll through them
- **Master-stack layout**: toggle between scroll and master+stack modes
- **Multi-monitor** via Xinerama
- **9 workspaces**
- **EWMH compliant** (partial)
- **Bar strut support**: respects `_NET_WM_STRUT` from external bars (polybar, etc.)
- **No animations, no built-in bar, no titlebars, no systray**

## Dependencies

- `libX11`
- `libXinerama`
- `pkg-config`

```sh
# Arch
sudo pacman -S libx11 libxinerama pkg-config

# Debian/Ubuntu
sudo apt install libx11-dev libxinerama-dev pkg-config
```

## Build

```sh
make
sudo make install
```

Default `PREFIX=/usr/local`. Override with `make PREFIX=/usr install`.

Binary: `dswm` (31K). Session wrapper: `dswm-session`.

## Configuration

Edit `dswm.h` then rebuild:

```sh
$EDITOR dswm.h
make
```

### Keybindings (default MODKEY = Super)

| Key | Action |
|-----|--------|
| `Return` | Terminal (kitty) |
| `d` | Menu (dmenu_run) |
| `b` | Browser (firefox) |
| `q` | Close window |
| `Shift+q` | Quit WM |
| `j/k` | Focus next/prev |
| `Shift+h/l` | Swap next/prev |
| `h/l` | Resize master |
| `Left/Right` | Scroll windows |
| `t` | Toggle layout (scroll ↔ master-stack) |
| `a` | Toggle gap (placeholder) |
| `f` | Toggle fullscreen |
| `Shift+Space` | Toggle float |
| `,` / `.` / `/` | Focus monitor 0/1/2 |
| `Shift+,` / `.` / `/` | Set scroll visible 2/3/4 |
| `=` / `-` | Increment/decrement scroll visible |
| `1-9` | Switch workspace |
| `Shift+1-9` | Move window to workspace |

### Customization

All config lives in `dswm.h`:

- `NUM_WORKSPACES` — number of workspaces (default 9)
- `BORDER_WIDTH` — window border (default 3)
- `BORDER_COLOR` / `FOCUS_COLOR` — border colors (hex)
- `GAP_OUTER` / `GAP_INNER` — gaps (default 0)
- `SCROLL_WINDOWS_VISIBLE` — windows visible in scroll mode (default 2)
- `SCROLL_STEP` — scroll step (default 550)
- `RESIZE_STEP` — resize step (default 60)
- `rules[]` — window rules (float by class)
- `keys[]` — keybindings

## Session

Use `dswm-session` in your display manager `.desktop` file. It:

1. Sets env vars (Qt, GDK)
2. Forks `~/.config/dswm/autostart.sh` (if exists)
3. Execs `dswm`

## License

See original [RenderHam/dswm](https://github.com/RenderHam/dswm).
