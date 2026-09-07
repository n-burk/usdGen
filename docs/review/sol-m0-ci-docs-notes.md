# sol CI/docs validation notes
## Findings

### [P0] C-1: OpenUSD build script path is wrong
- Location: .github/workflows/usdgen.yml:111
- Issue: The first uncached push will fail before configuring OpenUSD because it runs `python3 build_usd.py` from the checkout root.
- Evidence: OpenUSD v26.08 contains `build_scripts/build_usd.py`, not a root-level `build_usd.py`. The selected feature flags themselves are correct: `--usd-imaging` enables `PXR_BUILD_IMAGING` and `PXR_BUILD_USD_IMAGING`; v26.08 defaults `PXR_ENABLE_GL_SUPPORT=ON`, producing Storm/`hdSt` and `hgiGL`; `--no-python --no-usdview` leaves the C++ `usdcat` and `usdAppUtils` targets available.
- Recommended fix: Change line 111 to `python3 build_scripts/build_usd.py \`.
- Why: On a cache miss—the guaranteed state for the first push—the shell exits with “can't open file …/build_usd.py”.

### [P0] C-2: OpenUSD artifact loses executable permissions
- Location: .github/workflows/usdgen.yml:147
- Issue: Even after fixing C-1, the downloaded `usdcat` will not be executable.
- Evidence: The prefix is uploaded as a directory and later executes `usd-install/bin/usdcat` at line 201. GitHub documents that artifact upload normalizes files to mode `0644`, losing executable bits: [actions/upload-artifact permission-loss documentation](https://github.com/actions/upload-artifact/blob/main/README.md#permission-loss).
- Recommended fix: Before upload, run `tar -C usd-install -cf usd-install.tar .`; upload that tar file. After each download, run `mkdir -p usd-install && tar -C usd-install -xf usd-install.tar`.
- Why: The `usdcat smoke` step will otherwise fail with permission denied, so a corrected first push remains red.

### [P1] C-3: Required every-push CI graph is not implemented
- Location: .github/workflows/usdgen.yml:162
- Issue: There is no `configure-offline` job, no independently gated `build-release` job, and no dedicated T0+T1 job as specified by plan §6.2.
- Evidence: `usdgen-build-test` combines configure, build and tests without `-DFETCHCONTENT_FULLY_DISCONNECTED=ON`. Its required `ctest -L '^T[01]$' -j8` runs only five of seven tests because `testUsdGenLinkRule_usdGenMath` and `testUsdGenLinkRule_usdGenTestUtils` lack labels in `CMakeLists.txt:218-228`; the workflow compensates by rerunning every test.
- Recommended fix: Add separate `configure-offline`, `build-release`, and `t0-t1` jobs with the exact plan commands. Inside the CMake `foreach`, give every link-rule test `LABELS "T0;build;gate:B-1"` and remove the unfiltered second `ctest`.
- Why: The present workflow neither proves disconnected configuration nor makes the canonical label command sufficient.

### [P1] C-4: Install-check can pass with a broken package
- Location: .github/workflows/usdgen.yml:259
- Issue: `install-check` performs only layout/JSON inspection instead of the required installed-tree `testUsdGenInstallTree`, `testUsdGenConsumer`, and `testUsdGenPluginDiscovery`.
- Evidence: Those tests are not registered in the current CMake. More seriously, `configure_package_config_file()` at `CMakeLists.txt:306` omits `PATH_VARS`, so the generated config contains `set_and_check(usdGen_PLUGIN_DIR "")` and `set_and_check(usdGen_LIBRARY_DIR "")`. The workflow does not consume the package and therefore misses this failure.
- Recommended fix: Add `PATH_VARS USDGEN_INSTALL_PLUGINDIR USDGEN_INSTALL_LIBDIR` to `configure_package_config_file`; implement/register the three installed-tree tests, then run them with both `CMAKE_PREFIX_PATH`/`USD_INSTALL_DIR` and plugin/library paths pointing exclusively at the scratch installation.
- Why: The current job can report green while `find_package(usdGen)` fails immediately for downstream consumers.

### [P1] C-5: Ref-keyed cache aliases different OpenUSD recipes
- Location: .github/workflows/usdgen.yml:93
- Issue: The cache is correctly keyed on the resolved v26.08 commit, but not on architecture or the build-option recipe.
- Evidence: The key is `openusd-${runner.os}-${sha}`. Changing Python, imaging, MaterialX, GL, shared-library, or dependency options while retaining v26.08 restores the old prefix and skips the build.
- Recommended fix: Include `${{ runner.arch }}` and an explicit recipe epoch, for example `openusd-${{ runner.os }}-${{ runner.arch }}-${sha}-usdimg-gl-nopy-v1`, bumping the epoch whenever the invocation changes.
- Why: Ref identity alone does not identify the binary contents being cached.

### [P1] C-6: Protocol tool commands are not launchable from the declared setup
- Location: docs/workstation-protocol.md:35
- Issue: Every section invokes bare `usdview`, `usdrecord`, or `testusdview`, but the actual `bin/_env.sh` does not add `$USD/bin` to `PATH`.
- Evidence: `_env.sh` sets `USD`, `PY`, plugin and library paths only. The protocol comment claiming it sets the complete tool environment is false, and plan §3.6 requires OpenUSD Python tools to be invoked through `"$PY"`.
- Recommended fix: Use `"$PY" "$USD/bin/usdview"`, `"$PY" "$USD/bin/usdrecord"`, and `"$PY" "$USD/bin/testusdview"` throughout, or amend `_env.sh` to export `PATH="$USD/bin:$PATH"` and make the protocol explicitly depend on that change.
- Why: Exact commands must not rely on an undocumented caller `PATH`, especially because installed script shebangs may name the build machine’s interpreter.

### [P1] C-7: R-2 does not actually toggle MSAA or alpha-to-coverage
- Location: docs/workstation-protocol.md:129
- Issue: Section 3 uses a nonexistent `HD_ENABLE_SAMPLE_ALPHA_TO_COVERAGE` variable and claims 1×/4× MSAA can be toggled in viewport settings.
- Evidence: OpenUSD 26.08 reads `USDVIEW_ENABLE_MSAA` at `stageView.py:839-842`, selecting no MSAA or four samples at process startup. Alpha-to-coverage is disabled through the `HDX_DISABLE_ALPHA_TO_COVERAGE` TfDebug symbol; the documented `HD_ENABLE_SAMPLE_ALPHA_TO_COVERAGE` name does not occur in v26.08.
- Recommended fix: Specify separate launches using `USDVIEW_ENABLE_MSAA=0`, `USDVIEW_ENABLE_MSAA=1`, and `TF_DEBUG=HDX_DISABLE_ALPHA_TO_COVERAGE USDVIEW_ENABLE_MSAA=1`, followed by the translucent/OIT launch.
- Why: As written, the supposed A/B changes no OpenUSD state, invalidating release gate R-2.

### [P1] C-8: Multiple protocol commands remain placeholders or omit required cases
- Location: docs/workstation-protocol.md:163
- Issue: Despite every section having an environment and artefact field, several commands are not exact or cannot produce the requested artefacts.
- Evidence: Section 4 uses `usdrecord -f 1` without the required `###` output placeholder; only `out/t4` is created while commands write under `r1`, `r4`, and `r5`; §§6 and 10 retain `<…>` fixture placeholders; §7 retains `/path/to/rman` and does not identify an OpenUSD build containing hdPrman; §8 does not pass `--viewportSize 3840 2160`; §9 invokes only 100k despite requiring 10k/100k/1M; §11 substitutes a 4k fixture for the specified 10k groom; §5 gives no exact Vulkan launch such as `HGI_ENABLE_VULKAN=1`.
- Recommended fix: Replace every placeholder with an M7-runner path/variable that has an explicit validation command, create all output directories, correct the `usdrecord` frame format, and provide loops over the exact required scene sizes and backends.
- Why: A human cannot execute the document verbatim or reproduce all requested artefacts.

