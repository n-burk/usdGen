# Third-party notices

usdGen is MIT, Copyright (c) 2026 Nick Burkard (`LICENSE`). The components
below keep their own licenses. This file is the index; `NOTICE` is the
short form installed to `share/usdGen/NOTICE`.

## Build dependencies (not in this tree)

| Component | License | Where the terms live |
| --- | --- | --- |
| OpenUSD | Tomorrow Open Source Technology License 1.0 | https://openusd.org/license |
| OpenSubdiv | Apache-2.0 with a modified trademarks section | upstream `LICENSE.txt` |
| MaterialX | Apache-2.0 | upstream license |
| oneTBB | Apache-2.0 | upstream license |

No OpenUSD source is vendored here, so there is no Pixar/TOST header to
relicense. Link against an OpenUSD build you supply (`USD` or
`-DUSD_INSTALL_DIR`).

## Vendored libraries

| Path | Upstream | License |
| --- | --- | --- |
| `thirdparty/seexpr`, `thirdparty/seexprFrontend` | Walt Disney Animation Studios SeExpr | Apache-2.0 with Section 6 (Trademarks) replaced. Disney copyright stays. Terms are in each header and in `thirdparty/seexprFrontend/LICENSE`. `thirdparty/seexpr` has no separate `LICENSE` file; the grant is the header block, which points at http://www.apache.org/licenses/LICENSE-2.0 |
| `thirdparty/ptex` | Ptex | BSD-3-Clause, Disney non-endorsement clause. `thirdparty/ptex/LICENSE` |
| `thirdparty/nanoflann/nanoflann.hpp` | nanoflann | BSD-2-Clause, in the header |
| `thirdparty/zlib` | zlib | zlib License. `thirdparty/zlib/LICENSE` |

## `usdNoodles/`

The directory is a copy of [facebookexperimental/noodles](https://github.com/facebookexperimental/noodles)
plus a few usdGen files.

Upstream files keep `Copyright (c) Meta Platforms, Inc. and affiliates`
and `usdNoodles/LICENSE.txt` (MIT). That includes `core/`, `render/`,
`spatial/`, `undo/`, `assets/`, `tests/` except the files named below,
and the upstream docs (`README.md`, `BUILDING.md`, `CODE_OF_CONDUCT.md`,
`CONTRIBUTING.md`, `SECURITY.md`, `THIRD_PARTY_LICENSES.txt`).

Poppins atlases under `usdNoodles/assets/fonts/Poppins-*` are SIL OFL 1.1.
The Baskerville atlas is described as public domain in
`usdNoodles/THIRD_PARTY_LICENSES.txt`. GLEW, stb_image, and RapidJSON are
FetchContent dependencies, not files in this repository; their notices are
in that same third-party license file.

These paths are **not** in the public noodles tree. They are usdGen code
under the root MIT license (`SPDX-License-Identifier: MIT`), not Meta's:

- `usdNoodles/.github/workflows/ci.yml`
- `usdNoodles/cmake/noodlesConfig.cmake.in`
- `usdNoodles/core/AttributeConnectionDrag.cpp`
- `usdNoodles/core/AttributeConnectionDrag.h`
- `usdNoodles/render/noodles_gl.h`
- `usdNoodles/tests/AttributeConnectionDragTest.cpp`
- `usdNoodles/tests/ExpressionConnectionsTest.cpp`
- `usdNoodles/tests/package-consumer/CMakeLists.txt`
- `usdNoodles/tests/package-consumer/main.cpp`
- `usdNoodles/tests/package/CMakeLists.txt`
- `usdNoodles/tests/package/main.cpp`
- `usdNoodles/usd/ExpressionConnections.cpp`
- `usdNoodles/usd/ExpressionConnections.h`
- `usdNoodles/usd/api.h`

## Optional host renderer

Scripts and comments can talk to a MoonRay Hydra plugin (`hdMoonray`,
`moonray:sceneVariable`). That renderer is not vendored. The plugin names
are the host API, so they are unchanged.
