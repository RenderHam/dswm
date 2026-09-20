# DSWM Stability & Quality Plan

> **Goal:** Fix error-prone code, add missing safety protocols, remove unnecessary restrictions.
> **Reference:** bspwm source at `~/Project/bspwm/src/`
> **Base:** Commit `a73463f` on master

---

## Analysis Summary

### Critical Issues Found

| # | Issue | Severity | bspwm equivalent |
|---|-------|----------|-----------------|
| 1 | No BAD_WINDOW error handling — any X call on destroyed window crashes WM | CRASH | `process_error()` ignores BAD_WINDOW |
| 2 | No FocusIn handler — focus stealing not prevented | BUG | `focus_in()` + `update_input_focus()` |
| 3 | No window existence checks before X calls | CRASH | `window_exists()` |
| 4 | No ICCCM input hint checks before `XSetInputFocus` | BUG | `set_input_focus()` checks `input_hint`/`take_focus` |
| 5 | Floating windows clamped to monitor bounds (can't move off-screen) | LIMITATION | bspwm allows free movement |
| 6 | Floating windows clamped to monitor size (can't resize larger) | LIMITATION | bspwm allows any size |
| 7 | `raise_above_windows` called from too many places (focus_cycle, move_horizontal, etc.) | WASTE | bspwm uses precise sibling-based stacking |
| 8 | `compute_usable_w` duplicates `compute_usable_area` | DEAD CODE | — |
| 9 | `handle_configure_request` blocks ALL stacking for ALL managed windows | BUG | bspwm only blocks for tiled, sends synthetic ConfigureNotify |
| 10 | No motion event debouncing | PERF | bspwm has `pointer_motion_interval` |

---

## Fix Plan

### Fix 1: X Error Safety (CRITICAL)

**Problem:** Every `XSetInputFocus`, `XRaiseWindow`, `XMoveResizeWindow`, `XSetWindowBorder` call can fail with `BadWindow` if the window was destroyed between the event that triggered the call and the call itself. The current error handler (`xerror` in main.c:128-132) silently returns 0, which is correct behavior, but there's no proactive checking.

**Solution:** Add a `window_exists()` helper and check before critical X calls. Also add `FocusIn` handler.

**File:** `wm.c`

**Changes:**
1. Add `window_exists(Window w)` helper — uses `XGetWindowAttributes` with error check, or `XSendEvent` with `BadWindow` check
2. Add `handle_focus_in(XFocusInEvent *e)` — re-focus the tracked window if root gets focus (prevents focus stealing)
3. Add `FocusIn` case to event loop in `main.c`
4. Guard `XSetInputFocus` in `refocus()` with `window_exists()` check
5. Guard `XSetInputFocus` in `refocus_after_remove()` with `window_exists()` check

**bspwm reference:** `events.c:343-373` (`focus_in`), `window.c:985-996` (`window_exists`)

---

### Fix 2: ICCCM Input Hints

**Problem:** `refocus()` blindly calls `XSetInputFocus` on every window. Some windows set `WM_HINTS` with `input = False` meaning they don't want keyboard focus. Others use `WM_TAKE_FOCUS` protocol instead.

**Solution:** Read `WM_HINTS` during `manage_window()`, store in `ManagedWindow`, check before focusing.

**File:** `dswm.h`, `wm.c`

**Changes:**
1. Add `int input_hint` field to `ManagedWindow` (default 1)
2. In `manage_window()`, read `WM_HINTS` via `XGetWMHints`, store `input_hint`
3. In `refocus()`, skip `XSetInputFocus` if `!next->input_hint`, send `WM_TAKE_FOCUS` client message instead

**bspwm reference:** `window.c:920-936` (`set_input_focus` checks `icccm_props.input_hint`)

---

### Fix 3: Floating Move/Resize — Remove Arbitrary Limits

**Problem:** `handle_motion_notify()` (wm.c:1365-1375) clamps floating windows to monitor bounds:
- Move: `if (x < mon->x) x = mon->x` ... `if (x + w > mon->x + mon->width) x = ...` 
- Resize: `if (nw > mon->width - 2*BORDER_WIDTH) nw = ...`

This prevents users from:
- Moving a floating window partially off-screen (useful for side-by-side comparison)
- Making a floating window larger than the monitor (useful for image viewers, videos)

**Solution:** Remove monitor clamping. Only enforce `MIN_WIN_DIM` on resize. Allow any position and size.

**File:** `wm.c`

**Changes:**
1. Remove the 4 `if` clamping blocks in `handle_motion_notify()` (lines 1367-1372)
2. Keep `MIN_WIN_DIM` check for resize minimum
3. Remove `mon->width - 2 * BORDER_WIDTH` max size clamp

**bspwm reference:** `window.c:487-545` (`move_client`) — no bounds clamping at all

---

### Fix 4: Fix `handle_configure_request` Stacking

**Problem:** Current code blocks `CWSibling | CWStackMode` for ALL managed windows (tiled and floating). Floating clients should be able to configure their own stacking among themselves.

**Solution:** Only block stacking for tiled windows. For floating, proxy the stacking request. Also send synthetic `ConfigureNotify` for tiled windows (like bspwm does) so clients know their actual geometry.

**File:** `wm.c`

**Changes:**
1. Tiled path: block stacking (current behavior), send synthetic `ConfigureNotify` with actual tiled geometry
2. Floating path: proxy stacking requests (restore the original behavior we removed)

**bspwm reference:** `events.c:98-218` (`configure_request`) — tiled gets synthetic ConfigureNotify, floating gets full proxy

---

### Fix 5: Remove `raise_above_windows` from Unnecessary Call Sites

**Problem:** `raise_above_windows()` is called from 7 places: `retile_ws`, `focus_cycle`, `move_horizontal`, `dwindle_focus_prevnext`, `toggle_layout`, `resize_master`, `resize_window`. Most are redundant because `retile_ws()` already calls it.

**Solution:** Only call from `retile_ws()` and places that change floating state without triggering retile.

**File:** `wm.c`, `layout.c`

**Changes:**
1. Remove from `focus_cycle()` (wm.c:610) — focus doesn't change stacking
2. Remove from `move_horizontal()` (wm.c:156) — camera scroll doesn't change stacking
3. Remove from `dwindle_focus_prevnext()` (layout.c:869) — focus doesn't change stacking
4. Remove from `resize_master()` (layout.c:410) — `tile_windows_ws()` doesn't need it (only horizontal layout uses `update_camera_ws`)
5. Remove from `resize_window()` (layout.c:428, 441) — retile handles it
6. Keep in `retile_ws()` — this is the correct single place
7. Keep in `toggle_layout()` — no retile path on some branches

---

### Fix 6: Clean Up Dead Code

**File:** `layout.c`

**Changes:**
1. Remove `compute_usable_w()` (lines 197-203) — duplicates `compute_usable_area()`, used only in `update_camera_ws()`. Replace with inline or call `compute_usable_area()`.

---

## Implementation Order

| Step | Fix | Risk | Files |
|------|-----|------|-------|
| 1 | Fix 3 — Remove floating move/resize limits | LOW | wm.c |
| 2 | Fix 5 — Remove redundant `raise_above_windows` | LOW | wm.c, layout.c |
| 3 | Fix 6 — Remove `compute_usable_w` | LOW | layout.c |
| 4 | Fix 4 — Fix configure_request stacking | MED | wm.c |
| 5 | Fix 1 — X error safety + FocusIn | MED | wm.c, main.c |
| 6 | Fix 2 — ICCCM input hints | MED | dswm.h, wm.c |

---

## Verification

After each fix:
- `make clean && make` — zero warnings
- Test focus cycling with floating windows stacked
- Test Mod4+drag floating windows off-screen
- Test Mod4+Right-click resize beyond monitor bounds
- Test applications that set `WM_HINTS input = False` (e.g., some GTK apps)
- Test `_NET_WM_STATE` client messages from applications