### [P1] C-9: RC-4 is mapped to the wrong section
- Location: docs/workstation-protocol.md:91
- Issue: The document says the ≥30 fps 1080p comb-stroke criterion RC-4 is read from §2.
- Evidence: Section 2 measures camera tumbling only. The synthetic comb stroke and presented-frame timing are in §9, matching `plan/08-tools.md` §8.3.
- Recommended fix: Map RC-4 to §9, use §2 only for its static/tumble baseline, and require §9’s artefact table to include derived FPS at 1080p.
- Why: The current mapping permits RC-4 to be signed off without running a stroke.

### [P1] C-10: Retired W identifiers still appear literally
- Location: docs/workstation-protocol.md:10
- Issue: The document states there are no `W-` identifiers anywhere while spelling out both `W-` and `W1`–`W4`.
- Evidence: Lines 11-13 contain the prohibited forms, notwithstanding their “retired” context.
- Recommended fix: Replace the passage with “There is no alternate identifier scheme; all legacy workstation identifiers are retired,” without reproducing those identifiers.
- Why: This violates the plan’s literal single-numbering/no-W-ID rule and defeats mechanical validation.

### [P0] C-11: Flattened type names do not prove schema resolution
- Location: docs/prework/m0-usdcat-extent-evidence.md:117
- Issue: Run 3 treats preserved authored type-name tokens as proof that the codeless schemas resolved.
- Evidence: The fixture authors `def UsdGenGroom` and `def UsdGenDescription`, so flattening can preserve those names without registration. The document’s own negative control excludes the schema plugin yet still prints `prim type = UsdGenDescription` at line 78.
- Recommended fix: Retain the `usdcat` output as a smoke test, but extend the stock-only probe to assert `!TfType::FindByName(...).IsUnknown()` and `UsdSchemaRegistry::IsConcrete()` for both `UsdGenGroom` and `UsdGenDescription`, with negative and positive plugin-path controls.
- Why: One half of the stated M0 proof currently cannot distinguish successful Plug type registration from an unknown authored type token.

