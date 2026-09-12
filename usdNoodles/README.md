# noodles

`noodles` is a standalone C++17 library for rendering interactive node graphs
with OpenGL. It provides the core rendering, layout, spatial indexing, and
undo/redo machinery used to display large directed graphs of nodes connected
by curved links — the kind of UI you see in shader editors, compositors, and
visual programming tools.

The library is host-agnostic: it does not create a window or own a GL
context. The consuming application provides a current OpenGL context and
calls `noodles` to draw into it.

## Status

🚧 Early-stage / experimental. The public C++ API may change without notice.

## Features

- GPU-accelerated rendering of node graphs with thousands of nodes
- MSDF (multi-channel signed distance field) text rendering for crisp,
  zoom-independent labels
- Curved Bezier links with forward and polyline rendering modes
- Spatial index for fast hit-testing and viewport culling
- Pluggable command-based undo/redo stack
- Host-neutral attribute-connection drag controller with live dangling-link
  previews and an optional OpenUSD SeExpr authoring adapter
- Animator for smooth interpolation of node positions and link geometry
- Cross-platform rendering backends: OpenGL 3.3 on macOS/Linux/Windows and
  OpenGL ES 3.0 on iOS/iPadOS and Android

## Repository Layout

```text
core/      Math primitives, node data model, render config, animator
render/    OpenGL renderers (nodes, links, text, stickers, font atlas)
spatial/   Spatial index for hit-testing
undo/      Command pattern + undo manager
assets/    Bundled fonts (Poppins, Baskerville) and GLSL shaders
tests/     GoogleTest-based unit tests
```

## Building

See [`BUILDING.md`](BUILDING.md) for full build instructions.

Quick start:

```bash
git clone https://github.com/facebookexperimental/noodles.git
cd noodles
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Implementation-only dependencies (stb and RapidJSON) are fetched
automatically when missing. GLEW is only used on Linux and Windows; Apple
platforms use their system OpenGL/OpenGLES frameworks directly.

## Using `noodles` in your project

After `cmake --install build`, link against the installed library:

```cmake
find_package(noodles 1.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE noodles::noodles)
```

The installed package exports `noodles_ASSET_DIR` (and the uppercase alias
`NOODLES_ASSET_DIR`) pointing at the installed fonts and shaders. A build-tree
`add_subdirectory` integration exposes `NOODLES_ASSET_DIR` and the same value
as the `NOODLES_ASSET_DIR` property on the `noodles` target.

See the headers under `core/` and `render/` for the public API. Hosts that draw
their own whole-prim relationship arrow and already place `LinkData::end` at
that arrow endpoint can disable noodles' normal target inset explicitly:

```cpp
noodles::RenderConfig renderConfig;
renderConfig.insetWholePrimRelationshipTargets = false;
```

The default remains `true`, preserving the standalone renderer's arrow-base
endpoint behavior.

## Attribute connection drags

`core/AttributeConnectionDrag` maps a host's pointer-down, pointer-move,
pointer-release, and escape/lost-capture events to `Begin`, `Update`, `Drop`,
and `Cancel`. Endpoints retain their exact namespaced property names. `Preview`
returns a connected `LinkData` over a compatible port and a cursor-positioned
dangling `LinkData` otherwise; hosts can render that temporary noodle with
`LinkRenderManager::renderLinks` without inserting it into `GraphModel`.

The controller has no window, mouse, or USD dependency. Its author callback
returns an unapplied undo command; the controller executes it once and gives it
to `NoodlesUndoManager`. The optional `noodles::usd` adapter validates and
authors expression-output to operator-input connections. A host chooses the
evaluation grain/domain in that callback; noodles does not infer it.

## Contributing

We welcome pull requests. See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the
contributor workflow and Meta's CLA requirements.

## License

`noodles` is MIT licensed. See [`LICENSE.txt`](LICENSE.txt) for details.

This repository bundles third-party assets (Poppins and Baskerville fonts).
Their licenses are reproduced in [`THIRD_PARTY_LICENSES.txt`](THIRD_PARTY_LICENSES.txt).
Build-time dependencies fetched by CMake (GLEW, stb, RapidJSON) are also
acknowledged there.
