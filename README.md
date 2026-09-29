# usdGen

usdGen generates and grooms hair and fur inside an OpenUSD pipeline. A groom is ordinary USD: a description, an operator stack, guides, maps, and looks. A Hydra scene index cooks that graph and publishes curves for Storm and other render delegates. usdview plugins author the groom and edit expressions on the stage.

The intended result is a set of plugins and tools a studio pipeline can build, install, and load next to OpenUSD. Artists and TDs use the usdview tools. Engineers use the libraries, the schema, and the tests.

## What ships in this repository

- **Engine** (`libs/usdGen`, `libs/usdGenMath`). A stage-free operator graph. Example scenes exercise scatter, grow, noise, length, width, clumping, guide interpolation, curve import, and deformation.
- **Hydra** (`libs/usdGenImaging`, `plugin/usdGenImaging`). A groom scene index that publishes curve tiles into Hydra, including Storm shading and optional instanced fur.
- **Schema** (`libs/usdGenSchema`, `plugin/usdGenSchema`). Codeless `usdGen:` types. The checked-in resources are generated from `libs/usdGenSchema/schema.usda`.
- **Pomade** (`libs/usdGenPomade`, `plugin/usdGenPomadeTools`). A usdview tool for hierarchical groom authoring: tubes, fills, guides, and a commit back to USD. See [docs/pomade-tool.md](docs/pomade-tool.md).
- **Expression editor** (`plugin/usdGenTools`). A usdview dock for SeExpr on `usdGen:expr:source`.
- **Shaders** (`usdGenShaders`). Storm materials for strand shading and self-shadowing. See [docs/storm-fur.md](docs/storm-fur.md).
- **Examples** in [examples/](examples/README.md).

Pomade is this repository's authoring tool. It is not a separate product name from an outside DCC.

## How it sits next to OpenUSD and usdRig

usdGen is built against **OpenUSD 26.08** and uses Hydra 2 scene indexes. OpenUSD is a build dependency. It is not vendored here. Its license is the Tomorrow Open Source Technology License; see [NOTICE](NOTICE).

**usdRig** is an optional sibling. The groom index is meant to run after deformation (UsdSkel or RigExec) in the Hydra chain. Configure `-DUSDGEN_WITH_RIGEXEC=ON` and `RIGEXEC_INSTALL_DIR` only when you want to link `rigExecMath`. A normal usdGen build does not need usdRig.

This repository does not integrate usdOrchestrator.

## Quick start

You need CMake 3.26 or newer, a C++17 compiler, and an OpenUSD 26.08 install. Ninja is recommended. CUDA 12.8 or newer is optional; without it the build is CPU-only and a cook that asks for the CUDA backend is refused at runtime.

From the repository root:

```sh
export USD=/path/to/OpenUSD
export VENV=/path/to/venv   # optional; Python that can import pxr
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DUSD_INSTALL_DIR="$USD"
ninja -C build
```

`bin/build_usdgen.sh` wraps configure and build. On Windows, `bin/build_usdgen.ps1` does the same job. The OpenUSD prefix must match the compiler you are using.

Load the build tree and run a flatten smoke test:

```sh
source bin/_env.sh
usdcat --flatten tests/scenes/scene_empty_groom.usda
```

`bin/_env.sh` sets `USD`, `GEN`, `PY`, and `PXR_PLUGINPATH_NAME` from the checkout location. Override `USD` or `VENV` before sourcing it when the defaults (a sibling `OpenUSD_26_08` directory, and `python3` on `PATH`) are wrong.

Open an example in usdview:

```sh
source bin/_env.sh
"$PY" "$USD/bin/usdview" examples/scatter-grow-plane.usda
```

On Windows, `bin/launch_usdview.ps1` is the equivalent launcher.

Install:

```sh
cmake --install build --prefix /path/to/usdGen-install
```

That writes the libraries, headers, and plugin resources under the prefix.

Turn the groom scene index off for one process with `USDGEN_ENABLE=0`.

## More documentation

| | |
|---|---|
| [docs/README.md](docs/README.md) | Index of user-facing docs and historical notes |
| [docs/pomade-tool.md](docs/pomade-tool.md) | Pomade workspace, modes, and hotkeys |
| [docs/storm-fur.md](docs/storm-fur.md) | Storm hair shading and self-shadowing |
| [docs/moonray-fur.md](docs/moonray-fur.md) | Instanced fur through the MoonRay Hydra delegate |
| [examples/README.md](examples/README.md) | Scenes and how to view them |
| [plan/README.md](plan/README.md) | Design record. It is not the build guide |

Regenerating schema resources, test tiers, and coding conventions are in [AGENTS.md](AGENTS.md).

## License

usdGen is [MIT](LICENSE), Copyright (c) 2026 Nick Burkard.

Third-party code keeps its own license. OpenUSD is a build dependency under the [Tomorrow Open Source Technology License](https://openusd.org/license). SeExpr, Ptex, nanoflann, and zlib are vendored under `thirdparty/` with their upstream notices. `usdNoodles/` includes the noodles project (MIT, Meta Platforms), SIL OFL Poppins atlases, and a Baskerville atlas. Files in `usdNoodles/` that are not in the public noodles tree are project MIT. See [NOTICE](NOTICE) and [THIRD_PARTY.md](THIRD_PARTY.md).
