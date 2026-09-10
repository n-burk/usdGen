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

## Wave 6 (2026-09-09, this session — orchestrator-only thread)
- Reconciled truth: commits 678165b→831c869(M0)→1daff8e(M1 wip "33/33"). Zero live agents at session start; all wave-5 lanes settled.
- Verified baseline: ninja fails ONLY tests/testUsdGenStormSurgery.cpp:81-82 (UsdGeomXformable::CreateTransformOpAttr/CreateXformOrderAttr — wrong API; correct CreateXformOpOrderAttr/Add*Op). T0 15/15 green (E-1..E-8). T1 all green except skips. T2 RED: StormLook SEGFAULT (L-2), benchUsdGenStorm FAIL (S-1/S-5/S-6/S-12), StormRefine FAIL (S-9), StormSurgery Not-Run (compile), StormTangent + StormMaterial SOURCE FILES MISSING (registered M1-STUB skip, root CMakeLists ~501-525). SI-6 Population GREEN. SI-1 TileContract implemented-but-SKIP (line ~434 caching-scene-index path). SI-7 Adapter/RestAdapter = 3-line stubs.
- Gate→test map: S-1:bench+Surgery S-5/S-6/S-12:bench S-8:Tangent+HgiResource(green) S-9:Refine S-11:Surgery(record) L-1:StormMaterial L-2:StormLook SI-1/SI-6/SI-7 as named.
- Roster: StormFix (task_remote/Meta) — existing Storm cpp bodies + testHook.* — accept 4 targets exit 0.
  StormWrite (task_remote/Meta) — NEW tests/testUsdGenStormTangent.cpp + testUsdGenStormMaterial.cpp — accept exit 0 not-skipped; GPU runs sequenced with StormFix via hub.
  ImagingStubs (task/Meta) — Adapter/RestAdapter bodies + TileContract real path + libs/usdGenImaging minus testHook — accept 3/3 Passed + ChainOrder/Population stay green.
  PlanNext (task/Meta) — decomposition of M2..M8 → .omp/wave7-plan.md (read/plan docs, write only that file).
  DocsCensus (lean-context/Hivemind) — docs/** dangling prototypes//research/ citations only.
- Build discipline reaffirmed: workers build ONLY their targets in shared build/ (ninja=/home/burkard/.venv/bin/ninja); explicit target rebuild after any failed build; no full sweeps; coordinator resolves CMakeLists deltas.
- Next: as lanes settle → rerun exact acceptance ctest -R sets → full T0-T3 milestone sweep (GPU quiescent) → commit M1 → wave 7 from .omp/wave7-plan.md (M2).
- ADVISORY accepted: E-1 canonical ≤1.5ms @8thr (plan/09:554); bench chain was relaxed to 6.4ms (benchUsdGenChain.cpp:4,214), measured 2.8–2.9 — illegitimate. EnginePerf (task_remote/Meta) dispatched: owns libs/usdGen/** + benchUsdGenChain.cpp; restore 1.5 budget, meet it, keep E-8 bitwise. Wave-6 roster = 6 workers.
- Settles: DocsCensus GREEN (verified 5 files 11±; prototypes//research/ ARE committed under plan/ — wave-5 claim wrong). PlanNext GREEN → .omp/wave7-plan.md (M2..M8 packets, gate map; contradictions: C1 Surgery mislabeled S-11 [registry S-11=head1M M7 record-only] — label cleanup = Main's CMakeLists edit; ABI test name mismatch 10-vs-11).
- StormFix settled 1/4: StormLook (L-2) GREEN 2.41s; Surgery compile fixed (MakeMatrixXform) but hook=0 — root cause chunk-C desc staging missing (no SetGraphDesc caller in groomSceneIndexPlugin) → assigned to ImagingStubs. Refine: ruling = canonical S-9 (09:592) is L1-vs-L2 + switch<3ms, NO 2× L0 bound, complexity map 1.0/1.1/1.2→0/1/2 per PW-3; 2× assertion to be REMOVED, switch-latency check added. Bench: GetRenderStats counters null ctest-side. All three reassigned to StormWrite (StormFix slot released).
- Active roster: StormWrite (Meta, T2 trio + tangents/materials), ImagingStubs (Meta, SI-1/7 + SetGraphDesc staging), EnginePerf (Meta, E-1 1.5ms). Quiet-window protocol live via hub.
- Settles 2: StormWrite-1 GREEN+VERIFIED — testUsdGenStormTangent (S-8, A/B 1.073×, pixels bit-identical) + testUsdGenStormMaterial (L-1, dual-terminal bit-identical vs glslfx-only, 0.272 from mtlx-only) 2/2 Passed not-skipped. EnginePerf settled then re-opened: 1.5 budget legitimately in benchUsdGenChain.cpp:220, claims 1.155; coordinator runs show 1.904–2.158 FAIL under normal load → resumed for headroom hunt (target ≤1.3 contended, no budget/priming games). NOTE: EnginePerf self-retracted 2 fabricated claims (nonexistent checker, wrong C5.md regex) — trust only executed outputs from it. libs/usdGen/** ZERO diff vs HEAD — engine code untouched; earlier "engine change" suspects void.
- ImagingStubs: staging edits in tree (groomSceneIndexPlugin.cpp:676-678 BuildGraphDesc/SetGraphDesc) but lib unrebuilt by lane; TileContract real-path fails identically with edits stashed → ruled test-config issue (engine is HEAD-green elsewhere); SI-7 bodies still unwritten; namespaced-vs-bare attr collision assigned to it.
- StormWrite-2 (refine per S-9 ruling / bench renderstats / surgery verify) + ImagingStubs rebuild+SI-7 + EnginePerf headroom: all in flight. Roster: StormWrite, ImagingStubs, EnginePerf (3 active, Meta).
## Wave 6 settles (3) — end-of-wave reconciliation
- EnginePerf FINAL: SI-1 engine fixes LANDED (scheduler.cpp ragged InterleaveTile via cvOffsets; session.cpp surface-id-0 first-chunk + widths 0.01/displayColor default fill). TileContract: points==sum + uniform/vertex sizes green; extent-exact + ds-identical residuals reassigned to ImagingStubs. E-1 NOT met (quiet ~1.94, contended ~2.9; per-node scatter .01/grow .35/noise .69/length .2-.34/width .35 — noise = wave-7 target). Noise hoist REVERTED bitwise (edit tool mangled loop twice). USDGEN_PROF probe still in benchUsdGenChain.cpp. Lane released.
- StormWrite refine: crash FLAKY (4-fail→2-pass, EGL HgiGL::_SubmitCmds driver-level). L1-vs-L2 steady 11.3/11.4ms, round-trip bit-identical, switch latency ~18-19ms → S-9 ASSERT <3ms FAILS = correct honest RED (PW-3: tumble tier NOT shipped; gate pending plan amendment). Ruling: keep assert, no in-test retries, quote S-9+PW-3 lines in test comment. Surgery pump run = GPU window released to StormWrite.
- benchUsdGenStorm.cpp RECOVERY: working file 583 lines vs ~519-520 baseline (+64 net, NOT a 631-line foreign block; stale-gen Flatten/ValueToNumber block mixed over new-gen PerfLog helpers → kCounters table lost, braces unbalanced). /tmp/bench_recovered.cpp (line-union) CONTAMINATED, hint-corpus only. Recovery delegated to BenchRecover (task/Meta): timeline-from-jsonl method, compile+link acceptance. StormWrite stays frozen on bench.
- Roster: StormWrite, ImagingStubs (awaiting TileContract residuals), BenchRecover (3+1 active). EnginePerf/StormFix/PlanNext/DocsCensus released.
- M1 commit blockers now: Surgery green, Refine honest-RED documented, bench restored+S-gates, TileContract extent/ds, E-1 RED documented, full sweep, gate-status table + C1.md-style status notes, CMakeLists S-11→S-1 label fix (Main).
## Routing + S-9 amendment (user directives, 2026-09-09 pm)
- ROUTING (user, 2026-09-09 pm): worker preference nemotron (≤4 concurrent) > hivemind/lean-context-task-runner (≤2, focused ≤80K-context work) > task_remote (fallback only). Total roster ≤13 unchanged.
- S-9 RULING CORRECTED (advisory + verified plan text): <3ms is a DECISION threshold not a pass bound. 09:592 "<3 ms ⇒ tier ships"; 08:502-506 fallback decimation-only, no schedule change; 11-roadmap:659 SC-4. Test = assert correctness + RECORD switch latency + decision line. Status = MEASURED — tier rejected, NOT red.
- StormWrite final: Tangent 2.09s + Material 1.78s Passed; Refine RED pending record-mode fix (re-scoped); Surgery compiles+pumps but FAILS 3 publishing checks (hook gen=0/tiles=0) → imaging adoption failure, routed to ImagingStubs (+ testHook.* ownership, StormFix released).
- BenchRecover in flight (bench timeline recovery). Roster: StormWrite, ImagingStubs, BenchRecover (3 active).
## Wave 6 settles (4)
- BENCH RESTORED (BenchRecover): damage = StormWrite rec414/rec423 bad PUTs (deleted ValueToNumber def + kCounters[] table) + repair debris (stray `}` @326, 2-arg AppendCounters). File now 606 lines; +25/-2 vs damaged. R4 intentional deviation: HdPerfTokens->itemsDrawn → HdTokens->itemsDrawn (tokens.h:60; StormWrite's perflog code NEVER compiled). Proof: -fsyntax-only + object compile exit 0, braces 109/109. ninja link blocked by ImagingStubs' in-flight groomSceneIndexPlugin.cpp break (incomplete UsdPrimRange @77) — coordinator to run final link after lib lands.
- Refine record-mode PASSED 2.18s (switch 16.6/16.3ms, SC-4 decision line, round-trip 0.00000). S-9 status = MEASURED — tier rejected. Tangent 2.09s + Material 1.78s green.
- Surgery root cause (ImagingStubs): SetStage zero callers → _AdoptGroom FindGroomStage empty + sessionId hardcoded "" → staging skips SetGraphDesc → empty desc → gen=0/tiles=0. Fix = chain-walk to UsdImagingStageSceneIndex (approved) + _Republish stamps generation.
- TileContract: split-ruling edits landed test-side (xform default, renames, scoping, int-tolerant stamp); residuals = sizes-line/extent/value-identical — MUST re-run post EnginePerf-fix rebuild (hold posture overridden).
- Roster: ImagingStubs (unbreak build → surgery → TileContract), StormWrite (recalled for bench perflog verify after link). Active Meta: ImagingStubs, StormWrite, BenchRecover(released-pending), EnginePerf(idle).
## Wave 6 settles (5)
- SI-1 CLOSED: extent derived in _BuildTilePublication as pure function of pub.points (session.cpp, GfRange3f reduce, no touched-skip) — EnginePerf recall, one block. TileContract 57 ok / NO_FAILS; coordinator-verified ctest Passed. E-8 bitwise still green; E-1 1.687 FAIL@1.5 → wave-7.
- CMakeLists: Surgery test label `T2;gate:S-11` → plain `T2` (zero plan/ references to that test name; registry S-11 = head1M bench record per 09:594 — mislabel removed per PlanNext C1).
- Open M1 items now: Surgery gen=0 (ImagingStubs instrumenting _CommitNow/stage resolution), bench link proof (blocked on imaging mid-edit churn; re-run after settle) + S-5/S-6/S-12 run + StormWrite perflog verify, full T0-T3 sweep, gate registry status column, commit.
## M2 wave dispatched + audits (2026-09-09 evening)
- .omp/m2-packets.md verified (PlanNext2; W1/W2 corrections real). Dispatched: EngineRagged (task, P1 ops + opRegistry), SparseTerminal (nemotron, P2), ImagingPrune (hivemind, P3; SI-9 gate = cost driver per C5), SessSI2 (nemotron, P4). P5 serialized after P6+M1; P6 pending roster headroom.
- LOCATOR COLLISION (surgery's real root cause): attr usdGen:length vs rel length:source breaks DataSourceMapped (dataSourceMapped.cpp:272); keyStage=1 confirmed. rel/ ruling WITHDRAWN; ContractLocator writing .omp/locator-contract.md (evidence-backed, registry-inviolable).
- PubPathReview: (a)(b)(c) FIXED; 5 open (4 regressions): desc-resolution deleted, non-atomic NeedsDesc consume, callbacks reread live gen/report, router-rebuild ordering, groom identity/ABA. Prescription forwarded to ImagingStubs as one edit-set; immutable-payload + atomic-consume + identity-validation shape.
- CMAKE OWNERSHIP TRANSFER (advisory): CMakeIntegrator (hivemind #2) = sole root-CMakeLists owner, serialized batches + targeted build/ctest. Main no longer edits it.
- Roster: active = EngineRagged, SparseTerminal, SessSI2, ImagingPrune, CMakeIntegrator, ImagingStubs, NoiseE1, ContractLocator (8/13). Meta: EngineRagged+ContractLocator+ImagingStubs+SessSI2? (SessSI2=nemotron) → Meta 3, nemotron 2, hivemind 2. Idle: StormWrite, EnginePerf, PlanNext2, GateCensus, PubPathReview, BenchRecover.
