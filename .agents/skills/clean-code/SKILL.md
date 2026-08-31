---
name: clean-code
description: Enforce clean code conventions for the dswm C window manager. Use when writing, modifying, or reviewing code in dswm.c, dswm.h, or dswm-session.c.
---

# dswm Clean Code

Enforce "The Art of Clean Code" principles: clear, unambiguous, functional, readable, accessible, reliable, efficient, maintainable.

## When to Use

- Writing new functions or modifying existing ones in `dswm.c`
- Editing `dswm.h` (header must remain declarations-only)
- Reviewing code for quality before commits
- Refactoring duplicated or overly long code

## Rules

### 1. Header Hygiene
- `dswm.h` = declarations only: `#define`s, typedefs, enums, function prototypes
- `dswm.c` = all definitions: static variables, function bodies
- NO `static` data definitions in headers (eliminates `__attribute__((unused))`)
- NO `#include` in headers unless type-dependent

### 2. Linkage
- Every internal function = `static`
- Only `main()` is external
- This enables compiler optimization and prevents symbol leakage

### 3. Const Correctness
- `const` on all read-only parameters
- `const` on all read-only pointer targets
- `const` on string literals and command arrays

### 4. Naming
- `snake_case` for functions and variables
- `SCREAMING_SNAKE` for `#define` constants
- No abbreviations: `browsercmd` not `browcmd`, `quit` not `quit_wm`
- No C++ reserved words as field names: `class` → `wm_class`
- Consistent naming across header and source

### 5. Magic Numbers
- Every literal gets a named `#define`
- `256` → `MAX_TILED`, `16` → `INITIAL_CAP`, `8` → `MAX_MONS`
- `0.3f`/`3.0f` → `MIN_MASTER_HORIZ`/`MAX_MASTER_HORIZ`
- `0.1f`/`0.9f` → `MIN_MASTER_VERT`/`MAX_MASTER_VERT`
- `1`/`10` → `MIN_SCROLL_VIS`/`MAX_SCROLL_VIS`

### 6. Duplication (DRY)
- Extract shared logic into helpers
- Bar/strut/usable computation → `compute_usable_area()`
- `scroll_left`/`scroll_right` wrappers → eliminate, call directly
- `increment`/`decrement_scroll_visible` → `adjust_scroll_visible(int delta)`

### 7. Function Length
- Max 40 lines per function body
- Extract loop bodies, switch arms, init blocks into helpers
- One function = one responsibility

### 8. Style
- C99 throughout: `for (int i = ...)` allowed
- Single space after commas, no double spaces
- Consistent brace style: opening brace on same line
- 4-space indent, no tabs

### 9. Safety
- Check `realloc` return: save old pointer, free on failure
- No `void*` ↔ `int` cast gymnastics
- Re-validate pointers after `memmove`
- Use typed function signatures, not `void*` dispatch where possible

### 10. Dead Code
- Remove unused variables, unreachable branches, empty functions
- If a feature is placeholder-only, remove it from keybindings too
- `scroll_maximized` branches: implement or remove
- `toggle_gap()`: implement or remove from `keys[]`

## Verification

After any change, run:
```sh
make clean && make
```
Must produce **0 warnings, 0 errors**.

## File Structure

```
dswm.h      — declarations only (types, enums, macros, prototypes)
dswm.c      — all implementation (static vars, function bodies, main)
dswm-session.c — session wrapper (standalone, no deps on dswm.h)
```
