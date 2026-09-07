# exec-docs-report-2 — docs/CI/env lane, round 2 (sol M0 exit notes)

Inputs: `docs/review/sol-m0-exit-notes.md` (X-1..X-10). This lane owns
`README.md`, `.github/workflows/usdgen.yml`, `docs/workstation-protocol.md`,
`bin/_env.sh`, `.gitignore`. No git commits (X-9/X-10 deferred to the
post-workflow commit step).

## Per-finding status

### X-3 (P0) — stale T0/bridge claims in README + workflow — APPLIED

Evidence: `CMakeLists.txt` labels all five gate B-1 tests `T0`
(`testUsdGenLinkRule_*` → `T0;build;gate:B-1` at CMakeLists.txt:231-232;
`testUsdGenIncludeRule` → `T0;build;gate:B-1` at 240-241;
`testUsdGenNoThirdPartyExports` → `T0;build` at 248-249). The by-name bridge
step in the `t0-t1` job was already deleted in round 1, but the NOTE block
that described it survived (workflow lines ~283-288), and README.md
carried two stale passages.

Changes:
- `README.md` Test section: the parenthetical "`testUsdGenLinkRule_usdGenMath`
  and `testUsdGenLinkRule_usdGenTestUtils` are not yet label-bearing"
  replaced with: all five gate B-1 tests carry `T0`, so the canonical
  `ctest --test-dir build -L '^T[01]$' -j8` run covers every M0 test; no
  by-name bridge.
- `README.md` "Smoke test" section: the note "`bin/_env.sh` … does *not*
  extend `PATH`" was stale after X-6; now says `source bin/_env.sh` sets the
  variables and puts `usdcat` on PATH (plan/10 §3.6).
- `.github/workflows/usdgen.yml` `t0-t1`: removed the obsolete NOTE block
  ("do not yet carry a T0 label … Delete the Run-2 step …"); the Run-1 step
  comment now states all five gate B-1 tests are `T0`-labelled so the single
  anchored run is complete.
- `README.md` CI section: `t0-t1` bullet no longer mentions a bridge run.

### X-5 (P0) — workstation protocol §5 not executable — APPLIED

Changes to `docs/workstation-protocol.md` §5 (rewritten; section numbering
untouched, still 11 sections, zero `W-[0-9]`):

- Backend-selection mechanism documented with citations:
  `_MakeNewPlatformDefaultHgi()` (`pxr/imaging/hgi/hgi.cpp:177-208`) →
  Linux `HgiGL` / macOS `HgiMetal`; `HGI_ENABLE_VULKAN=1`
  (`hgi/hgi.cpp:24`) overrides Linux to `HgiVulkan` and `TF_CODING_ERROR`s
  unless built with `-DPXR_ENABLE_VULKAN_SUPPORT=ON`
  (`build_scripts/build_usd.py --vulkan`, line 1919; requires `VULKAN_SDK`).
  Vulkan/Metal therefore run through the stock GL render engine
  (`usdrecord -r GL`); there is no separate Metal Hydra delegate in 26.08
  (Metal is the `hgiMetal` Hgi backend).
- Dev-host caveat made explicit: `/home/burkard/work/OpenUSD_26_08` ships
  `libusd_hgiGL.so`/`libusd_hgiInterop.so` only, no `libusd_hgiVulkan.so` /
  `libusd_hgiMetal.so`; on the dev host only Leg 1 (GL) runs.
- **Leg 1 (GL, mandatory smoke, dev host + all M7 runners)**: single
  headless `usdrecord -r GL -w 1920 -f 1:2` command with
  `HDST_ENABLE_HGI_RESOURCE_GENERATION=1 TF_DEBUG=HDST_DUMP_FAILING_SHADER_SOURCE`
  writing `out/t4/§5/storm_001.png`, `storm_002.png` and
  `out/t4/§5/compile_gl.log`. `mkdir -p out/t4/§5` included.
- **Leg 2 (Vulkan, Linux M7 runner)**: exact build prerequisite
  (`build_usd.py usd-vulkan … --vulkan` mirroring the CI `openusd` recipe,
  `export VULKAN_SDK=…`, `test -e usd-vulkan/lib/libusd_hgiVulkan.so` sanity
  check), then the exact headless capture
  `HGI_ENABLE_VULKAN=1 HDST_ENABLE_HGI_RESOURCE_GENERATION=1
  TF_DEBUG=HDST_DUMP_FAILING_SHADER_SOURCE "$PY" "$USD/bin/usdrecord" -r GL
  -w 1920 -f 1 "$FIX" "$GEN/out/t4/§5/vulkan_###.png"` teeing
  `out/t4/§5/compile_vulkan.log`. (The old interactive-viewport usdview
  launch is gone.)
