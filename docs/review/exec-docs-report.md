> Historical engineering note. It records a review or probe, not the current product overview. Start at the [repository README](../../README.md) and [docs index](../README.md).

# exec-docs report — qwen execution lane (M0 exit)

Executed against `sol-m0-ci-docs-notes.md` (13 claims, all verified in this
run). `sol-m0-exit-notes.md` begins with **SOL UNAVAILABLE** and was skipped
per task rule step 1 — no X- claims were available to process; the only
exit-related note (M0 exit checklist) is therefore **not** attested here and
goes to the gatekeeper as unverified.

## Per-note status

### `.github/workflows/usdgen.yml` (my owned path)

| Note | Priority | Status | What changed |
|------|----------|--------|--------------|
| C-1 build_usd.py path | P0 | **FIXED** | `python3 build_usd.py` → `python3 build_scripts/build_usd.py` in the `openusd` build step. Verified against the v26.08 checkout at `<openusd-src>`: the entrypoint exists only at `build_scripts/build_usd.py`, and every flag used (`--usd-imaging`, `--no-usdview`, `--no-python`, `--no-tests --no-examples --no-tutorials --no-docs`, `--no-materialx`, `--generator`, `--build-variant`, `--jobs`, `-v`) was grep-confirmed present in that file's argparse. `--usd-imaging` confirmed to set `PXR_BUILD_IMAGING=ON` + `PXR_BUILD_USD_IMAGING=ON` (`build_usd.py:1878,1927`). |
| C-2 exec bits lost in artifact | P0 | **FIXED** | `openusd` now packs the prefix: `tar -C usd-install -cf usd-install.tar .` and uploads `usd-install.tar`; every consumer job (`configure-offline`, `build-release`, `t0-t1`, `install-check`, `build-fpfast`) downloads and untars. Comment block explains why (artifact upload normalizes modes to 0644; cache is unaffected, it preserves exec bits). |
| C-3 missing 6.2 job graph | P1 | **FIXED** | Replaced the single `usdgen-build-test` job with the plan §6.2 shape: `configure-offline` (configures with `FETCHCONTENT_FULLY_DISCONNECTED=ON` + `USDGEN_WITH_RIGEXEC=OFF`), `build-release` (Release configure + `ninja -j16` + usdcat smoke; uploads the build tree), and `t0-t1` (`ctest -L '^T[01]$' -j8` against that tree, no rebuild). Job comments state that `t2-egl` (measurement host) and `t3-xvfb` are workstation/manual at M0. The unfiltered second `ctest` was removed; a **by-name bridge step** (`ctest -R '^testUsdGenLinkRule_(usdGenMath|usdGenTestUtils)$'`) covers the two link-rule tests that still lack T0 labels in `CMakeLists.txt` — minimal gate B-1 coverage until the CMake lane labels them (marked for deletion). |
| C-4 install-check not consuming the package | P1 | **ALREADY COMPLIANT — no change needed** | The job already does exactly the task-specified fix: `cmake --install` into a scratch prefix (`usdgen-install`), layout verification (`lib/libusdGen*.so` ×3, `include/usdGen*`, both plugin resource dirs + `generatedSchema.usda`), and a python3 validator asserting schema `Type=="library"`, `LibraryPath` → installed `libusdGenSchema.so` (file exists), `implementsComputeExtent` on `UsdGenDescription`, `AutoApplyAPISchemas` **inside** `Info`, `UsdGenMaskAPI` auto-apply, and imaging `Type=="library"` + `LibraryPath` → installed `libusdGenImaging.so`. I confirmed the layout assertions match the actual install rules in `CMakeLists.txt` (`USDGEN_INSTALL_LIBDIR=lib`, `USDGEN_INSTALL_PLUGINDIR=lib/usd`). SOL's *deeper* C-4 evidence (registered `testUsdGenInstallTree`/`testUsdGenConsumer`/`testUsdGenPluginDiscovery`; `configure_package_config_file()` missing `PATH_VARS` so `usdGenConfig.cmake` leaves `usdGen_PLUGIN_DIR`/`usdGen_LIBRARY_DIR` empty) is **out of my lane** (`cmake/`, `CMakeLists.txt`) — see Open items. The job header comment now documents that dependency. |
| C-5 cache key | P1 | **FIXED** | Key now `openusd-${{ runner.os }}-${{ runner.arch }}-${{ steps.openusd-sha.outputs.sha }}-usdimg-gl-nopy-v1` (restore + save), with a comment: the epoch suffix names the recipe (usd-imaging, GL on, no python, no MaterialX) and must be bumped to `-v2` when the `build_usd.py` invocation changes. |

