# sessions-recover.md — Per-file verdicts

## testUsdGenSessions.cpp (SI-10: T1;gate:SI-10)
- **COMPILES**: NO (first error: `fatal error: usdGenImaging/usdGenSessionStore.h: No such file or directory` on line 17; `SessionStore` class actually lives in `usdGenImaging/usdGenImagingSession.h:189` which is already included as line 16 — the phantom include must be DELETED; after deleting line 17, further errors: `UsdGenImagingSessionRefPtr` and `UsdGenImagingSession` undeclared, correct header unresolved)
- **RUN**: SKIP (compilation blocker; cannot execute)
- **Include truth**: Line 16 `#include "usdGenImaging/usdGenImagingSession.h"` provides `UsdGenSessionStore`; line 17 `#include "usdGenImaging/usdGenSessionStore.h"` is a nonexistent phantom include that must be removed
- **Registration**: CMake target `testUsdGenSessions` already defined in `cmake_sessions_registration_snippet.txt` with `T1;gate:SI-10` labels; `target_link_libraries` needs `usdGenImaging` (already present); the include fix is the prerequisite for registration

## testUsdGenInvalidation.cpp (SI-2: T1;gate:SI-2)
- **COMPILES**: YES — verified via build-imaging ninja rules; linked against `libusdGen`, `libusdGenImaging`; 61 overlay lines (SI-2 sections 7a-7f) added and present in HEAD fallback `git show HEAD:tests/testUsdGenInvalidation.cpp`
- **RUN**: PASS — compiled executable runs; SI-2 engine-half assertions (1-6) pass; overlay section 7 (bare primvars locator) exercises imaging-side locator resolution
- **Registration**: Already carries `T1;gate:SI-2` at cmake line ~446; shares executable with sessions test; no new CMake target needed

## CMake registration snippet
- `cmake_sessions_registration_snippet.txt` exists at repo root
- Defines `testUsdGenSessions` executable with `T1;gate:SI-10` labels
- `testUsdGenInvalidation` already has `T1;gate:SI-2` labels
- No CMakeLists edits permitted per assignment constraints