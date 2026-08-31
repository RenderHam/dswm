# DSWM Migration Plan — C++ → Minimal C (JotaHamWM Pattern)

> **Project:** `~/Project/dswm` — fork `RenderHam/dswm` (orig `xsoder/nwm`) → renamed `dswm`
> **Model:** `~/Project/JotaHamWM` (~1500 LOC, C, `jotahamwm.c:1318` + `jotahamwm.h:116` + `Makefile:38`)
> **Target:** Pure C + headers, flat root, minimal deps, header-only config
> **Status:** Plan stored before any code changes. Update this file after each phase.
> **Author:** JotaHamWM simple/optimize structure applied to DSWM scrollable tiler
> **Date:** 2026-09-01
> **AI Memo:** Keep consistent — update MEMORY section after each phase completion.

---

## 0. Session & Clarified Requirements

**Original DSWM (`~/Project/dswm`):**
- `src/` 11 files, 6008 LOC: `nwm.cpp:2852`, `animations.cpp:947`, `tiling.cpp:493`, `bar.cpp:321`, `systray.cpp:233`, headers `nwm.hpp:221`, `animations.hpp:218` etc
- `Makefile:77` (C++17 `-std=c++17 -O3`, SRC/OBJ, `version` awk, `copy` generated `src/config.hpp`), `flake.nix:100`, `default.nix:62`, `extension/zoomer:405` (GL)
- Heavy deps: `freetype2 fontconfig xft x11 xrandr xinerama Xrender lm` (`Makefile:10-11`)
- OOP: `namespace nwm`, `Base` god-object `nwm.hpp:56` (~60 fields), `Animation` virtual inheritance 10 subclasses `animations.hpp:40`, STL `std::vector<std::string>`, `new/delete` 19 sites, `enum class`, lambdas, `chrono`
- Features: master-stack + horizontal scroll `tiling.cpp:493`, built-in `Xft` bar + `XEMBED` systray, titlebars, animations (9 easings), zoomer OpenGL

**Clarified Answers (User 2026-09-01):**
1. **Drop:** animations, built-in bar+systray, titlebars, zoomer OpenGL ✅
2. **Replace BSP with dswm scrollable tiler:** Keep `tiling.cpp` master-stack + `tile_horizontal` (scroll_offset, scroll_visible, master_factor) — drop JotaHamWM BSP `Node` `jotahamwm.c:21-34`
3. **Strip deps to `x11 xinerama` only:** `JotaHamWM/Makefile:4` `pkg-config --cflags x11 xinerama`, fallback `?=` `Makefile:6-7` → remove `xft freetype fontconfig xrandr Xrender`
4. **Remove Nix:** delete `flake.nix` + `default.nix`, keep simple `make` (`JotaHamWM/README.md:20` `makepkg -sic` vs `make && sudo make install`)
5. **Merge into single `dswm.h`, no widgets:** `src/default-config.hpp:175` (`WIDGET` vector 1-9) + `nwm.hpp` → one header like `jotahamwm.h:116` (defines + `Rule` + `Key` + `WS(n)`), no `WIDGET`
6. **Rename binary `nwm` → `dswm`:** `nwm.desktop:3` `Exec=nwm` → `dswm-session`, `Makefile` `BINDIR`, `PKGBUILD:2` `pkgname=dswm-git`
7. **Keep same flags:** `JotaHamWM/Makefile:9` `-O2 -Wall -Wextra $(X11CFLAGS)`, `CC ?= cc`, `PREFIX ?= /usr/local`

---

## 1. Target Layout (Flat)

```
~/Project/dswm/
  Makefile         # 38-45 lines, like JotaHamWM/Makefile:38
  dswm.h           # ~120-150 lines, like jotahamwm.h:116
  dswm.c           # ~1800-2200 lines (merged nwm.cpp + tiling.cpp minus dropped)
  dswm-session.c   # 39 lines, copy jotahamwm-session.c:39
  dswm.desktop     # 6 lines, from nwm.desktop:6
  LICENSE, README.md, .gitignore, PKGBUILD (optional minimal)
  # REMOVED: src/ (6008 LOC), flake.nix, default.nix, extension/zoomer/
```

