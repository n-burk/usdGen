# sol M0 re-verification notes
(Sandbox deviation: danger-full-access because host bwrap netns fails with RTM_NEWADDR EPERM; read-only discipline enforced by prompt.)
## Results
| item | PASS/FAIL | evidence (quoted, 1-3 lines) |
|---|---|---|
| 1 | PASS | “`NEEDED libusd_tf.so`; `TF_REGEX_RC=1`”<br>“`Gate B-1 VIOLATION: libusd_hd.so links forbidden library: libusd_tf.so`”<br>“`POSITIVE_RC=0` — `B-1 link check passed for libusdGen.so`” |
| 2 | PASS | “`B-1 include check passed (7 files scanned)`”<br>“`B-1 include gate correctly FAILED on forbidden fixture`”<br>“`100% tests passed, 0 tests failed out of 3`” |
| 3 | PASS | “`testUsdGenLinkRule_usdGen`, `usdGenMath`, `usdGenTestUtils`”<br>“`testUsdGenIncludeRule`; `testUsdGenNoThirdPartyExports`”<br>“`Total Tests: 9`” (five required gates plus four fixtures) |
| 4 | PASS | “`PXR_PLUGINPATH_NAME=...:/home/burkard/work/OpenUSD_26_08/lib/usd`”<br>“`ok: every UsdImaging/UsdSkel node...`; `ok: every observed Hdsi*/HdSt*/Storm renderer node...`”<br>“`ok: usdGen sits strictly before the synthetic hdPrman:motionBlur node`; `PASS (0 failures)`” |
| 5 | PASS | “`DEFINED_CPP_TYPE_ERRORS=0 CODING_ERROR_DIAGNOSTICS=0`”<br>“`USDGEN_ENABLE metadata names: instance=0 proxy=0`”<br>“`InstanceDataSourceNames()` and `ProxyPathTranslationDataSourceNames()` return empty vectors when `TfGetEnvSetting(USDGEN_ENABLE)` is false.” |
| 6 | PASS | “`Installing: /tmp/usdgen-sol-scratch/lib/cmake/usdGen/usdGenConfig.cmake`”<br>“`set_and_check(usdGen_PLUGIN_DIR "${PACKAGE_PREFIX_DIR}/lib/usd")`; `set_and_check(usdGen_LIBRARY_DIR "${PACKAGE_PREFIX_DIR}/lib")`”<br>“`100% tests passed, 0 tests failed out of 2`” |
| 7 | PASS | “`/home/burkard/work/OpenUSD_26_08/bin/usdcat`”<br>“`GEN=/home/burkard/work/usdGen GENBUILD=/home/burkard/work/usdGen/build PY=/home/burkard/.venv/bin/python3`” |
| 8 | PASS | “`PW_EGL_STATUS_MATCHES=0`”<br>“`.gitignore:23:docs/prework/probes/PW-egl/scene_*.usdc`”<br>“`.gitignore:24:docs/prework/probes/PW-egl/pw4_*.usda`” |
| 9 | PASS | “`STALE_LABEL_CLAIMS=0 ACTIVE_BY_NAME_BRIDGES=0 W_IDS=0`”<br>“Vulkan and Metal commands name `vulkan_001.png`/`compile_vulkan.log` and `metal_001.png`/`compile_metal.log`.”<br>“`YAML_PARSE=PASS`” |
| 10 | PASS | “`100% tests passed, 0 tests failed out of 13`”<br>“`T0 = 0.17 sec*proc (11 tests)`; `T1 = 0.09 sec*proc (2 tests)`” |
## Residual findings
None
## Summary
ALL P0s VERIFIED