YAML validated: `python3 -c "import yaml; yaml.safe_load(...)"` → OK, jobs
`[openusd, configure-offline, build-release, t0-t1, install-check, build-fpfast]`.

### `docs/workstation-protocol.md` (my owned path) — full rewrite

| Note | Priority | Status | What changed |
|------|----------|--------|--------------|
| C-6 commands not launchable | P1 | **FIXED** | `bin/_env.sh` is not in my owned paths, so I took the task's preferred option: every section now sets `PXR_PLUGINPATH_NAME` and `LD_LIBRARY_PATH` explicitly in its own block, and every tool launch uses full paths through an explicit interpreter: `"$PY" "$USD/bin/usdview"` / `usdrecord` / `testusdview` (`PY=$VENV/bin/python3` — verified to be the build venv; `"$PY" -c 'import pxr'` resolves to `$USD/lib/python3.12/site-packages/pxr`). Verified the tool shapes: `usdview`/`usdrecord`/`testusdview` are Python scripts with venv shebangs; `usdcat` is a native ELF. A "Common environment" section documents this plus a one-time sanity block (`test -x $USD/bin/usdcat`, pxr import, `usdcat --version`). |
| C-7 nonexistent MSAA/A2C toggles | P1 | **FIXED** | §3 now uses the real 26.08 names, each grep-verified in the OpenUSD source: `USDVIEW_ENABLE_MSAA=0\|1` (read at process start, `pxr/usdImaging/usdviewq/stageView.py:839-842`; `"1"` → 4-sample backbuffer) with three **separate** usdview launches per resolution, and A2C-off via `TF_DEBUG=HDX_DISABLE_ALPHA_TO_COVERAGE` (real `TfDebug` symbol, `hdx/debugCodes.h:20`; consumed at `hdx/drawTargetTask.cpp:428` and `hdx/renderSetupTask.cpp:227`). The invented `HD_ENABLE_SAMPLE_ALPHA_TO_COVERAGE` is gone (grep-confirmed absent). |
| C-8 placeholders | P1 | **FIXED** | No `<…>` fixture/path/interpreter tokens remain outside legitimate CLI-usage documentation (`makeStage.py <n_prims> <out.usda>`). Verified against the real CLIs: usdrecord uses `-f`/`--frames` and `-r`/`--renderer` (UsdAppUtils arg groups) and Hydra requires exactly one `###` frame-number placeholder in the output path for frame ranges — all `usdrecord` legs now carry `###` and use `-w 1920` (usdrecord has **no** `--viewportSize`; it is a usdview/testusdview option only, `usdviewq/__init__.py:233`). §5 Vulkan leg is exact: `HGI_ENABLE_VULKAN=1` (`hgi/hgi.cpp:24`) with a verified dev-host caveat (local prefix ships `libusd_hgiGL.so`/`hgiInterop` only — no Vulkan/Metal backend). §6 ptex leg now names `bench/ptex_baked_groom.usda` (M4) with an explicit `test -f` fallback to the existing prototype fixture. §7 RMANTREE is `${RMANTREE:?...}` + `test -x "$RMANTREE/bin/prman"`, no `/path/to/rman`. §8 usdview gets `--viewportSize 3840 2160`. §9 loops the exact 10 k/100 k/1 M fixtures (`bench/head10k.usda`, `bench/head100k.usda`, `bench/head1M.usda`) with `test -f` guards. §10 generates its stages concretely via the existing `plan/prototypes/freeze-bake/uv/makeStage.py`. Every section creates its `out/t4/§N/` dir with `mkdir -p` before writing. |
| C-9 RC-4 mapping | P1 | **FIXED** | The gate-mapping paragraph and §2 now say RC-4 (≥ 30 fps 1080p comb stroke) is read from **§9**; §2 is "none — recorded only" (static/tumble baseline). §9's heading and pass criterion carry RC-4, including the concrete 33.3 ms/30 fps bound. |
| C-10 retired W ids spelled out | P1 | **FIXED** | The numbering paragraph now reads "There is no alternate identifier scheme; all previous W ids are retired" — `grep -n 'W[0-9]'` returns nothing in the doc. |

### `docs/prework/probes/m0-usdcat-extent/probe_usdcat_extent.cpp` + evidence doc (my owned paths)

