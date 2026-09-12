# Building noodles

`noodles` uses CMake 3.20+ and a C++17 compiler. Missing implementation-only
dependencies are fetched at pinned revisions by default.

## Requirements

| Dependency | Requirement | How it is obtained |
| --- | --- | --- |
| CMake | 3.20+ | system |
| C++ compiler | C++17 | system |
| stb_image | pinned commit | system header or `FetchContent` |
| RapidJSON | 1.1.0 | system package or `FetchContent` |
| GoogleTest | optional | system package, when `BUILD_TESTING=ON` |

The graphics dependency is platform-specific:

| Target | Headers and link dependency |
| --- | --- |
| iOS / iPadOS | OpenGL ES 3.0, system `OpenGLES` framework |
| macOS | OpenGL 3 core, system `OpenGL` framework |
| Android | GLES 3, NDK `GLESv3` library |
| Linux / Windows | OpenGL plus GLEW 2.2 (system or fetched) |

Apple deprecated OpenGL and OpenGL ES in iOS 12/macOS 10.14, but they remain
available for existing applications. `noodles` does not create or own a GL
context; the host must make a compatible context current before initialization
or rendering.

## Configure, build, and test on macOS

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=ON \
  -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CTest is enabled through the standard `BUILD_TESTING` option. If GoogleTest is
not installed, configuration succeeds but reports that the unit-test executable
was not generated.

## Build options

| Option | Default | Effect |
| --- | --- | --- |
| `BUILD_SHARED_LIBS` | `ON` | Build a shared library; `OFF` builds a static archive and propagates `NOODLES_STATIC` to consumers |
| `BUILD_TESTING` | `ON` | Build/register unit tests when GoogleTest is available and the build is not cross-compiling |
| `NOODLES_FETCH_DEPENDENCIES` | `ON` | Fetch missing stb, RapidJSON, and desktop GLEW dependencies |
| `NOODLES_REQUIRE_TESTS` | `OFF` | Fail configuration instead of silently omitting tests when GoogleTest is unavailable |
| `NOODLES_ASSET_DIR` | source `assets/` | Build-tree runtime font/shader root exposed to parent projects |
| `NOODLES_ENABLE_USD_EXPRESSION_CONNECTIONS` | `OFF` | Build/install the optional `noodles::usd` OpenUSD SeExpr connection adapter; requires `pxr` |
| `NOODLES_USDGEN_SCHEMA_RESOURCE_DIR` | empty | Required only for optional adapter tests; directory containing usdGenSchema `plugInfo.json` for typed `UsdGenOperator` inheritance |

The public C++ API is experimental. Shared-library install names include the
major and minor version (`libnoodles.1.1`) so a later minor release cannot be
loaded accidentally into an application built against a different public
struct layout. Rebuild dependents when upgrading minor versions.

## Build a device-architecture iOS archive

The library is context-agnostic C++, so the canonical iOS verification is a
device SDK arm64 static compile. The consuming app supplies the `EAGLContext`
and bundles the contents of `assets/`.

```bash
cmake -S . -B build-ios \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-ios --parallel
```

The iOS and Android backends adapt the shipped desktop GLSL 3.30 shaders to
GLSL ES 3.00 at runtime, use a GLES-compatible 2D transform texture, and fall
back from `glMultiDrawArrays` to individual draw calls.

## Install and consume the CMake package

```bash
cmake --install build --prefix /your/install/prefix
```

The install contains:

```text
<prefix>/lib/                         library/archive
<prefix>/lib/cmake/noodles/           config, version, and exported targets
<prefix>/include/noodles/{core,...}/  public headers
<prefix>/share/noodles/assets/        fonts and shaders
<prefix>/share/noodles/               README and license notices
```

Consume it from an independent CMake project:

```cmake
find_package(noodles 1.1 CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE noodles::noodles)

# Runtime font/shader root supplied by the installed package config.
message(STATUS "noodles assets: ${noodles_ASSET_DIR}")
```

### Optional OpenUSD expression connections

The core library remains host- and USD-independent. To build the optional
adapter and its test, point CMake at matching OpenUSD and usdGen schema
resources:

```bash
cmake -S . -B build-usd \
  -DNOODLES_ENABLE_USD_EXPRESSION_CONNECTIONS=ON \
  -DNOODLES_USDGEN_SCHEMA_RESOURCE_DIR=/path/to/build/usd/usdGenSchema/resources \
  -Dpxr_DIR=/path/to/OpenUSD/lib/cmake/pxr
```

The test sets `PXR_PLUGINPATH_NAME` only for itself so OpenUSD discovers the
schema plugin. It exercises `AttributeConnectionDrag` from an operator input
back to an expression output, then verifies the adapter's undoable USD edit.
The consuming host supplies its selected evaluation domain to the adapter
callback; no noodles component creates a window or performs mouse dispatch.

`tests/package-consumer/` is a standalone installed-package smoke consumer.
Configure it with `-DNOODLES_PACKAGE_CONSUMER_WITH_USD=ON` to require the
exported `noodles::usd` target as well as the core target.

`tests/package/` is a minimal out-of-tree consumer used to verify the installed
target, headers, link interface, version file, and asset-root contract.
When cross-compiling for iOS, CMake's sysroot search policy can intentionally
ignore a host `/tmp` prefix; pass
`-Dnoodles_DIR=<prefix>/lib/cmake/noodles` explicitly in that case.

## Host integration options

`RenderConfig::insetWholePrimRelationshipTargets` defaults to `true`: the
whole-prim relationship ribbon stops at the base of noodles' arrowhead. Set it
to `false` only when a host draws its own arrow and has already placed
`LinkData::end` at the desired ribbon endpoint. This keeps product-specific
endpoint ownership explicit instead of requiring a source fork.

## Platform notes

### Windows static builds

The exported `noodles::noodles` target propagates `NOODLES_STATIC` for a static
build. Shared builds define `NOODLES_EXPORTS` only while compiling the library.

### Linux / Windows GLEW fallback

When no system `GLEW::GLEW` target is available and dependency fetching is
enabled, noodles builds a pinned static GLEW target, installs its headers and
archive, and includes that target in `noodlesTargets.cmake`.

### Headless / CI rendering

The library does not create a GL context. Tests that execute GL calls require a
compatible current context (for example EGL/OSMesa or `xvfb-run` on Linux).
Most unit tests exercise layout, geometry, cache, and interaction logic without
creating a context.