- **Leg 3 (Metal, macOS M7 runner)**: exact macOS block with
  `export DYLD_LIBRARY_PATH="$USD/lib:$GENBUILD"`,
  `PXR_PLUGINPATH_NAME` = build-tree usdGen resources + `$USD/plugin/usd` +
  `$USD/lib/usd`, `test -e "$USD/lib/libusd_hgiMetal.so"` backend check, then
  `TF_DEBUG=HDST_DUMP_FAILING_SHADER_SOURCE "$PY" "$USD/bin/usdrecord" -r GL
  -w 1920 -f 1 … "$GEN/out/t4/§5/metal_###.png"` teeing
  `out/t4/§5/compile_metal.log`.
- Machine-verifiable usdGen shader-compile check: pass iff no
  `Failed to compile shader` / `Failed to link shader` marker
  (`pxr/imaging/hdSt/glslProgram.cpp:211,421` TF_WARN strings) in any
  `compile_*.log` produced on the host, with a copy-pasteable `for` loop;
  png existence reported per backend (skipped for legs the host cannot run).
- Every named artefact (`storm_001.png`, `storm_002.png`, `vulkan_001.png`,
  `metal_001.png`, `compile_gl.log`, `compile_vulkan.log`,
  `compile_metal.log`) now has a producing command and `mkdir -p`.

### X-6 (P0) — bin/_env.sh contract — APPLIED

`bin/_env.sh` rewritten to the plan/10 §3.6 contract:
- `export USD` (default `$(cd $GEN/..)/OpenUSD_26_08`, kept if pre-set),
  `export GEN` (repo root from the file's own location), `export GENBUILD`
  (`$GEN/build`), `export PY` (`${VENV:-/home/burkard/.venv}/bin/python3`);
  all honor existing environment values (overrides allowed, incl. `USD`).
- `PATH` prepends `$GENBUILD:$USD/bin`.
- `LD_LIBRARY_PATH` = `$USD/lib:$GENBUILD` + inherited; `DYLD_LIBRARY_PATH`
  twin set (the usdRig defect the plan flags, §8.3).
- `PXR_PLUGINPATH_NAME` = existing build-tree usdGen resources
  (`$GENBUILD/usd/usdGenSchema|usdGenImaging|usdGenShaders/resources`,
  `$GENBUILD/python/usdgen`, dir-guarded) + `$USD/plugin/usd` +
  `$USD/lib/usd` (both are real plugin roots in 26.08) + inherited;
  build-tree first so the generated LibraryPath-bearing plugInfo wins.
- `PYTHONPATH` = `$GENBUILD/python` + OpenUSD site-packages (gated on
  presence; empty in a `--no-python` prefix).
- `USDGEN_IMAGING_DLL` exported when the build .so exists.
- `usdgen_require_usd` / `usdgen_require_python` helpers kept.

Verification (clean shell, `env -i`):
```
$ cd /home/burkard/work/usdGen && env -i /bin/bash -c 'source bin/_env.sh && command -v usdcat && echo GENBUILD=$GENBUILD'
/home/burkard/work/OpenUSD_26_08/bin/usdcat
GENBUILD=/home/burkard/work/usdGen/build
```
Additionally: `usdcat --version` runs after sourcing; exports confirmed:
`PXR=…/build/usd/usdGenSchema/resources:…/build/usd/usdGenImaging/resources:…/OpenUSD_26_08/plugin/usd:…/OpenUSD_26_08/lib/usd`;
`LDP=…/OpenUSD_26_08/lib:…/usdGen/build`; PATH head
`…/usdGen/build:…/OpenUSD_26_08/bin:…`; `USD`/`PY` exported.

### X-7 (P0) — .gitignore hygiene — APPLIED

Added:
```
scratch/
docs/prework/probes/PW-egl/scene_*.usdc
docs/prework/probes/PW-egl/pw4_*.usda
```
Verification:
- `git check-ignore -v` matches all 7 giants (scene_100k_A/B.usdc,
  scene_200k_B.usdc, pw4_both/pw4_glslfx_ctx_only/pw4_glslfx_only/
  pw4_mtlx_only.usda) and `scratch`;
- `git check-ignore` on the kept files (make_pw_scenes.py,
  make_pw4_scenes.py, results_*.txt, pw4_*.png, pw4_diffs.txt,
  usdGenHairPreview*.glslfx, small_test.usdc, trivial.usdc/.png) returns
  nothing → they remain untracked-and-commit-able;
- `git status --porcelain --untracked-files=all | grep PW-egl` no longer
  lists any `scene_*.usdc` or `pw4_*.usda`.

### X-4 (P0, CI half) — install-check runs installed-tree tests — APPLIED

- `docs/review/exec-code-B-report.md` does not exist yet (the code-lane B
  round has not landed; `tests/` contains only `testUsdGenChainOrder.cpp`
  and `testUsdGenPluginDiscovery.cpp`, and `CMakeLists.txt` registers no
  `testUsdGenInstallTree` / `testUsdGenConsumer`). Per the lane brief the
  env var is **`USDGEN_INSTALL_PREFIX`** — noted in a workflow comment for
  code-lane coordination.
- New step in the `install-check` job, after `cmake --install build
  --prefix …/usdgen-install` (scratch prefix = `${{ github.workspace }}` +
  `/usdgen-install`): `Run installed-tree tests`
  - `env`: `USDGEN_INSTALL_PREFIX=…/usdgen-install`;
    `PXR_PLUGINPATH_NAME` = scratch `lib/usd/usdGenSchema|usdGenImaging/
    resources` + stock `${{ github.workspace }}/usd-install/plugin/usd` +
    `/usd-install/lib/usd`; `LD_LIBRARY_PATH` = stock `usd-install/lib` +
    the usdGen build tree.
  - `run`: `ctest --test-dir build -R 'testUsdGen(InstallTree|Consumer)$'
    --output-on-failure`; a non-zero ctest exit fails the step. While the
    tests are unregistered, ctest prints "No tests were found" and the step
    prints an explicit WARNING instead of failing — the step becomes the
    real gate automatically once the code lane registers the tests (no CI
    edit needed then).
- Layout verification and the plugInfo.json python3 validation are
  unchanged (kept, as required).

### X-9 / X-10 — NO ACTION (per lane brief; the commit happens after this
workflow; no git commits made here). X-10 is P2 — recorded only.