**Reference JotaHamWM structure (`/home/kuuki/Project/JotaHamWM:14` entries):**
- No `src/`, binaries in root (`jotahamwm`, `jotahamwm-session`), `make clean` removes `pkg/ src/ *.pkg.tar.zst` (`Makefile:20-24`)

---

## 2. Detailed Phases

### Phase 0 — Baseline Tag (5 min, reversible)
- `git tag cxx-last && git branch cxx-archive` at `1e97b38` (122 commits, `origin/master`)
- Document `Readme.org:78006` checklist, keep `CHANGELOG.md:42` + `LICENSE:MIT`
- Lock current `make && test -x nwm` (`Makefile:40`)

### Phase 1 — Build System (`dswm/Makefile:77` → JotaHamWM:38) — LOW RISK, DO FIRST
- **Before:** `CXXFLAGS -std=c++17 -O3 -Wpedantic -DGIT_VERSION`, `SRC/OBJ/DEPS`, `LDFLAGS` misused for cflags, `GIT_VERSION` shell, `version` awk `Makefile:15-18` rewriting `nwm.hpp:221` `MAJOR/MINOR/PATCH`, `copy` `Makefile:33` `cp default-config.hpp → config.hpp`
- **After:** Like `JotaHamWM/Makefile:1-38`:
  ```make
  PREFIX ?= /usr/local
  CC ?= cc
  X11CFLAGS := $(shell pkg-config --cflags x11 xinerama 2>/dev/null)
  X11LIBS   := $(shell pkg-config --libs x11 xinerama 2>/dev/null)
  X11CFLAGS ?= -I/usr/include
  X11LIBS   ?= -L/usr/lib -lX11 -lXinerama
  CFLAGS += -O2 -Wall -Wextra $(X11CFLAGS)
  LDFLAGS += $(X11LIBS)
  all: dswm dswm-session
  dswm: dswm.c dswm.h
  	$(CC) $(CFLAGS) -o $@ dswm.c $(LDFLAGS)
  dswm-session: dswm-session.c
  	$(CC) $(CFLAGS) -o $@ dswm-session.c $(LDFLAGS)
  clean: rm -rf dswm dswm-session pkg/ src/ *.pkg.tar.zst
  install: all; install -Dm755 dswm $(DESTDIR)$(PREFIX)/bin/dswm
  ```
- Delete `flake.nix:100`, `default.nix:62`, update `nwm.desktop:3` Exec
- Verify: `make` needs only `cc` + `libx11-dev libxinerama-dev` (`JotaHamWM/.github/workflows/ci.yml:21`)

### Phase 2 — Config Header `src/default-config.hpp:175` → `dswm.h` (1h)
- **Template:** `jotahamwm.h:1-115` defines `NSPACE 9` `BARH 24` `GAP_OUTER 8` `GAP_INNER 4`, `typedef struct {const char *class; int isfloat;} Rule; static Rule rules[]`, `enum {EXEC,VIEW...}`, `union Arg`, `struct Key`, `static Key keys[]`, `WS(n)` macro `jotahamwm.h:51-60` (`VL`, `BR`)
- **Merge:** Keep `NUM_WORKSPACES 9` `nwm.hpp:18`, `SCROLL_WINDOWS_VISIBLE 2` `default-config.hpp:80`, `SCROLL_STEP 550`, `GAP_SIZE 0` → `GAP_OUTER/GAP_INNER`, `BORDER_WIDTH 3` `BORDER_COLOR` `FOCUS_COLOR`, `MODKEY Mod4Mask`, `keys[]` 30 entries `default-config.hpp:110-170` → rewrite to `jotahamwm.h:31-32` `Key` struct. Drop `ANIM_*`, `FONT`, `BAR_*`, `USE_BUILTIN_BAR`, `SHOW_WINDOW_TITLES`, `TITLE_BAR_HEIGHT`, `WIDGET`.
- Remove `using namespace nwm;`, `__attribute__((unused))`, `std::vector<std::string>`

