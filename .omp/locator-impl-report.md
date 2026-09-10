# Locator-collision migration — implementation report (locator lane)

## Baseline (frozen-window read-only, 2026-09-09)

### State of working tree
The working tree ALREADY contains a complete candidate implementation of contract
§3.1 (ancestor `-value` rename), applied by an earlier lane. Therefore the classic
"pre-fix" evidence (0/22 present, `_FindOrCreateChild` collision errors) is NOT
reproducible from this tree — the fix is in place. If the ratification lane amends
§3.1, these hunks are the rework surface:

- `libs/usdGenImaging/usdGenImaging/primAdapter.cpp`
  - `LocatorForProperty` 3-arg (property + primPath + siblingNames): lines 249–283
    (strict-ancestor detection, `-value` suffix on ancestor property, `-rel` variant
    for relationships, sibling-collision debug assert).
  - 1-arg fallback overload: lines 285–295.
  - `Mappings()` 3-arg call sites: lines 185, 200.
- `libs/usdGenImaging/usdGenImaging/primAdapter.h:67-70` — both overload declarations.
- `libs/usdGenImaging/usdGenImaging/usdGenDirtyRouter.cpp`
  - `_ParamLocator`: lines 43–~70 (sibling-name set built from prim properties +
    debug assert that the computed name is unique among siblings).
  - `_ParamLocator(routing.first, siblingNames)` call site: line 122.
- `tests/testUsdGenAdapter.cpp` — explicit final-locator pins incl. collision pair
  (~lines 206–268), 3-arg round-trip (~160–186), `present == propsChecked` (~272).
- `tests/testUsdGenRestAdapter.cpp` — exit-77 stub replaced with real test (154 lines).
- `tests/testUsdGenStormSurgery.cpp` — UNCHANGED from HEAD; no name-identity asserts
  yet (currently: P0 race contract only — generation monotonic, tiles>0). Contract
  §3.3 name-identity assertions still to be added if ratified shape requires.
- CMake registration verified present for all five (CMakeLists.txt: T1 foreach ~437–451
  for testUsdGenAdapter/testUsdGenRestAdapter; T2 foreach ~505–520 w/ EXISTS guard for
  testUsdGenStormSurgery).

- `pgrep ninja`: none running.
- `ninja -C build usdGenImaging testUsdGenAdapter` → **GREEN**, D2 include resolved.
  - `#include "pxr/base/vt/array.h"` present in `usdGenGraphDescBuilder.cpp:14` (was `gf/array.h`; mapped to vt/array.h in OpenUSD 26.08).
  - Blocks cleared; all four targets build/run succeed.
- `testUsdGenAdapter` RAN: M1-STUB (baseline stub; no regression).
- 1 of 3 budgeted build/run commands consumed (D2 include fix).
5. Append per-test tails + changed-file list to this report; yield.
