# DSWM Stability Plan

> Base: `4550710` on master. Reference: bspwm at `~/Project/bspwm/src/`.
> Rule: zero warnings (`-O2 -Wall -Wextra`), verify with `make clean && make`.

## Phase A — stability (crashes / data loss)

- [x] A1. Monocle unmaps managed windows → they get unmanaged.
  `layout.c:803` (`dwindle_arrange` monocle branch) calls `XUnmapWindow` on
  non-focused tiled leaves. Managed windows have `StructureNotifyMask`, so
  `handle_unmap_notify` (`wm.c`) → `unmanage_window` removes them from the WM.
  Fix: per-window `monocle_hidden` flag; `handle_unmap_notify` skips flagged
  windows; non-monocle arrange path remaps flagged windows and clears the flag;
  `show_workspace` skips flagged windows when mapping.
  (bspwm ref: `window.c:895-908` visibility guards.)
- [x] A2. `toggle_fullscreen` exit always re-tiles, even for floating windows.
  `wm.c:785-788`: on fullscreen exit it unconditionally calls `tiled_add` +
  `layout_insert`, then restores `is_floating = pre_fs_floating`. A floating
  window that went fullscreen comes back inserted into `tiled[]`/dwindle while
  flagged floating (stale dwindle leaf until next rebuild).
  Fix: gate re-insert on `!pre_fs_floating`.
- [x] A3. Dangling-pointer focus model.
  `ws->focused`, `mouse.win`, `scratch_saved_focus` are raw pointers into
  `wins[]`, which moves on every `realloc`/`memmove`. Store X11 `Window` IDs
  instead + lookup helpers (`find_mw`, `focused_mw`), validate on use.
  (bspwm ref: `locate_window` lookups, never raw pointers across mutations.)
- [x] A4. Focus ignores drag state.
  `handle_enter_notify` has no `mouse.active` guard: hovering another window
  mid-drag re-focuses while `mouse.win` still targets the drag window.
  Fix: early return while `mouse.active`.
  (bspwm ref: `pointer.c` disables focus-follow during grabs.)

## Phase B — focus / stacking

- [x] B1. Keyboard focus can't reach floating windows (`focus_cycle` is tiled-only).
- [x] B2. Raise-on-hover for floating stays in `refocus`; restrict raise to
  explicit focus (key/click/drag), not hover-enter.
  DECIDED: explicit-focus-only. `refocus` no longer raises; explicit raise
  added to focus_cycle, dwindle_focus_cycle, Mod4+click/drag, _NET_ACTIVE_WINDOW,
  manage (born-fullscreen). `window_exists` exported for layout.c.
- [x] B3. `handle_configure_request` only searches `active_ws`; search all workspaces.
- [x] B4. Add `MappingNotify` handler → `grab_keys()` (layout changes break grabs).

## Phase C — spec parity (pick subset)

- [x] C1. `_NET_WM_STATE_BELOW` / layer-below (bspwm `stack.c:123-133`).
- [x] C2. `_NET_CLIENT_LIST` maintenance for pagers/taskbars.
- [x] C3. `_NET_WM_DESKTOP` handling.
- [x] C4. Urgency (`WM_HINTS` / `DEMANDS_ATTENTION`).
- [x] C5. Size hints (`WM_NORMAL_HINTS`) for floating resize.
- [x] C6. `WM_TRANSIENT_FOR` (dialogs above parent).
- [x] C7. Error logging: ignore `BadWindow`, warn on the rest.

## Phase D — simplify

- [x] D1. Scratchpad dim overlay — restored as configurable: `SCRATCHPAD_DIM`
  + `DIM_COLOR` in `dswm.h` (default on). All dim code (ARGB visual, colormap,
  create/destroy, init/cleanup) is `#if`-guarded; both 0/1 branches compile clean.
- [x] D2. Orphaned master-stack layout (`tile_windows_ws` unreachable from `retile_ws`).
- [x] D3. `FIT_WINDOW`/width-factor vs dwindle-resize duality review.
- [x] D4. Fold scattered `XRaiseWindow` loops into one `restack_workspace` helper.