### Phase 3 — Data Structures `nwm.hpp:221` → C `dswm.h` (1h, parallel with Phase 4)
- **C++ `nwm.hpp:56` `Base` god-object (~60 fields: `Display*`, `Window root`, `std::vector<ManagedWindow> windows`, `Monitor monitors`, `Workspace workspaces`, `AnimationManager*`) → C structs:**
  ```c
  typedef struct { Window window; int x,y,w,h; int is_floating,is_fullscreen; int workspace,monitor; char title[256]; } ManagedWindow;
  typedef struct { int id,x,y,w,h; int current_workspace; float master_factor; int horizontal_mode; int scroll_visible; int scroll_offset; } Monitor;
  typedef struct { ManagedWindow *wins; int nwin,cap; ManagedWindow *focused; int scroll_offset; int scroll_maximized; } Workspace;
  // globals like jotahamwm.c:60-88: Display *dpy; Window root; Monitor mons[8]; Workspace spaces[9];
  ```
- Replace `std::vector` → `*wins, nwin, cap` + `realloc` helper (like `jotahamwm.c:99` `mkleaf` `calloc`). `std::string title` → `char[256]` via `XGetWindowProperty` like `jotahamwm.c:762`.

### Phase 4 — Tiling Core `tiling.cpp:493` → `dswm.c` (2-3 days, CRITICAL PATH)
- **Keep DSWM tiler, drop BSP:** Do NOT port JotaHamWM `Node` `jotahamwm.c:21-34` (BSP tree `leaf, horiz, ratio, a/b/par, win, x/y/w/h, fx/fy/fw/fh, cx/cy/cw/ch`). Instead port `tiling.cpp`:
  - `tile_horizontal` (scroll `scroll_offset`, `scroll_windows_visible`, `master_factor` 0.1-0.9, `usable_width/scroll_visible`, gaps/borders)
  - `tile_windows` (master-stack `master_width = usable*master_factor`)
  - `move_horizontal`, `scroll_left/right`, `toggle_layout`, `swap_next/prev`, `resize_master`, `increment_scroll_visible` (`tiling.hpp:30`)
- **C translations:** `std::min/max` `algorithm` → `MIN` `jotahamwm.c:15`, `for (auto &w: vec)` → `for (i=0;i<n;i++)`, `std::swap` → tmp, `std::vector::erase` → `memmove`.
- **Integrate `tile()`:** Like `jotahamwm.c:496-552` per-workspace dispatch `if (mon->horizontal_mode) tile_horizontal(mon,ws); else tile_windows(mon,ws);`. Keep `strut` via `_NET_WM_STRUT_PARTIAL` `nwm.cpp:1200` only for external bar (polybar).

### Phase 5 — Drop Features (No Code, Just Deletion)
- Delete `bar.cpp:321` (`BarSegment`, `XftDraw`, `Pixmap` double-buffer), `bar.hpp:48`, `systray.cpp:233` (`XEMBED`, `_NET_SYSTEM_TRAY`, `icons 20x20`), `animations.cpp:947` + `animations.hpp:218` (10 `AnimationType`, 9 `EasingType`, `AnimationManager new/delete` 19 sites, `chrono` `functional`), titlebar `nwm.hpp:12` `TitleBar`, `extension/zoomer/:405` (`GL/glew`).
- Remove deps `-lXft -lXrender -lXrandr -lfreetype -lfontconfig -lm -lGL -lGLEW` (`dswm/Makefile:10-11`). Keep only `x11 xinerama` (`JotaHamWM/Makefile:4`).
- Keep `Xrandr`? No — Q3 stripped → use `XineramaQueryScreens` like `jotahamwm.c:682`.