| Note | Priority | Status | What changed |
|------|----------|--------|--------------|
| C-11 flattened tokens ≠ registration | P0 | **FIXED** | The probe now asserts, for **both** `UsdGenGroom` and `UsdGenDescription`: `TfType::FindByName(name)` is not `TfType::Unknown()` **and** `UsdSchemaRegistry::IsConcrete(name)` is true (both APIs verified present in the 26.08 installed headers, `usd/schemaRegistry.h:290/295`). Empirical finding recorded in the probe comments: codeless types only become visible **after** `UsdStage::Open` + prim resolution (SdrCatalog plugin discovery), so the checks run after `GetPrimAtPath` — a check before stage open reads unknown even in the positive control. Fresh runs: negative control (no plugin dir) → `known=0/IsConcrete=0` ×2, no extent, **exit 1**; positive control (build-tree schema resources dir on `PXR_PLUGINPATH_NAME`) → `known=1/IsConcrete=1` ×2, `ComputeExtent ok=1 size=2`, extent `[(-0.5 -0.5 -0.5)..(0.5 0.5 0.5)]`, **exit 0**. |
| C-12 PLUG_REGISTRATION ≠ dlopen | P0 | **FIXED** | Evidence restructured around the 26.08 `plug` debug codes (`plug/debugCodes.cpp:16-17`): `Registering shared library plugin …` fires on metadata read; `Loading plugin 'usdGenSchema'.` fires on the actual dlopen. New Run 4a (probe, `TF_DEBUG="PLUG_LOAD PLUG_REGISTRATION"`) shows **both**: line 1 registration, line 113 load — the load lands immediately before `ComputeExtent ok=1`, proving the extent callback resolved from the freshly dlopen'd library. New Run 4b (usdcat under the same `TF_DEBUG`) shows only `Registering … 'usdGenSchema'` and a single `Loading plugin 'sdf'` — usdcat **never loads** the schema lib during flatten, confirming the old page's "PLUG_REGISTRATION proves dlopen" claim was false. Also documented: 26.08 `TF_DEBUG` symbol lists are **space-separated**; `TF_DEBUG=PLUG_LOAD,PLUG_REGISTRATION` (comma) matches nothing (verified empirically). |
| C-13 unreproducible transcript | P1 | **FIXED** | Evidence doc fully rewritten: env block is valid shell (`export PXR=…` etc.); every run is a single copy-pasteable `env -i … "$PROBE"/"$PXR/bin/usdcat" …` command with `echo "exit=$?"` appended, and the verbatim blocks end with the recorded exit code (`exit=1` for Run 1, `exit=0` elsewhere). All runs re-executed fresh on the current build tree. New Run 0 includes `ldd "$PROBE"` verbatim plus `grep -ci usdgen` → **0** (no `libusdGen*` dependency; the probe links stock `libusd_*` only, so any extent is runtime-dlopen-sourced). |
| Step 3 (rebuild probe) | — | **DONE** | Recompiled against `$USD` with the specified `-I /usr/include/python3.12`, the specified library list, and `-Wl,-rpath,$USD/lib`. Deviation, documented in the evidence doc: the specified list alone fails to link (undefined `pxr_boost::python::converter::registry::lookup`), so `-lusd_python -lpython3.12` is appended — required only by the boost-python shim headers pulled through `usd/stage.h` → `ar/resolverContext.h` → `tf/pyLock.h`; no runtime Python. |

### `README.md`, `.gitignore` (my owned paths)

