# M1 checkpoint — Wave 4 (2026-09-09)

## Verified baseline (this session, `build/`, Release)
- Full build green; `libusdGenImaging.so` links (ninja no-op).
- `ctest -L '^T[01]$'`: 24/27 pass; skipped: SchemaUpToDate, TileContract (SI-1), Population (SI-6), Adapter (SI-7), RestAdapter (SI-7), Contracts (C1/C2/C5) — stubs.
- FAIL `testUsdGenChainOrder` (SI-5): dlopen `libusdGenImaging.so` → undefined symbol `UsdGenGroomSceneIndex::New(const TfRefPtr<HdSceneIndexBase>&, int)`. Root cause: class declared in `groomSceneIndexPlugin.h` (170 lines) with ZERO member definitions in `groomSceneIndexPlugin.cpp` (54 lines, plugin-registration half only).
- FAIL `testUsdGenGraph` (E-6): recompile 0.757 ms > 0.2 ms; rebuilt-nodes==1 passes.
- FAIL `benchUsdGenChain`: E-6 compile fails — fixture `MakeChain` uses `UsdGenClump` (unregistered in M1); E-1 23.66 ms > 6.4; E-7 0.523 < 3.0. E-8 bitwise passes.
- Hivemind reachable: `http://hivemind.local:1235/v1` → 200.

## Roster (Wave 4)
- **ImagingCore** — local `task` — owns `libs/usdGenImaging/usdGenImaging/**`. Accept: `testUsdGenChainOrder` exit 0 (plugin dlopens clean), SI-2/3/4/8 stay green. Build: `build/`, affected targets only.
- **EngineCore** — `task_remote` (Hivemind) — owns `libs/usdGen/**` + `tests/perf/benchUsdGenChain.cpp`. Accept: with USDGEN_GATE=1, `testUsdGenGraph benchUsdGenChain benchUsdGenSparse testUsdGenChainDeterminism testUsdGenKernelDeterminism` exit 0. Build: `build-engine/` (Release) ONLY.
- **SchemaRegistry** — `task_remote` (Hivemind) — owns `libs/usdGenSchema/**`, `plugin/usdGenSchema/resources/**`, `usdGenShaders/**`, `docs/freezes/C1.md`, `C5.md`. No cmake builds; checks via `bin/gen_schema.sh` + `checkC5.py` + usdcat.

- **EngineCore** — `task_remote` — FAILED (Hivemind context limit). Replaced by **EnginePerf** — `large-context-task-executor` (local, 1M ctx) — owns `libs/usdGen/**`; fixture Clump→Grow fixed by coordinator. Build: `build-engine/` ONLY.
- **TestWriter** — local `task` — owns the 5 stub test .cpp files (TileContract SI-1, Population SI-6, Adapter + RestAdapter SI-7, Contracts C1/C2/C5). No ninja; compile via compile_commands.json. Contracts must go green today; others honest-FAIL until impls land.
- **Fixtures** — `task_remote` — owns `usd/fixtures/**` (+ optional data lines in tests/CMakeLists.txt). G1–G5 per plan/09 §0.3, generator reproducible byte-identical.
- `tests/**`, `CMakeLists.txt`, shared headers, generated interfaces → coordinator/integration owner (ImagingCore relays deltas).
- Engine ABI = current `libs/usdGen/usdGen/session.h` + C ABI; ImagingCore must NOT edit `libs/usdGen`.

## Verified gate state (coordinator, this session)
- SI-5 ChainOrder: FAIL — `New` symbol cascade. checkC5: 6/7 OK, Sdr stage fails on same symbol. Both blocked ONLY on scene-index impl → ImagingIndex (large-ctx) dispatched.
- Schema: schema.usda/generatedSchema/plugInfo edited by SchemaRegistry; checkC5 doc-vs-shader stages green. Regen idempotence NOT yet re-verified.
- Engine (USDGEN_GATE=1, build-engine): E-1 28.49ms (budget 1.5), E-7 0.65×, E-2 0.166ms FAIL (regressed vs green earlier — EnginePerf mid-edit suspect), E-6 0.826ms. EnginePerf resumed with corrected gate set (no testUsdGenChainDeterminism target).
- thirdparty/seexpr Noise.cpp +2 float template instantiations — benign/intended (CPU float noise), keep.
- Imaging lane (ImagingCore) & first remote EngineCore & SchemaRegistry settled mid-work; disk state above is authoritative, not their transcripts.