### Phase 6 — Event Loop `nwm.cpp:2852` → `dswm.c` (1-2 days)
- **Ref:** `jotahamwm.c:665-1318` `XOpenDisplay`, `XSetErrorHandler`, `XSelectInput SubstructureRedirect`, `XGrabKey` 4 mods `LockMask/Mod2Mask` `jotahamwm.c:648`, `while (!XNextEvent)` switch `MapRequest` `DestroyNotify` `ConfigureRequest` `EnterNotify` `ButtonPress` `MotionNotify`.
- Port `nwm.cpp:2800` `init/run/cleanup` + `handle_*` (key/button/motion/map/unmap/enter/destroy/expose/client_message/property), `manage/unmanage`, `focus_next/prev`, `spawn` `fork+setsid+execvp` `jotahamwm.c:1119`, EWMH `_NET_ACTIVE_WINDOW` `jotahamwm.c:635`. Replace `std::cout` → `fprintf(stderr)`, `std::find_if` lambda → loop.

### Phase 7 — Session/Desktop
- Add `dswm-session.c` copy `jotahamwm-session.c:1-39` → `s/jotahamwm/dswm/` (`getenv HOME`, `mkdir ~/Pictures`, `QT_/GDK_` env, `fork+exec ~/.config/dswm/autostart.sh` fallback, `exec dswm`).
- Update `nwm.desktop:1-6` → `dswm.desktop` `Exec=dswm-session`, `Makefile install` to `$(PREFIX)/share/xsessions`.

### Phase 8 — Flatten & Docs (Final)
- `git mv src/* .` delete `src/`, update `.gitignore:103` `**/*.o` `nwm` `src/config.hpp` → `JotaHamWM/.gitignore:71-73` `/src/ /pkg/ *.pkg.tar.zst` + `dswm`
- Replace `Readme.org:78006` Org with `README.md` minimal `JotaHamWM/README.md:121` (deps `Xlib`, `make`, `sudo make install`, `make clean`, config via `dswm.h`, keybinds)
- Update `PKGBUILD:1-34` if keeping: `pkgname=dswm-git`, `depends=('libx11' 'libxinerama')`, `source=("$pkgname::git+file://${startdir}")`, fixed `pkgver()` `if desc=$(git describe)` `JotaHamWM/PKGBUILD:17-24`

---

## 3. Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| STL `std::vector` → `realloc` off-by-one | Helper `vec_push` + Xephyr test `Readme.org:70` `Xephyr :5` |
| Inheritance flatten `Animation` virtual `update` `animations.hpp:40` | Drop animations (all `ANIM_*_ENABLED false` except scroll) → no virtual needed |
| Xft drop loses anti-aliased `FONT` `default-config.hpp:80` | Accept (no bar) or recommend external polybar `JotaHamWM/README.md:103` |
| Multi-monitor hotplug `Xrandr` `nwm.cpp:800` → `Xinerama` only `jotahamwm.c:682` | Static setups fine; else keep `Xrandr` optionally (1 extra lib) |
| Config breakage `src/config.hpp` generated vs `dswm.h` | Provide `sed` migration for `#define` |

---

## 4. Verification (Like `JotaHamWM/.github/workflows/ci.yml:24-27`)

- `make && test -x dswm && test -x dswm-session`
- `Xephyr :5 -screen 1280x720 & DISPLAY=:5 ./dswm` + `DISPLAY=:5 kitty &` scroll test `MODKEY+Left/Right`
- `valgrind --leak-check=full ./dswm` (was `flake.nix:45` devShell) for `malloc/free`
- `wc -l dswm.*` ~2000 vs 6008 before; `pkg-config --libs` only `x11 xinerama`
- CI: adapt `JotaHamWM/.github/workflows/ci.yml:21` `apt-get libx11-dev libxinerama-dev` + `make`

---

## 5. Implementation Order (Agreed)

1. Phase 1 Makefile (30 min, low risk)
2. Phase 2 `dswm.h` (1h)
3. Phase 4 Tiling C port (core, 2-3 days) — before bar
4. Phase 3 structs + Phase 6 event loop (parallel)
5. Phase 7 session/desktop + Phase 8 cleanup

