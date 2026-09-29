> Historical engineering note. It records a review or probe, not the current product overview. Start at the [repository README](../../README.md) and [docs index](../README.md).

# sol M1 schema/shaders/build validation notes

> Sandbox deviation: invoked with `-s danger-full-access` (host bwrap netns is broken, so the
> read-only sandbox cannot start). Sol's read-only discipline came from its prompt, which
> forbids modifying the repo; no build or mutating command was observed in the session log
> (/tmp/sol-m1-schema.log).

## Findings

### [P0] N-1: C1 and C2 freezes are absent
- location: `docs/freezes/C1.md:1`; `docs/freezes/C2.md:1`; `docs/m1/interfaces.md:1`
- issue: Neither C1 nor C2 exists, and the referenced M1 interface contract is also missing.
- evidence: `docs/freezes/` contains only `C5.md`; numerous M1 sources cite the three nonexistent files. Roadmap §2.2 requires C1, C2, and C5 to freeze at M1.
- fix: Add C1 with literal property/type/default/uniformity/token/dirty-class data and mask arithmetic; add C2 with the complete tile payload and exact R21 formulas/worked example; restore `docs/m1/interfaces.md` with the stats placement and pinned M1 interfaces.
- why: Two of three M1 freezes cannot be reviewed or enforced, and the stats block cannot be validated against its claimed authority.

### [P1] N-2: Schema does not match the normative property registry
- location: `libs/usdGenSchema/schema.usda:43`
- issue: Container properties and two M1 ramp properties diverge from `plan/02-schema.md`.
- evidence: `UsdGenGroom` declares non-uniform `int usdGen:schemaVersion` and `uniform token usdGen:sessionId = "default"` instead of `uniform int` and `uniform string = ""`, and omits `densityScale`, `renderDensityScale`, and `label`. `UsdGenDescription` omits most §2.3 properties. `UsdGenNoise` and `UsdGenWidth` omit the required `usdGen:noise:magnitude:interpolation` and `usdGen:width:interpolation`; §2.7 lines 598–600 requires an interpolation token for every knots property.
- fix: Bring Groom, Description, Noise, and Width into exact agreement with §2.2, §2.3, and §2.7, then regenerate all schema resources and encode the result in C1.
- why: R7 makes `plan/02-schema.md` the single normative registry; these are artist-visible C1 properties.

### [P0] N-3: All newly registered M1 tests are non-tests
- location: `tests/testUsdGenGraph.cpp:3`
- issue: All 15 requested M1 test and benchmark executables only print `M1-STUB` and return 77.
- evidence: Every listed source has a one-line stub at line 3. CTest’s show-only JSON confirms none has `SKIP_RETURN_CODE`; therefore 77 is a failure, despite `CMakeLists.txt:387` claiming it becomes “Not Run.”
- fix: Replace every stub with its specified assertion or benchmark before M1 exit. Do not mark gate tests skipped.
- why: No M1 schema, C1/C2/C5, engine, imaging, or Storm exit claim is currently tested.

### [P0] N-4: Four M1 exit-gate mappings are missing
- location: `CMakeLists.txt:370`; `CMakeLists.txt:437`
- issue: `SI-5`, `S-8`, `S-9`, and `L-1` are not selectable as gate tests.
- evidence: `testUsdGenChainOrder` has only `T1`, not `gate:SI-5`. The prescribed `testUsdGenStormTangent`, `testUsdGenStormHgiResource`, `testUsdGenStormRefine`, and `testUsdGenStormMaterial` targets do not exist. Plan §6.5 assigns these gates to M1.
- fix: Label chain order `gate:SI-5`; register and implement the four prescribed Storm tests with `T2` and their exact gate labels.
- why: `ctest -L 'gate:'` omits required M1 decisions and cannot represent milestone readiness.

### [P1] N-5: Schema regeneration has no up-to-date test
- location: `CMakeLists.txt:385`
- issue: `testUsdGenSchemaUpToDate`, required by plan §5.6, is not registered.
- evidence: `ctest -N` lists 28 tests but no schema-up-to-date test; `testUsdGenContracts` is only a stub.
- fix: Register a T1 test that regenerates into temporary storage and byte/semantic-compares `generatedSchema.usda` and `plugInfo.json`.
- why: The checked-in generated resources can silently drift from the authoritative schema.

### [P1] N-6: Installed shader discovery layout is broken
- location: `CMakeLists.txt:516`
- issue: The install rule flattens the contents of `shaders/` into `usdGenShaders/resources/`, while `ShaderResources` points to `shaders`.
- evidence: The trailing-slash source is installed with destination `.../resources`, whereas the build tree correctly contains `resources/shaders/*`. Additionally, `cmake/usdGenConfig.cmake.in:11` and installed-tree/plugin-discovery tests expose only schema and imaging resource paths.
- fix: Install the directory to `.../usdGenShaders/resources/shaders`; add the shader resource directory to `usdGen_PLUGINPATHS`, plugin-discovery arguments, installed-tree assertions, and CI layout checks.
- why: Build-tree discovery may work, but an installed Storm process cannot locate `shaderDefs.usda` or the glslfx assets.

