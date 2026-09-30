# Ptex vendoring

This directory vendors the Ptex texture library from the `v2.4.3` tag
(annotated tag object `e78fdaa64b7abe9a4bf28e173b19869b28e03da7`, commit
`4aa2c35f9f25f025014fbbbf41cef669881b4172`) of
`https://github.com/wdas/ptex`.

v2.4.3 is chosen because it is zlib-based: v2.5.0 switched to libdeflate,
whose headers are absent from the OpenUSD prefix this project builds against
(`plan/07-look-maps-expressions.md` §6.1). The OpenUSD install has no Ptex of
its own (`PXR_ENABLE_PTEX_SUPPORT` is off), and Storm's Ptex path is mesh-only
anyway, so usdGen samples `.ptx` files on the CPU at capture time. The zlib it
needs is vendored next to it in `thirdparty/zlib`.

Contents: `ptex/` holds only the library sources and headers of upstream
`src/ptex/` (the ten `.cpp` files the upstream library target compiles plus
every header). The upstream license is preserved in `LICENSE` (BSD-3-Clause
with an upstream non-endorsement clause).

Local changes:

* `ptex/PtexVersion.h` is upstream `PtexVersion.h.in` instantiated for 2.4
  (`PtexLibraryMajorVersion 2`, `PtexLibraryMinorVersion 4`) plus a comment;
  upstream generates it at configure time.
* `PtexHalfTableGen.cpp` (the stand-alone program that generated the checked-in
  `PtexHalfTables.h`) is not vendored: it has a `main()` and is not part of the
  library.
* No source is modified.

CMake builds the sources as the private static target `usdGen_ptex` with
`PTEX_STATIC` (no dllimport/visibility attributes) and `PTEX_VENDOR=usdGen`
(the library then lives in `Ptex::v2_4_usdGen`, so it cannot collide with a
site's Ptex in the same process). It is linked privately into the `usdGen`
shared library, compiled with hidden visibility, and is never installed or
exported; the include directory is a build-tree SYSTEM include only. Engine
code reaches Ptex only through `libs/usdGen/usdGen/maps/ptexMap.h`, which does
not include `Ptexture.h`.