---

## 6. MEMORY — Phase Completion Log (Update After Each Phase)

> **Instruction:** After finishing each phase, append entry here with date, changes, verification, next. Keep file as single source of truth for AI + human.

### Phase 0 — Plan Stored
- **Date:** 2026-09-01
- **Status:** No code changes yet per user request. Plan written to `~/Project/dswm/.agents/plans/plan.md` (this file). Also empty `~/Project/dswm/.agents/plan.md` exists (legacy).
- **Next:** User to approve build mode; then Phase 1 Makefile.

### Phase 1 — Makefile Simplification
- **Date:** 2026-09-01 (completed)
- **Changes Made:**
  - `Makefile:1-38` replaced 77-line C++ build with JotaHamWM-style C build: `CC ?= cc`, `pkg-config --cflags --libs x11 xinerama`, `CFLAGS += -O2 -Wall -Wextra`, single-step compile `$(CC) $(CFLAGS) -o $@ dswm.c $(LDFLAGS)`, targets `all/dswm/dswm-session/clean/install/uninstall`, `PREFIX ?= /usr/local`, `DESTDIR` staging
  - Deleted `flake.nix:100` (Nix flake devShell, multi-input) + `default.nix:62` (Nix derivation, `pname=nwm`, `buildInputs=[libX11 libXft libXrender libXrandr libXinerama freetype fontconfig]`) — 162 lines removed
  - `nwm.desktop:6` deleted, replaced with `dswm.desktop:6` (`Exec=dswm-session`, `Name=DSWM`, `DesktopNames=DSWM`)
  - `.gitignore:10` → 75 lines: JotaHamWM-style flat patterns (`*.o`, `*.so`, `dswm`, `dswm-session`, `/src/ /pkg/ *.pkg.tar.zst`), removed `src/config.hpp` generation, `src/extension/zoomer/zoomer`
- **git diff:** `.gitignore` +75/-10, `Makefile` -77+38 (net -39), `default.nix` -62, `flake.nix` -100, `nwm.desktop` -6 → total **-243 lines** removed
- **Verification:** Build system references `dswm.c` + `dswm.h` (will exist Phase 2+). `make` will work after Phase 2 creates `dswm.c`. `pkg-config` now targets only `x11 xinerama` (stripped 5 deps: `freetype2 fontconfig xft xrandr Xrender`)
- **Dependencies Remaining:** `libx11-dev libxinerama-dev` only (Arch: `pacman -S libx11 libxinerama`)
- **Next:** Phase 2 Config Header `dswm.h` — merge `src/default-config.hpp:175` + `src/nwm.hpp:12-30` into single C header like `jotahamwm.h:116`

### Phase 2 — Config Header `dswm.h`
- **Date:** 2026-09-01 (completed)
- **Changes Made:**
  - Created `dswm.h:1-148` — single C header, `#include <X11/Xlib.h>` + `<X11/keysym.h>`, header guard `#ifndef DSWM_H`
  - Merged `src/default-config.hpp:175` + `src/nwm.hpp:12-20` → defines: `NUM_WORKSPACES 9`, `BORDER_WIDTH 3`, `BORDER_COLOR 0x181818`, `FOCUS_COLOR 0x005577`, `GAP_OUTER 0`, `GAP_INNER 0` (was single `GAP_SIZE 0`), `SCROLL_WINDOWS_VISIBLE 2`, `SCROLL_STEP 550`, `RESIZE_STEP 60`, `BAR_POSITION 0`, `BAR_HEIGHT 25`, `USE_XINERAMA 1`
  - `Rule` struct + `static Rule rules[]` (4 entries: `pavucontrol`, `rofi`, `steam`, `steamwebhelper`)
  - 21-action enum (`SPAWN`..`MOVE_TO_WORKSPACE`) replacing C++ function pointer dispatch
  - `Arg` union `{ int i; void *v; }` + `Key` struct `{ mod, sym, act, arg }` — replaces C++ `std::function<void*,Base&>` pattern
  - `WS(n)` macro for workspace keybinds (doubles to `SWITCH_WORKSPACE` + `MOVE_TO_WORKSPACE`)
  - 30 `static Key keys[]` entries: launch, close, quit, focus, swap, resize, scroll, toggles, monitor, scroll_visible, workspaces
  - Dropped: all `ANIM_*` (39 lines), `WIDGET` vector, `FONT`, `BAR_*` colors (7 lines), `TITLE_BAR_*` (6 lines), `SHOW_WINDOW_TITLES`, `using namespace nwm;`, `__attribute__((unused))`
  - Fixed const-qualifier warning: `static int` (not `const int`) for workspace/monitor/scroll_visible vars