### [P0] C-12: PLUG_REGISTRATION output is not dlopen proof
- Location: docs/prework/m0-usdcat-extent-evidence.md:172
- Issue: Run 4 claims that `TF_DEBUG=PLUG_REGISTRATION` proves `usdcat` loaded `libusdGenSchema.so`.
- Evidence: In OpenUSD v26.08, that message is emitted when Plug constructs/registers plugin metadata (`plugin.cpp:110`); a library plugin is still marked unloaded. Actual loading emits `Loading plugin '…'` under `PLUG_LOAD` (`plugin.cpp:203-240`). Flattening need not request an extent and therefore need not dlopen the schema library.
- Recommended fix: Run the stock-only extent probe with `TF_DEBUG=PLUG_REGISTRATION,PLUG_LOAD`, capture both the registration and `Loading plugin 'usdGenSchema'` messages, and optionally add `LD_DEBUG=libs` or a loaded-object check.
- Why: The positive extent result strongly implies the callback loaded, but the advertised debug proof is technically false and does not satisfy the stated evidence requirement.

### [P1] C-13: Displayed commands cannot produce the claimed verbatim outputs
- Location: docs/prework/m0-usdcat-extent-evidence.md:24
- Issue: The environment block is not valid shell, and Runs 1-3 show exit-marker lines absent from their displayed commands.
- Evidence: Assignments such as `PXR   = /home/...` are syntactically invalid. Commands at lines 68-71, 92-95, and 120-123 never print `r1_exit`, `r2_exit`, or `r3_exit`, although those strings appear inside the claimed verbatim output.
- Recommended fix: Replace the environment block with valid quoted shell assignments. Rerun each command through `tee`, capture `rc=${PIPESTATUS[0]}` where applicable, print the recorded exit value explicitly, and include `readelf -d`/`ldd` output demonstrating that the tested probe has no `libusdGenSchema` dependency.
- Why: The underlying positive/negative extent outputs are plausible and the link line names only stock dependencies, but the published transcript is not reproducible exactly as written.

## Summary

P0: 4, P1: 9, P2: 0; CI: INVALID; protocol: INVALID; extent evidence: INVALID