### [P1] N-7: Install/export layout contradicts the target table
- location: `CMakeLists.txt:484`
- issue: Internal `usdGenMath`, `usdGen_seexpr`, and `nanoflann` artifacts and headers are installed/exported despite plan §1.2 marking them non-installed or “never” installed at M1.
- evidence: The install target list includes all three; `tests/testUsdGenInstallTree.cpp:93` explicitly expects both static archives and third-party headers.
- fix: Make internal static dependencies private where required, remove them from the install/export set, and defer their public headers to the milestone specified by §1.2.
- why: This exposes implementation libraries as an unintended public ABI and violates the frozen install layout.

### [P1] N-8: CI remains an M0 workflow and does not document the M1 measurement host gate run adequately
- location: `.github/workflows/usdgen.yml:2`
- issue: CI still describes M0 scope and only mentions a generic manual `ctest -L T2`; it does not identify or provide a complete measurement host command for M1 gates S-1, S-5, S-6, S-12, and L-2.
- evidence: There is no `t2-egl` job. The referenced workstation protocol is a T4 document and contains no T2 CTest command. CI also retains stale claims that install-tree tests and PATH_VARS are absent.
- fix: Add a triggerable measurement host job or a copy-pasteable manual block with environment, build directory, `ctest -L '^T2$'`, and the five gate mappings; update the workflow header and stale install-check commentary.
- why: The required GPU gates have no reproducible M1 execution route.

### [P1] N-9: Shader and C5 documentation is malformed or contradictory
- location: `usdGenShaders/resources/shaders/usdGenHairPreviewTranslucent.glslfx:68`; `docs/freezes/C5.md:27`
- issue: The translucent shader comment says it draws in the opaque/A2C pass, contrary to its actual tag and plan §2.7. C5 also contains an empty fence at lines 27–29 and an unmatched fence at line 115.
- evidence: Runtime metadata correctly says `"materialTag": "translucent"`, which routes to OIT. `checkC5.py` treats lines 29 and 115 as its fence pair even though Markdown treats lines 27 and 29 as the pair.
- fix: Replace the translucent material-tag paragraph with the OIT ordering caveat from §2.7; remove the extra opening/closing fence so the frozen block has one valid fence pair.
- why: The shipped shader’s own documentation gives the wrong rendering semantics, and the contract document renders incorrectly.

### [P1] N-10: Repository hygiene does not satisfy the requested policy
- location: `.gitignore:1`
- issue: M1-generated shader test scenes are not ignored, compiled binaries are committed, and the current diff fails whitespace checking.
- evidence: `usdGenShaders/test/make_scene.py` writes `usdGenShaders/test/scenes/`, which has no ignore rule. Tracked artifacts include three ELF files under `plan/prototypes/usdrig-linux-build/consumer/build/` and two static archives under `tests/fixtures/gateLink*`. `git diff --check` reports trailing whitespace throughout generated `plugInfo.json`.
- fix: Ignore the shader scratch scene/output directory and nested prototype build trees; remove committed build products and generate gate archives from source during the build; normalize regenerated JSON whitespace.
- why: Host-specific binaries and generated scenes create noisy, non-portable commits.

## Verified OK

- Both schema USDA files parse with OpenUSD 26.08; all 14 current schema types and their `usdGen:*` names/types/uniformity have source-to-generated parity.
- The five M1 operators exist and inherit `UsdGenOperator`; Groom inherits `UsdGeomImageable`, Description inherits `UsdGeomBoundable`, and the 12 M0 types remain present.
- The complete 22-property MaskAPI block is present in both schema representations.
- Effective build-tree schema metadata is `Type=library`, has a resolving `LibraryPath`, places `AutoApplyAPISchemas` inside `Info`, and marks Description `implementsComputeExtent=true`. Base-type auto-apply structurally covers all five derived M1 operators.
- Tile arithmetic helpers reproduce R21 exactly: 100,000 curves produce 196 chunks, four chunks per tile, and 49 tiles; the schema default is 64.
- `checkC5.py` passes: all three glslfx files have byte-identical 20-input C5 blocks matching the freeze and shader definitions.
- Variant A uses `inData.Neye`; Variant B declares and consumes `hairTangent`; tags are `defaultMaterialTag`, `translucent`, and `defaultMaterialTag`.
- `hairLook.usda` parses and contains the required `surface`, `glslfx:surface`, and `mtlx:surface` terminals.
- CMake declares the expected nine production targets, exports `usdGenShaders`, and applies `-ffp-contract` only to `usdGenMath`.
- The B-1 machinery remains intact: three link checks, one include check, one export check, four fixtures, and exactly five `gate:B-1` tests.
- All 15 requested M1 CTest names and their primary T0/T1/T2 labels are structurally registered.
- `usdGenConfig.cmake.in` still uses the corrected PATH_VARS substitutions.
- CI YAML parses successfully, and its T0/T1 selector remains exactly `ctest -L '^T[01]$' -j8`.
- No untracked-and-not-ignored file exceeds 1 MiB.
- `ctest -N` confirms all 13 M0 tests remain registered: five B-1 checks, four fixtures, chain order, plugin discovery, install-tree, and consumer.

## Summary

P0: 3, P1: 7, P2: 0; M1 schema/shaders/build: FAIL