- **Verification:** `echo '#include "dswm.h"' | cc -xc -fsyntax-only -std=c99 -Wall -Wextra -` → clean (0 warnings, 0 errors)
- **Deps required:** `libx11-dev` (Xlib.h, keysym.h) — no Xft, no Xrandr, no Xinerama headers in config
- **Next:** Phase 4 Tiling Core (`src/tiling.cpp:493` → `dswm.c` scrollable tiler, C style). Phase 3 (data structures) and Phase 6 (event loop) parallel after.

### Phase 3 — Data Structures
- **Date:** pending
- **Expected Changes:** C structs for `ManagedWindow`, `Monitor`, `Workspace`, globals
- **Verification:** `grep -r "std::" src/` → 0 hits
- **Notes:**

### Phase 4 — Tiling Core (Scrollable)
- **Date:** 2026-09-01 (completed)
- **Changes Made:**
  - Created `dswm.c:1-1051` — full C WM: data structures, tiling, window management, event loop, main
  - **Data structures:** `ManagedWindow` (Window, geom, flags, pre_fs_*, no std::string), `Monitor` (id, x/y/w/h, master_factor, horizontal_mode, scroll_windows_visible), `Workspace` (wins array, nwin, cap, focused, scroll_offset, scroll_maximized) — replaces `nwm.hpp:221` `Base` god-object + `std::vector`
  - **Globals:** `dpy`, `root`, `screen`, `scrw`, `scrh`, `running`, `cur_ws`, `mons[8]`, `spaces[9]` — replaces C++ `Base` reference passing
  - **Tiling functions ported from `src/tiling.cpp:493`:**
    - `tile_horizontal()` — scroll layout: collects tiled windows per monitor, computes usable space (BAR_POSITION, strut TODO), positions with scroll_offset, scroll_visible, master_factor, gaps/borders
    - `tile_windows()` — master-stack: single window fill, master+stack split with stack_h calculation
    - `resize_master()` — adjusts master_factor based on delta, horizontal mode (0.3-3.0) vs master-stack (0.1-0.9)
    - `move_horizontal()` — scroll offset forward/backward with scroll_amount calculation
    - `scroll_left()`, `scroll_right()` — wrappers around move_horizontal
    - `toggle_layout()` — flip horizontal_mode, reset scroll_offset, adjust master_factor (1.0 vs 0.5)
    - `swap_next()`, `swap_prev()` — manual swap with tmp, retile + focus
    - `increment_scroll_visible()`, `decrement_scroll_visible()` — clamp 1..10
  - **Window management:** `manage_window()`, `unmanage_window()` with realloc growth (cap doubling, 16 initial)
  - **Focus:** `focus_next()`, `focus_prev()` with XSetInputFocus + XRaiseWindow
  - **Toggle states:** `toggle_fullscreen()` (save/restore pre_fs_*, EWMH), `toggle_float()` (center)
  - **Close/quit:** `close_window()` (WM_DELETE_WINDOW ClientMessage), `quit_wm()`
  - **EWMH:** `setup_ewmh()` (_NET_SUPPORTED, _NET_NUMBER_OF_DESKTOPS, _NET_CURRENT_DESKTOP)
  - **Event loop:** `init()` (XOpenDisplay, XSetErrorHandler, grab_keys, XSelectInput), `run()` (switch on MapRequest/DestroyNotify/UnmapNotify/ConfigureRequest/EnterNotify/KeyPress/ButtonPress), `cleanup()`
  - **C translations:** `std::vector` → `ManagedWindow *wins, nwin, cap` + `realloc`, `std::min/max` → `MIN/MAX`, `for (auto &w: vec)` → `for (i=0; i<count; i++)`, `bool` → `int`, `size_t` → `int`
  - Created `dswm-session.c:1-32` — session wrapper (env vars, autostart.sh fork, exec dswm)
  - Added `#include <X11/extensions/Xinerama.h>` for monitor detection, `monitors_init()` with Xinerama fallback
  - Dropped: all bar/titlebar references, animation callbacks, `bar_draw()`, `titlebar_draw()`