## Validations (all run after the edits)

```
$ python3 -c "import yaml; d=yaml.safe_load(open('.github/workflows/usdgen.yml')); print(list(d['jobs']))"
['openusd', 'configure-offline', 'build-release', 't0-t1', 'install-check', 'build-fpfast']
$ grep -cE '^## [0-9]+\.' docs/workstation-protocol.md; grep -cE 'W-[0-9]' docs/workstation-protocol.md
11
0
$ env -i /bin/bash -c 'source bin/_env.sh && command -v usdcat && echo GENBUILD=$GENBUILD'
/home/burkard/work/OpenUSD_26_08/bin/usdcat
GENBUILD=/home/burkard/work/usdGen/build
$ git check-ignore -v docs/prework/probes/PW-egl/scene_100k_A.usdc … (7 giants + scratch)
.gitignore:23 / .gitignore:24 / .gitignore:55 matches on all
$ git check-ignore docs/prework/probes/PW-egl/{make_pw_scenes.py,results_s8.txt,pw4_both.png,usdGenHairPreview.glslfx,small_test.usdc}
(no output → still committable)
$ env LD_LIBRARY_PATH=/home/burkard/work/OpenUSD_26_08/lib ctest --test-dir build -L '^T[01]$'
100% tests passed, 0 tests failed out of 7   (code untouched; baseline state)
```

## Notes for the code lane

1. `USDGEN_INSTALL_PREFIX` is the name used by the new `install-check`
   step; if `testUsdGenInstallTree` / `testUsdGenConsumer` land under a
   different env var, the workflow's `env:` block is the single place to
   rename (comment in the file marks it).
2. `testUsdGenNoThirdPartyExports` currently carries `T0;build` without
   `gate:B-1` (X-1, code-lane owned); the README/workflow wording says "all
   five gate B-1 tests carry `T0`", which is true regardless.
3. Protocol §5 Leg 2/3 commands assume `"$PY" "$USD/bin/usdrecord"` —
   consistent with the 26.08 fact that usdrecord is a Python script with a
   baked-in shebang (see "Common environment" facts at the top of the file).