## Active roster
- ImagingIndex — large-context-task-executor (local) — `libs/usdGenImaging/**`, build/ — accept: ChainOrder green + checkC5 exit 0.
- EnginePerf — large-context-task-executor (resumed) — `libs/usdGen/**`, build-engine/ — accept: USDGEN_GATE=1 4-test set green, root-cause negative scaling first.
- TestWriter — task (local) — 5 stub test .cpps — Contracts green today, rest honest-FAIL.
- Fixtures — task_remote — `usd/fixtures/**` — G1–G5 + generator, byte-identical regen.

## Next actions
1. As lanes settle: rerun their exact acceptance commands (trust diffs+ctest, not reports).
2. Then wave 5: T2 Storm gates (L-1/L-2, GPU, quiescent), stub tests flip green with scene index, SchemaUpToDate stub.
3. Milestone-wide validation when all gates land; then commit M1 and open M2 (schema/lookdev/docs lanes prep only, engine lanes stay dark until M1 passes).

## Wave 4 progress (later session)
- N-5 `testUsdGenSchemaUpToDate`: IMPLEMENTED by coordinator, GREEN. Invokes real `bin/gen_schema.sh` (mirror-the-recipe variant drifted; deleted). gen_schema.sh:13 fixed `$USDGEN_ROOT`→`$GEN` (pre-existing bug; _env.sh exports GEN).
- Build-runner rule: ONLY coordinator runs cmake/ninja in `build/`. ninja = /home/burkard/.venv/bin/ninja; binaries at build/ root. TestWriter is write-only.
- T2 tier exists: tests #26–31 testUsdGenStormLook, benchUsdGenStorm, testUsdGenStormTangent, testUsdGenStormHgiResource, testUsdGenStormRefine, testUsdGenStormMaterial (stubs). Carries S-1/S-5/S-6/S-8/S-9/S-12/L-1/L-2.
- M1 exit gates (plan/11 §2.0): E-1,E-2,E-6,E-7,E-8; SI-1..SI-8; S-1,S-5,S-6,S-8,S-9,S-12; L-1,L-2.
- SchemaVerify: checker tests/checks/c1_registry_check.py green 43/43 types + 19/19 props; C1.md log append pending. NOTE: C1.md §1 API list = 3 *API schemas (Operator is auto-apply target, not a schema).
- interfaces.md §7 abstract-schema list WRONG vs C1.md (authoritative: abstract = Operator/Generator/Styler/Deformer/Map). Tile publisher entry = BuildTileDataSource(const UsdGenTilePublication&).
- TestWriter: first packet budget-exhausted (0 writes, contracts pinned); re-dispatched Contracts-only; awaiting "ready".
- Fixtures lane + .gitignore usd/fixtures/*.usda: done earlier.
- EnginePerf: running 45m+, reading E-6 context. ImagingIndex: running.

## Wave 4 progress (settles)
- C1 FREEZE COMPLETE: C1.md 42 property tables (all plan/02 §2 headings), checker green 44/44 types + 322/322 properties (verified by coordinator, rc=0). One documentary delta: UsdGenCollide offset/pushAmount/iterations live-non-uniform vs plan silent — frozen as live, noted. SchemaVerify lane closed.
- EnginePerf: zero edits so far, diagnosis: TfToken registry-lock contention (length.cpp:238 per-curve; ramp.cpp EvalOne x4 per LUT sample ~200k lookups/commit), per-chunk vector(257) alloc + dynamic_cast in SweepChunk, no tbb grainsize. Fix list greenlit in order 1-4; E-8 = per-pass regression gate. E-1 31.7ms vs 6.4 limit (bench), E-7 0.59, E-6 0.403, E-2 0.128-0.147 vs 0.10.
- Contracts: 8 uniformity fails remain; PrimDefinition::Property::GetMetadata("uniform")=false despite `uniform int` in schema — probing prim-level IsUniform().
- ChainOrder (SI-5) still red — ImagingIndex grounding APIs, no build attempt yet (my ctest probes don't trigger their build).
- Baseline sweep now: 22/27 pass + 3 skipped (Population/Adapter/RestAdapter) + 4 fail (ChainOrder=imaging, Graph+benchChain=engine, Contracts=iteration).

## Wave 4 settles (2)
- testUsdGenContracts GREEN (GetVariability pin; SdfVariability is the uniform encoding — no "uniform" metadata key in 26.08). C1 gate satisfied by test+checker.
- EnginePerf: passes 1-3 yielded nothing (token interning ~neutral, LUT hoist −4ms noise, scheduler pass-3 REVERTED). Hardware scales (3.2-4.9x on memcpy/compute probes; nproc=20). Stale-binary trap: direct build-engine binary runs link build/libusdGen.so via _env.sh PATH — authoritative = `USDGEN_GATE=1 ctest -V --test-dir build-engine`. Current: E-1 31.5, E-7 0.704, E-6 0.405, E-8 PASS. Stage-timer instrumentation pass running.
- ImagingIndex: two budget-exhausted runs, ZERO writes; design lock fully recorded (see its 1579739 message). Third run = forced write chunks A/B/C; watch wc -l groomSceneIndexPlugin.cpp.
- Population SI-6 test packet dispatched to TestWriter.
- T2 tier binary state: 6 registered tests (#26-31) — statuses being enumerated (bg_14).
Reconcile deltas; implement stub tests (TileContract SI-1, Population SI-6, Adapter/RestAdapter SI-7, Contracts) via next wave; then milestone-wide T0–T3 validation.
## Wave 5 (this session)
- **SI-5 GREEN**: groomSceneIndexPlugin.cpp chunks A+B (54→~540 lines) build clean; testUsdGenChainOrder Passed. Chunk C (fallback-parent + source-name override) pending status.
- **Engine gates ALL GREEN** in build/: benchUsdGenChain Passed (E-1 2.8-2.9ms, E-7 5.7-7.7, E-8 bitwise); KernelDeterminism/benchUsdGenSparse/Scheduling/SnapshotRace Passed. E-1 root cause = scheduler MISSING post-sweep dirty-byte clear (accumulated chunkDirty ⇒ every commit full-dirty); fix = commit-thread fill(None) per 03 §5.4 + inline ≤4-chunk sparse fast path. E-6 LAST ITEM: testUsdGenGraph single-shot 0.265ms vs 0.2 limit (EnginePerf quieter runs 0.176-0.190) — final attribution/warmup pass running.
- **SEGFAULT SCAR**: 6 engine tests segfaulted for hours = STALE EXECUTABLES. A failing target (testUsdGenPopulation) aborts ninja before relinking anything else; `cmake --build build --target usdGen` does NOT refresh inlined ctors/dtors in test executables. Fix: rebuild the six targets EXPLICITLY, never trust sweeps after any FAILED build.
- **C5 CHECK: PASS** (usdGenShaders/test/checkC5.py with ITS OWN env; my hand-set PXR_PLUGINPATH_NAME caused false FAIL — schema plugin discovery needs $GENBUILD/usd on path, not $GENBUILD).
- Contracts/SchemaUpToDate green. Baseline sweep: all pass + 10 stub-skips.
- **Roster**: ImagingIndex (chunk C pending), EnginePerf (E-6 final pass), TestWriter (Population SI-6; 1 build error relayed: UsdPrimRange::Count is v2-only), StormT2 (task_remote — S-gates).
- **STORM FACT**: "Storm" = OpenUSD's own hdStorm plugin + hdSt/hdx/hgiGL/usdImagingGL, installed at $USD (NO vendored repo needed). prototypes/ + research/ trees were NEVER committed (git ls-tree HEAD: only .github bin cmake docs libs plan plugin tests testutils thirdparty). Docs census must fix those citations.
- StormT2 harness recipe relayed: EGL dlopen + compatibility profile + pbuffer per plan/10 §T2; UsdImagingGLEngine + GetRenderStats for S-5; SetRendererSettings refine tokens for S-9; HDST_ENABLE_HGI_RESOURCE_GENERATION=1 env for S-8.
# Next
1. Population fix round → build → SI-6. 2. ImagingIndex chunk C status. 3. StormT2 functional bodies → build (may need usdImagingGL link libs — coordinator owns CMakeLists). 4. TileContract SI-1 + Adapter/RestAdapter SI-7 packets next (TestWriter after Population). 5. Full gated sweep → docs census → commit M1.