- **Verification:** `make clean && make` → **0 warnings, 0 errors**. Binaries: `dswm` 31K ELF, `dswm-session` 17K ELF. `pkg-config --libs` → `-lXinerama -lX11` only (2 libs)
- **LOC:** `dswm.c` 1051 + `dswm.h` 142 + `dswm-session.c` 32 = **1225 lines** vs original 6008 (80% reduction)
- **Next:** Phase 5 Feature Deletion (remove old `src/` files from git), Phase 6 Event Loop enhancements (monitor focus, drag, bar struts), Phase 7 cleanup (flatten, docs). Or commit current state first.

### Phase 5 — Feature Deletion
- **Date:** pending
- **Expected Changes:** Remove `bar/`, `systray`, `animations`, `zoomer`
- **Verification:** `pkg-config --libs` only `x11 xinerama`, `wc -l` drop 6008→~2000
- **Notes:**

### Phase 6 — Event Loop
- **Date:** pending
- **Expected Changes:** `nwm.cpp` handlers → C `dswm.c` switch
- **Verification:** `Xephyr` all keys `default-config.hpp:110-170` work
- **Notes:**

### Phase 7 — Session/Desktop & Flatten
- **Date:** pending
- **Expected Changes:** `dswm-session.c`, `dswm.desktop`, flat root, `README.md`
- **Verification:** `make install` `DESTDIR` + display manager login
- **Notes:**

---

## 7. References (Absolute Paths)

- `JotaHamWM/Makefile:1-38`, `jotahamwm.h:1-116`, `jotahamwm.c:1-1318`, `jotahamwm-session.c:1-39`, `PKGBUILD:1-34`, `.gitignore:1-73`, `.github/workflows/ci.yml:1-149`
- `dswm/Makefile:1-77`, `src/nwm.hpp:1-221`, `src/nwm.cpp:1-2852`, `src/tiling.cpp:1-493`, `src/bar.cpp:1-321`, `src/systray.cpp:1-233`, `src/animations.cpp:1-947`, `src/default-config.hpp:1-175`, `nwm.desktop:1-6`, `flake.nix:1-100`, `default.nix:1-62`
- Git: `RenderHam/dswm` HEAD `1e97b38`, `RenderHam/JotaHamWM` archived

---

## 8. Session Log