| Item | Status | What changed |
|------|--------|--------------|
| README usdcat smoke (C-6 spillover) | **FIXED** | The smoke section falsely claimed `bin/_env.sh` puts `$USD/bin` on `PATH` (it does not — no PATH export in the file). Replaced with the exact self-contained `export USD/PXR_PLUGINPATH_NAME/LD_LIBRARY_PATH` + `"$USD/bin/usdcat" --flatten …` block, which I executed and confirmed (prints `def UsdGenGroom "Groom"`, `def UsdGenDescription "Description"`, rc 0). Note added that `usdview`/`usdrecord`/`testusdview` must be launched via `"$PY"`. |
| README "Test tiers and CI" | **FIXED** | Now describes the actual six-job graph, the tarball transfer, the cache key shape, and the t0-t1 by-name bridge for the two unlabelled link-rule tests. |
| .gitignore | **FIXED** | Added `out/` (T4 artefact tree the protocol writes to) and the exact probe binary path `docs/prework/probes/m0-usdcat-extent/probe_usdcat_extent` (extensionless, so the existing `*.so`/`*.o` patterns don't catch it). |

## Files changed

1. `.github/workflows/usdgen.yml` — C-1, C-2, C-3, C-5 (C-4 verified compliant)
2. `docs/workstation-protocol.md` — C-6, C-7, C-8, C-9, C-10 (full rewrite of
   commands/env/gate-mapping/numbering; section structure §1–§11 unchanged)
3. `docs/prework/probes/m0-usdcat-extent/probe_usdcat_extent.cpp` — C-11
   (type-registration assertions; post-stage-open ordering with rationale)
4. `docs/prework/probes/m0-usdcat-extent/probe_usdcat_extent` — rebuilt binary
   (gitignored)
5. `docs/prework/m0-usdcat-extent-evidence.md` — C-11, C-12, C-13 (rewritten
   with fresh verbatim runs, valid shell, real exit codes, ldd, PLUG_LOAD
   register-vs-load contrast)
6. `README.md` — smoke-test commands corrected; CI section updated
7. `.gitignore` — `out/` + probe binary

## Open items (not in my owned paths — for gatekeeper/other lanes)

- **CMake lane**: add `LABELS "T0;build;gate:B-1"` to all three
  `testUsdGenLinkRule_*` tests (currently only the `usdGen` variant is
  labelled, `CMakeLists.txt:219-244`). The CI by-name bridge can be deleted
  from `t0-t1` afterwards.
- **CMake lane**: `cmake/usdGenConfig.cmake.in` uses
  `set_and_check(usdGen_PLUGIN_DIR "@PACKAGE_USDGEN_INSTALL_PLUGINDIR@")` but
  `configure_package_config_file()` is called without `PATH_VARS
  USDGEN_INSTALL_PLUGINDIR USDGEN_INSTALL_LIBDIR` — `find_package(usdGen)`
  from a consumer will fail with empty `set_and_check` values (SOL C-4
  evidence). Blocks any future installed-tree consumer test.
- **CMake lane**: register `testUsdGenInstallTree`, `testUsdGenConsumer`,
  `testUsdGenPluginDiscovery` against the installed tree (plan §6.2's full
  install-check; SOL C-4). The workflow header comment points at this.
- **Gatekeeper**: `sol-m0-exit-notes.md` was SOL UNAVAILABLE — the M0 exit
  checklist (X-1…) could not be verified or executed.
- **M7**: T4 probe scripts referenced by the protocol
  (`tests/python/t4/t4StrokeProbe.py`, `t4PickProbe.py`, `t4Probes.py`) and
  the `bench/*.usda` canonical scenes do not exist yet; protocol sections are
  PENDING M7 runner with M0 smoke substitutes and `test -f` guards, so no
  section is unexecutable on paper.

## Verification performed

- Workflow YAML parses (`yaml.safe_load`); job graph `openusd →
  {configure-offline, build-release → t0-t1, install-check, build-fpfast}`.
- All build_usd.py flags grep-verified in `<openusd-src>` (v26.08).
- All protocol env vars grep-verified in OpenUSD source:
  `USDVIEW_ENABLE_MSAA` (usdviewq/stageView.py:839),
  `HDX_DISABLE_ALPHA_TO_COVERAGE` (hdx/debugCodes.h:20),
  `HDST_ENABLE_HGI_RESOURCE_GENERATION` (hdSt/codeGen.cpp:166),
  `HGI_ENABLE_VULKAN` (hgi/hgi.cpp:24), `HD_ENABLE_PERFLOG`
  (imaging/hd/perfLog.cpp:25), `HDST_DUMP_FAILING_SHADER_SOURCE`
  (hdSt/debugCodes.h:26), `PLUG_LOAD`/`PLUG_REGISTRATION`
  (plug/debugCodes.cpp:16-17).
- Probe recompiled against `OpenUSD_26_08` with the task's link list plus
  `-lusd_python -lpython3.12` (the task's list alone fails to link — undefined
  `pxr_boost::python` symbol pulled in via `usd/stage.h` → `tf/pyLock.h`;
  documented in the evidence doc's Probe build section), then run: negative
  control exit 1 (types unknown), positive
  control exit 0 (types known+concrete, non-empty extent); `ldd` shows zero
  `libusdGen*` deps; `TF_DEBUG` logs show the probe dlopens
  `libusdGenSchema.so` where usdcat only registers it.
- README usdcat smoke command executed and passes.
