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

- [ ] B1. Keyboard focus can't reach floating windows (`focus_cycle` is tiled-only).
- [ ] B2. Raise-on-hover for floating stays in `refocus`; restrict raise to
  explicit focus (key/click/drag), not hover-enter.
  DECISION NEEDED: keep hover-raise vs explicit-focus-only raise.
- [ ] B3. `handle_configure_request` only searches `active_ws`; search all workspaces.
- [ ] B4. Add `MappingNotify` handler → `grab_keys()` (layout changes break grabs).

## Phase C — spec parity (pick subset)

- [ ] C1. `_NET_WM_STATE_BELOW` / layer-below (bspwm `stack.c:123-133`).
- [ ] C2. `_NET_CLIENT_LIST` maintenance for pagers/taskbars.
- [ ] C3. `_NET_WM_DESKTOP` handling.
- [ ] C4. Urgency (`WM_HINTS` / `DEMANDS_ATTENTION`).
- [ ] C5. Size hints (`WM_NORMAL_HINTS`) for floating resize.
- [ ] C6. `WM_TRANSIENT_FOR` (dialogs above parent).
- [ ] C7. Error logging: ignore `BadWindow`, warn on the rest.

## Phase D — simplify

- [ ] D1. Scratchpad dim overlay (ARGB/colormap churn for cosmetic dim).
- [ ] D2. Orphaned master-stack layout (`tile_windows_ws` unreachable from `retile_ws`).
- [ ] D3. `FIT_WINDOW`/width-factor vs dwindle-resize duality review.
- [ ] D4. Fold scattered `XRaiseWindow` loops into one `restack_workspace` helper.

## Log

- 2026-09-23: Plan written. Starting Phase A.
- 2026-09-23: Phase A done. Zero warnings. Not committed per user request.