## EWMH completeness (post-D, standalone)

- [x] `_NET_WORKAREA`: per-monitor usable rects via new `monitor_usable_area()`
  export, change-cached, refreshed on every `retile_ws()`, advertised.
- [x] `_NET_FRAME_EXTENTS`: border widths published per window at manage, advertised.
- `_NET_SHOWING_DESKTOP` skipped — no such mode.

## Widget respect (bspwm parity for KyuteWidgets)

- [x] W1. Keep client geometry for positioned floaters — center only when
  the client left the window at the origin (bspwm's rule); widgets and
  popups stay where they asked.
- [x] W2. Honor `_MOTIF_WM_HINTS` decorations=0 → border width 0, stored
  per-window (`borderless`), respected by manage and fullscreen restore.
- [x] W3. Lower WIN_SKIP desktop/dock on map — new `WIN_PINNED` class
  (`XMapWindow` + `XLowerWindow`); splash/notification stay map-only.
- [x] W4. Strut PARTIAL + live invalidation — `atom_net_wm_strut_partial`
  cached, preferred over legacy with root-relative range-vs-monitor
  overlap; legacy falls back using the window rect as span; strut
  `PropertyNotify` invalidates all monitors + retiles. Ranges still
  collapse to full-edge reservation (single usable rect per monitor).

## Layer reset on workspace switch (widget above/sticky burial)

Root cause: `XMapWindow` stacks on top, so showing a workspace buries
anything that stayed mapped (other-ws sticky, unmanaged widgets, pinned
docks), while `raise_above_windows` only reordered the current workspace.
Scroll path verified clean (zero map/unmap/restack calls) — burial happens
at map time and is noticed later.

- [x] L1. Global `restack_visible()` (layout.c): below sinks, current-ws
  layers via `raise_above_windows`, cross-ws sticky/above re-raised,
  tracked overlays re-raised (topmost) / re-lowered (pinned).
- [x] L2. Hooked into every map path: `show_workspace` (visible),
  `dwindle_unhide_all` (only when something unhid), `move_window_to_workspace`,
  `manage_window`. Scratchpad overlay still raised last (stays topmost).
- [x] L3. Overlay tracking for unmanaged widgets: `Overlay` table
  (`MAX_OVERLAYS`), layer intent read from `_NET_WM_STATE` at map,
  pruned on `DestroyNotify`, replaced on re-map.
- [x] L4. Un-sticky fix: unmap only when home workspace isn't visible
  (old loop unmapped the single window globally, and the resulting
  `UnmapNotify` unmanaged it out from under the WM).
- Known tradeoff: overlay-topmost notifications raise above managed
  fullscreen (OSD semantics); stale overlay entries (hidden without
  destroy) make `XRaiseWindow` a harmless no-op until pruned.
- Layer precedence fix: `layer_is_top(above, sticky, below)` =
  `above || (sticky && !below)` — sticky no longer outranks below
  (Kyute desktop preset is sticky+below and must stay down). Applied in
  `raise_above_windows`, `restack_visible`, overlay tracking; below-layer
  windows are never auto-raised (keyboard cycle, dwindle cycle, Mod4+click,
  pager focus requests). Above+below keeps above-wins.

## Log

- 2026-09-23: Plan written. Starting Phase A.
- 2026-09-23: Phase A done. Zero warnings. Not committed per user request.
- 2026-09-23: Phase B partial (B1/B3/B4) done. Zero warnings. B2 pending user decision.
- 2026-09-23: Phase B complete (B2 explicit-focus raise). Zero warnings. Not committed per user request.
- 2026-09-23: Phase C complete (C1-C7). Zero warnings. Not committed per user request.
- 2026-09-23: Phase D complete (D1-D4). Zero warnings. Not committed per user request.
- 2026-09-23: EWMH completeness (workarea + frame extents). Zero warnings. Not committed per user request.
- 2026-09-24: Widget respect W1-W4 (geometry, Motif borders, pinned lower, strut partial). Zero warnings. Not committed per user request.
- 2026-09-24: Layer reset L1-L4 (global restack, overlay tracking, un-sticky fix). Zero warnings. Not committed per user request.