- **2026-09-01 00:33 UTC:** User archived JotaHamWM, shifted to dswm. Plan explored via `Task` agents `ses_fa75...` + `ses_fa75...`. User clarified 7 answers (drop features, keep scroll tiler, minimal deps, no Nix, single header, rename `dswm`, keep flags). User requested no changes yet, store plan to `~/Project/dswm/.agents/plans/plan.md` and update after each phase. Mode switched `plan→build`, file created here.
- **2026-09-01 01:00 UTC:** Phase 1 completed. Build system simplified: 77-line C++ Makefile → 38-line C Makefile (`CC ?= cc`, `pkg-config x11 xinerama`), deleted `flake.nix`+`default.nix` (162 lines), renamed `nwm.desktop`→`dswm.desktop` (`Exec=dswm-session`), updated `.gitignore`. Build targets `dswm.c`+`dswm.h` ready. Next: Phase 2 config header `dswm.h`.
- **2026-09-01 01:10 UTC:** Phase 2 completed. Created `dswm.h:1-148` — single C header merging `src/default-config.hpp:175` + `src/nwm.hpp:12-20` into JotaHamWM pattern. Defines `NUM_WORKSPACES 9`, border/gap/scroll constants, `Rule` struct, 21-action enum, `Arg` union + `Key` struct, `WS(n)` macro, 30 keybinds. Dropped all `ANIM_*` (39 lines), `WIDGET` vector, `FONT`, bar/titlebar colors, C++ syntax. Compiles clean: `cc -xc -fsyntax-only -std=c99 -Wall -Wextra` → 0 warnings. Next: Phase 4 tiling core port.
- **2026-09-01 01:20 UTC:** Phase 4 completed. Created `dswm.c:1-1051` (full C WM) + `dswm-session.c:1-32`. Ported from `src/tiling.cpp:493` + `nwm.hpp:221`: C structs (`ManagedWindow`, `Monitor`, `Workspace`), 12 tiling functions (tile_horizontal, tile_windows, resize_master, move_horizontal, scroll_left/right, toggle_layout, swap_next/prev, increment/decrement_scroll_visible), window management (manage/unmanage), focus, toggle states, close/quit, EWMH, event loop, main. `make clean && make` → **0 warnings, 0 errors**. Binaries: `dswm` 31K, `dswm-session` 17K. 2 libs only (`xinerama x11`). 1225 LOC vs 6008 (80% reduction). Next: Phase 5+6+7.
- **2026-09-01 05:40 UTC:** Phase 5-7 completed. Commit `262e532` — C rewrite committed. Commit `e6ebab7` — FOCUS_MONITOR, SET_SCROLL_VISIBLE, bar strut support, README.md. Both pushed to `RenderHam/dswm`. Clean code audit performed: found 3 bugs (rules[] never applied, memmove stale pointer, realloc leak), 25 non-static functions, inconsistent void*/int parameter passing, magic numbers, duplicated code, dead code. Created clean code plan (Phases 8-12).
- **2026-09-01 05:45 UTC:** Clean code refactor started. Created custom skill `.agents/skills/clean-code/SKILL.md`. Phases 8-12: bug fixes → header cleanup → static+const → extract helpers → dispatch cleanup.
- **2026-09-01 06:00 UTC:** Clean code refactor completed (Phases 8-12). Commit `13989f4` — bug fixes (rules[] applied via XGetClassHint, memmove stale pointer fixed, realloc leak fixed), header cleanup (Rule fields renamed, browcmd→browsercmd, INITIAL_CAP/MAX_MONS/MAX_TILED defines). Commit `9a9099d` — added static to all 25 internal functions, const to read-only params. Commit `0f4088d` — extracted compute_usable_area(), merged increment/decrement→adjust_scroll_visible(), removed scroll_left/scroll_right wrappers, removed dead toggle_gap/scroll_maximized, defined named constants. Commit `d901909` — standardized dispatch to int-by-value, removed unused mon0-mon2/sv1-sv5 globals. All pushed to RenderHam/dswm.
- **2026-09-01 07:40 UTC:** File split + workspace fix completed (Phases 13-15). Split dswm.c (1082 LOC) into 6 focused files: dswm.c (173, main/init/run/cleanup), layout.c (377, tiling), workspace.c (354, window mgmt + visibility), events.c (106, event handlers), ewmh.c (42, EWMH), util.c (45, grab/spawn/error). Fixed workspace visibility: added show_workspace() with XUnmapWindow/XMapWindow, switch_workspace() now hides old WS and shows new WS, manage_window() only maps if on active WS, move_to_workspace() unmaps moved window. Total 1367 LOC across 8 files. Build: 0 warnings, 0 errors. Binary: 36K dswm, 17K dswm-session.

