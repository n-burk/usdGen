# Judge report — engineering risk and delivery lens

**Date:** 2026-09-04 · **Judge lens:** can a small team ship this in phases without rewrites? Are
interfaces stable across phases? Are estimates and test gates credible? What is missing that would
block phase 1?

**Inputs read in full:** `design/brief-v1.md` (R1–R9, S1–S46, D1–D8), `ENVIRONMENT.md` including the
corrections block, and the three proposals `proposal-artist.md`, `proposal-performance.md`,
`proposal-risk.md`. Evidence checked directly where a proposal's claim rests on a number:
`G-storm-hair-look-prototype.md` §5 (frame times), `G-storm-throughput-and-prim-granularity.md`
§1.1/§1.5 (the points fastpath), `G-evaluation-scheduling-and-batching.md` §5–§10 (commit points),
`G-freeze-bake-undo-and-frozen-reentry.md` §4.1 (freeze costs).

All three proposals are competent, evidence-cited and internally coherent. None of them re-litigates
a settled decision in a way that matters. The differences that matter for delivery are (a) how much
of the design is *frozen early and deliberately*, (b) whether the phase boundaries are vertical
(demonstrable) or horizontal (layer-by-layer), (c) whether the estimates are arithmetic or wishes,
and (d) whether the gates can actually run on this host.

---

## 1. Scorecard

Scores are 1–10 **through the delivery lens**, not as abstract design quality.

| Area | Artist | Performance | Risk |
|---|:--:|:--:|:--:|
| D1 Schema | 9 | 8 | 8.5 |
| D2 Graph/engine API | 7.5 | 9.5 | 7 |
| D3 Imaging library | 7.5 | 9 | 8.5 |
| D4 Operator catalogue | 9 | 8 | 8.5 |
| D5 Look/maps/expressions | 8 | 8.5 | 9 |
| D6 Tooling | 9 | 7.5 | 8.5 |
| D7 Roadmap | 6 | 8 | 9.5 |
| D8 Libraries/build | 8 | 8.5 | 9 |
| **Overall (delivery)** | **7.6** | **8.4** | **8.8** |

Rationales are in §2–§4. The overall is not the mean: for this lens D7, D8 and D2 carry the most
weight, because a wrong roadmap or an unstable library boundary is what causes a rewrite, and the
engine is the part that is expensive to re-do once operators exist on top of it.

---

## 2. Artist-workflow-first (`proposal-artist.md`)

**Strengths.** The best schema and the best operator catalogue in the set. §3.7/§3.8/§3.9 are the
three `.usda` files a TD could type today; §6.1–§6.4 is the only catalogue that states the *rules
every operator obeys* (mask block on everything, `hash(seed, curveId, salt)` so two clumps with the
same seed decorrelate, `preserveLength`, `Space()` declaration, which types bump `topologyVersion`).
Those rules are worth more than the parameter tables, because they are what stops the catalogue from
drifting into 30 inconsistent operators over three releases. §8.3's brush table (which brush commits
to `points`, which to a `UsdGenSculptLayer`, which to a paint primvar) is the only place any proposal
says where a stroke *lands*, and §8.8's progressive-feedback trio ("the previous generation stays on
screen until the new one is complete") is a real product rule, not a nicety.

**Delivery weaknesses.**

1. **The roadmap is not credible.** Seven phases, 21 engineer-weeks total, one engineer. Phase 1
   alone (four weeks) is: the whole TBB DAG, chunking, per-node buffers, capture/evaluate, the
   deferred-commit/atomic-publish/diffed-dirties machinery, seven operators, the mask block, knot
   ramps, the C ABI, the pxr_boost module, the groom panel *and* the stack editor. Risk's comparable
   slices S1+S3+S5 budget 31 engineer-weeks for less. No staffing is stated, no contingency, and the
   later phases (Phase 6, "v2 catalogue and TD extension", 4 weeks) are sized as if operators were
   free after the first one.
2. **No contract-freeze policy.** Nothing says which names, which headers and which ABIs are frozen
   at which phase. The `UsdGenOp` v-table with `UpstreamChunks`, `ParameterNames`, `OutputPrimvars`
   is presented from §4.4 as the TD extension point, with an out-of-tree operator promised in Phase
   6 "with no change to usdGen's source". Publishing an operator ABI before the engine has run at
   production scale is the classic way to be unable to change `Evaluate`'s signature in Phase 5.
3. **Implicit wiring is an invalidation hazard.** ASSUMPTION 1.8 gives an operator with no authored
   `usdGen:input` its preceding namespace sibling, and the stack editor reorders with
   `reorder nameChildren`. S26 settles wiring as explicit `usdGen:input`. More concretely: S10 is
   measured for *attributes and relationships* through the adapter; nothing in the evidence says a
   `reorder nameChildren` produces any Hydra notice at all. If it does not, a stack reorder silently
   evaluates the old order until something else dirties the groom. The tool always authoring
   `usdGen:input` (which the proposal also says) makes the fallback unnecessary; keep the reserved
   scopes and the reorder gesture, drop the implicit edge.
4. **Two gates as written cannot pass.** Phase 1 asserts "exactly 2 `PrimsDirtied` calls observed
   per frame change" — that is the measured property of the *input* chain (S18/F8); usdGen emits its
   own dirties on top, so the assertion has to be scoped to the input side or it fails on day one.
   Phase 5's "1 M curves render in `usdrecord` with … an hdPrman-class delegate" cannot run on this
   host at all (no RenderMan install; `usdrecord --renderer GL` core-dumps outside the Xvfb).

**Constraint issues.** §3.6 and §8.5 state "never touch `refineLevel` for LOD" and cite S31's
"refineLevel 0 is slower than 1". That is a non-sequitur: the measured table
(`G-storm-hair-look` §5, line 309) is 12.43 / **8.17** / 23.93 ms for levels 0 / 1 / 2 at 200 k
curves. Level 1 is a measured **2.9× lever** over the pinned level 2 and no proposal examines it.
The conclusion may still be right (S29 pins one refineLevel per chunk set; switching costs a cubic
index rebuild at 2.6 ms warm / 5.2 ms cold per 500 k patches plus batch revalidation) — but the
stated justification does not support it.

---

## 3. Performance-and-engine-first (`proposal-performance.md`)

**Strengths.** The strongest engineering content in the set, and three ideas that the other two do
not have and that materially reduce delivery risk:

* **P5 — chunk ≠ tile.** The engine chunk stays at S23's 512 curves; the Hydra prim ("tile") is
  `chunksPerTile` chunks, clamped to S27's 32–256. `dirtyTiles |= 1 << desc.tile` at hop 3. This is
  the only proposal that notices S23 and S27 are two different granularities and reconciles them
  with an integer, instead of picking one and hoping.
* **P3 — the reference lane.** Hair chunks never read hair chunks; guides and clump centres are
  un-chunked reference buffers evaluated in full before their dependents, and a consumer's
  `Capture()` stores plain index/weight arrays. This *deletes* the `UpstreamChunks` cross-chunk
  fan-in that the data-plane report flagged as unmeasured — the one open cost in the engine — rather
  than containing it. Artist keeps `UpstreamChunks` for `Smooth`; Risk keeps it plus a serial
  pre-pass. Performance removes the question.
* **§11.2 — the gate table.** 28 gates across four tiers, each with a threshold anchored to a
  measured baseline (E-1 ≤ 2.5 ms against 1.72–1.91; E-5 ≤ 800 MB against 668; S-5 `drawBatches == 1`).
  This is directly CI-able and is the single most valuable artifact in all three documents.

Also good: the private `tbb::task_arena` at the measured 8-thread knee so usdGen never steals
Storm's `WorkParallelForN` workers during Rprim sync (P8); the `UsdGenDirtyRouter` compile-time
`(primPath, locator-prefix) → {node, bits, surfaceId}` table; the eviction policy scored by
`recomputeCostMs/bytes` with "never evict the tail base"; the interruption rule ("never half-publish");
and the library split justified by *measured rebuild cost* (10.05 s to relink `rigExecImaging`'s
dependents vs 21.4 s for one TU under pybind11 LTO).

**Delivery weaknesses.**

1. **Horizontal phasing.** P0 skeleton → P1 data plane → P2 first pixels means the first hair
   appears in Storm at the end of week 10 of a single-engineer plan, and the first *groomed* hair
   (clump, noise, mask) at week 28 (P5). Risk gets a visible groom in Storm at week 4–6 and a
   deforming, freezable groom at week 9. When something has to be cut, the horizontal plan has
   nothing shippable to cut *to*.
2. **No stability policy.** Nothing states which headers are installed. `usdGenMath` is a static
   library "with a stable header" from P1 — declared stable before an operator has used it.
3. **Internal inconsistencies in the digest.** §4.2 lists `usdGen:enabled` and `usdGen:seed` as
   structural (they enter the Merkle digest); §4.3 then says a seed edit "bumps a capture epoch and
   re-captures … but does **not** recompile the graph". Both cannot be true. Worse, §5.3 step 3 says
   nodes whose digest changed lose their `UsdGenOp`, **their capture cache and their output buffer** —
   so under §4.2's rule, muting an operator (`enabled = false`) throws away the kd-trees of that node
   and everything downstream. That turns a mute/solo A/B — the single most common artist gesture —
   into a 100 ms+ recapture. Artist's rule (§4.6: `enabled` is a value edit and a memcpy pass-through)
   is correct and must be grafted. Separately, the digest formula `d(n) = H(…, sorted(d(children)))`
   hashes *namespace children*, not `usdGen:input` ancestors; for a chain wired by relationship that
   inverts the claim that "a change deep in the chain leaves every prefix digest identical".
4. **P6 (double-buffered publish) is a torn-read hazard, and it contradicts S24.** S24 settles the
   Hydra handoff as `VtArray` copy-on-write with one detach on the next edit. P6 replaces that with
   an A/B ring and — in §6.5 — has the live override "patch CVs in place into the publish ring's back
   buffer". The back buffer is generation N−1, whose `VtArray`s may still be referenced by data
   sources handed out by `GetPrim` and read by an in-flight Storm sync task. S19's measured
   torn-read-free property (8 readers × 20 publishes) was measured for *atomic snapshot publish*, not
   for in-place mutation of a retired buffer. This needs an explicit retire rule (refcount check /
   N-deep ring / `VtArray::IsUnique`) or the CoW default; as written it is a heisenbug in P6/P2.
5. **The frame ledger under-counts (see §4.1 below).** §0.2's "usdGen total ≈ 0.8–1.2 ms, frame ≈ 15 ms"
   omits the `hairTangent` republish, which is not optional under S29 and which is measurably
   expensive under S30's fastpath rule.
6. **ASSUMPTION 1 (uniform CV count per chunk) collides with a v1 requirement.** Frozen and imported
   curves (`UsdGenCurveSource`, v1, R3/R4) are exactly the ragged case; the fast path and every kernel
   signature in §5.11 take a scalar `cvCount`. R-3 acknowledges it and offers `Resample` as the fix —
   but `Resample` is v2 in their own §7.2. Either promote `Resample` to v1 or accept that the R3
   workflow ships on the least-tested path.
7. **Machine tuning in the asset.** `usdGen:chunkSize`, `usdGen:threadLimit`, `usdGen:memoryBudgetMB`
   are authored on the `UsdGenGroom` prim. These are per-host values; R-7's own mitigation is a
   per-machine calibration. They belong in env/config, not in a layer that ships with the character.

Same refineLevel misreading as the other two, and stated more strongly: §0.1's thesis says "reducing
refineLevel does not help", contradicted by the table it quotes two lines later.

---

## 4. Risk-and-delivery-first (`proposal-risk.md`)

**Strengths — this is the delivery document.**

* **§0's five frozen contracts (C1–C5) plus §10.2's API stability boundaries.** C1 property/type
  names and C2 the published chunk contract freeze at S1; C3 the frozen-curve contract at S2; C4 the
  C ABI at S5; C5 the glslfx parameter names at S1. Everything else — `UsdGenOp`, chunk views, the
  graph headers, the imaging internals — is **deliberately not installed and free to churn until S7**.
  That single split is what lets slices 2–8 rewrite the engine without touching an artist's stage,
  and it is the answer to "are interfaces stable across phases?" that the other two do not give.
* **`usdGen` core links no `usd`.** S8 ("no design element may require a `UsdStage` downstream of the
  stage scene index") enforced at link time instead of by code review. Cheapest possible enforcement
  of the constraint most likely to be violated by accident under deadline.
* **Vertical slices with stop conditions.** S0 proves the chain placement (the one thing that
  invalidates the whole design) in two weeks. S1's exit criterion 2 — a `usdGen:magnitude` edit emits
  exactly `primvars/points/primvarValue` + `extent/*` on the affected chunk prims and *nothing else*
  — has an explicit stop condition: if precise invalidation cannot be achieved, fall back to
  `primvars:usdGen:*` (S10's stated prototyping fallback) and **re-plan**, do not proceed. That is a
  real gate: it names the failure, the fallback and the consequence.
* **Test tiers built before features** (§9.1 T0–T4), with T0 (pure `UsdGenGraph` over synthetic
  buffers, no Hydra, no USD) as the fastest gate — enabled by `UsdGenGraphDesc` being a pure-value
  description built by the imaging layer. This is a better engine/imaging boundary than
  Performance's `UsdGenSceneView` façade over the live scene index, because it is testable without a
  chain and it cannot leak a scene-index dependency into the evaluator.
* **The material-binding hedge behind `USDGEN_STORM_MATERIAL_OVERRIDE`** (§7.1), decided by a test in
  S1 rather than discovered in S7 — the correct treatment of S36's one UNVERIFIED item.
* **The most credible estimates.** 3 engineers named as an ASSUMPTION, effort separated from
  calendar (S0–S2 = 9 weeks / 23 eng-weeks; S0–S5 = 20 / 51; S0–S7 = 27 / 67), and an explicit
  statement that a 30 % buffer on S3 and S7 (the two slices with unmeasured dependencies) is the
  honest planning number. Compare: Artist 21 eng-weeks and Performance 40 eng-weeks for near-identical
  scope. A 3× spread across three plans for the same design is itself the finding; Risk's is the one
  a manager could take to a schedule.
* **Small things that only come from having shipped:** `_IsEnabled` env kill-switch on the scene
  index plugin; `UsdGenToolState` as a dataclass built in `__init__` (the exact drift that broke
  usdRig's own test fixture, S43/S46); `bin/_env.sh` globbing `lib/python*/site-packages` instead of
  hard-coding 3.11; `testUsdGenSkelInterop` as a named test for S5's bare-`primvars` promotion;
  `usdGen:<op>:algorithmVersion` so a clump kernel can be fixed in S8 without re-rendering a show.

**Delivery weaknesses.**

1. **The engine is the weakest of the three.** §4.4 keeps `UpstreamChunks` and adds a *serial
   pre-pass* at the start of each `Evaluate` to refresh cross-chunk snapshots. A serial phase inside
   the per-frame path is exactly what the measured TBB scaling (S21, 3.8× at 8 threads) does not
   survive; the proposal knows it (S3 risk gate, "if the serial pre-pass exceeds 20 % of the frame at
   1 M curves…"), but the fallback offered — widen chunks — degrades dirty granularity, which is the
   thing S23 exists to protect. Performance's reference lane removes the problem instead of gating it.
2. **Thread-model inconsistency.** §4.5's table forbids Hydra readers from evaluating, while §5.5 and
   S18(c) require the `GetPrim` backstop to commit. See §5.3 below — this is shared, but Risk states
   the prohibition most explicitly and therefore contradicts itself most visibly.
3. **The Storm material override synthesises a material prim at renderer level** (§7.1) without
   saying how that prim enters the prim set that S27/S28 require to be allocated once and never
   changed. A `PrimsAdded` for a synthesized material during a renderer switch is probably fine, but
   it is unstated.
4. **One gate cannot run where it is placed.** S5 exit criterion 1 — "a comb stroke on a 100 k-CV
   groom holds ≥ 30 fps end to end at 1080p **on the workstation**" — is a T4 manual gate used as a
   slice-exit criterion. Every other S5 criterion is automatable; this one will be waived under
   pressure. Split it: assert the automatable half here (per-move engine + publish time from the
   stats block, `layerChangeCount == 0` between press and release) and move the fps number to the
   workstation protocol.
5. **Estimates are still tight even where they are honest.** S1 at 4 calendar weeks / 11 eng-weeks
   delivers the graph, chunking, capture/evaluate, the digest, the adapter, the publisher with the
   full C2 primvar set, both glslfx files, the material hedge, five operators and seven exit criteria
   including golden images. Realistic, with the stated 30 % buffer applied to S1 as well as S3/S7:
   S0–S2 ≈ 11–12 calendar weeks, S0–S5 ≈ 25, S0–S7 ≈ 34.

Same refineLevel misreading (§8.5).

---

## 4.1 One cross-cutting technical gap that all three share

**`hairTangent` versus Storm's points fastpath.** S29 makes `hairTangent` (vertex vec3, object
space) a mandatory primvar, and S35's measured result is that the primvar path is the only
non-sparkly tangent for 1–2 px strands. `hairTangent` changes whenever `points` changes, so a
deforming frame must republish it. But the fastpath is taken **only when `DirtyPoints` is set and
`DirtyNormals|DirtyWidths|DirtyPrimvar` are all clear** (`hdSt/basisCurves.cpp:932-935`,
`G-storm-throughput` §1.1/§1.5), and `DirtyPrimvar` is one bit for *every* primvar except
points/normals/widths (`hd/changeTracker.cpp:958-979`). Dirtying `hairTangent` therefore drops the
prim into the full loop, re-pulls all primvar descriptors and re-uploads **every non-points primvar**
(`hairT`, `st`, `hairId`, `displayColor`, and the tangent itself).

All three proposals state the invalidation discipline as "emit the bare `primvars/points/primvarValue`
leaf and never co-dirty" (Artist §4.5/§5.3, Performance §5.4 hop 4, Risk §4.4 stage 6) *and* list
`hairTangent` as a mandatory per-frame primvar. Those two statements are incompatible. Performance's
frame ledger (§0.2, "Storm points upload 9.6 MB ≈ 1.2 ms") is the visible consequence: at 100 k × 8 CV
the tangent alone is another 9.6 MB, and the fastpath loss adds the descriptor re-pull and the other
primvars on top.

This is not fatal — the measured deform delta is only 0.13 ms/MB at 200 k — but it is a phase-1
design decision with a schema consequence (is `hairTangent` published at all? recomputed only on
topology change? dropped in favour of the `#ifdef HD_HAS_hairTangent` derivative fallback during a
drag?). It must be a measured gate in the first Storm slice, and no proposal has one.

---

## 5. What would block phase 1

Ranked by how expensive it is to discover late.

1. **Notice emission from the `GetPrim` backstop.** S18(c) requires a lock-free `atomic<bool>`
   backstop that commits on the first `GetPrim` of a generated prim; S17's measurement shows a
   `GetPrim`-driven cook runs on 4 worker threads; F9/S19 say observer callbacks need not be
   threadsafe. All three proposals say "notices go out only from the serialized commit path" and all
   three route the backstop through that same path. Decide before writing the commit model: the
   backstop **publishes without emitting** (the reader sees the fresh generation; notices are
   deferred to the next app-thread commit), or the backstop only sets a flag and returns the previous
   generation. Write it into the spec and gate it (`SI-3`-style: all commits on the main thread).
2. **Initial population.** A renderer-level filtering scene index is inserted with a populated input
   and gets no `PrimsAdded` replay. Nobody says whether usdGen traverses once at construction (what
   does that cost on a 2 205- or 11 005-prim stage?), populates lazily via `GetChildPrimPaths`, or
   relies on the app's first population sweep. Performance and Risk both say the groom prim set is
   maintained "from the notice-maintained path set" — which is empty at construction.
3. **Session identity across chains.** S15 keys the registry by (weak stage, groom root). usdview
   switching renderers, or a process running Storm and an hdPrman preflight, produces two scene index
   instances over one session, each with its own current frame, and (per all three schemas) a
   `renderDensity` that differs per delegate — i.e. a different curve count and therefore a different
   tile/prim set, which S27/S28 require to be fixed. Risk hints at the shape (`_BroadcastToChains`);
   nobody specifies ownership, frame arbitration, or what a renderer switch does to the prim set.
4. **`usdGen:surface` → `GeomSubset` semantics.** All three accept `GeomSubset` targets. None says
   whether scatter density is restricted to the subset's faces, how the face index maps to
   `primvars:skinprim` and to `Far::PtexIndices` face ids (S38's Ptex mapping is per *coarse mesh*
   face), or what a subset edit invalidates.
5. **The `hairTangent`/fastpath decision** (§4.1).
6. **Cross-chunk cost.** Both remaining designs (Risk's serial pre-pass, Performance's reference
   lane) rest on an unmeasured kNN capture cost — Performance's own ~10 ms at 100 k roots on 8
   threads is explicitly an ASSUMPTION extrapolated from nanoflann's benchmarks. One benchmark
   (nanoflann kNN over 100 k and 1 M rest roots, 8 threads) settles the operator schedule and should
   run before D4 is committed to a slice plan.
7. **Ragged (mixed CV count) buffers**, if Performance's uniform-CV assumption is adopted:
   `UsdGenCurveSource` is a v1 operator and imported grooms are ragged by nature.

---

## 6. Best ideas worth grafting

**From Risk (take as the base):** the C1–C5 contract-freeze table and §10.2's installed/not-installed
stability boundary; `usdGen` core linking no `usd` (link-time S8 enforcement); the S0→S1→S2 slice
order with S1's "imprecise dirtying ⇒ fall back to `primvars:usdGen:*` and re-plan" stop condition;
T0 as a Hydra-free engine tier behind a pure-value `UsdGenGraphDesc`; `USDGEN_STORM_MATERIAL_OVERRIDE`
as an env-flagged hedge decided in S1; `_IsEnabled` kill switch; `UsdGenToolState` dataclass;
`bin/_env.sh` python-glob fix; `testUsdGenSkelInterop` as a named S5 test; `usdGen:<op>:algorithmVersion`;
guides reusing the frozen-curve contract C3; staffing-stated estimates with a named contingency.

**From Performance:** P5's chunk-vs-tile split with `chunksPerTile` (reconciles S23 and S27); P3's
reference lane with `ReferenceInputs()` (deletes the unmeasured cross-chunk fan-in — replaces Risk's
serial pre-pass outright); the §11.2 gate table with thresholds anchored to measured baselines and
the §11.1 closed-form cost model; the private `tbb::task_arena` at the measured 8-thread knee with
`USDGEN_THREAD_LIMIT` and a one-shot calibration; `UsdGenDirtyRouter`'s compile-time locator→node
table and `UsdGenDirtyBits`; Morton-sorted roots at capture (tight tile extents for culling, O(1)
chunks per brush footprint) **plus** gate S-4 to prove culling actually rejects; the eviction score
`recomputeCostMs/bytes` with "never evict the tail base"; the "check `_generationRequested` between
nodes, never half-publish" interruption rule; `UsdGenMotionCache` keyed by
`(graphGen, surfaceGen, absTime)` with lerp+clamp for hdPrman's non-retained times; the library split
justified by measured rebuild cost and keeping the Python module out of LTO.

**From Artist:** `usdGen:enabled` as a **non-structural** pass-through memcpy (fixes Performance's
mute-recapture bug and makes A/B instant); `UsdGenNodeStats` per-operator profiling surfaced as a
stack-profiler column *and* as the CI counter source; the `<Description>/__usdGenRender` Hydra-only
scope (usdRig's `__RigExecGenerated` precedent) so no machine-generated prim ever appears in the USD
tree; the reserved `Ops`/`Guides`/`Maps`/`Prototypes`/`Frozen` scopes with reorder-as-`reorder
nameChildren` **while the tool always authors `usdGen:input`** (keep the gesture, drop the implicit
sibling edge); the full `UsdGenMaskAPI` block including Houdini's `rangeMin/rangeMax/effectPosition/
falloff` shortcut and `mask:random`, resolved once per capture into one `VtFloatArray` + a 257-entry
LUT; the seed-salting rule `hash(seed, curveId, saltPerOperator)`; freeze staleness UX (epoch prefix
versioning, stale badge, "Rebase sculpt" by nearest root UV); `UsdGeomComputeExtentFunction` on a
Boundable `UsdGenDescription` so select-and-F frames the hair; the progressive-feedback trio
(interactive LOD ceiling with parked curves, camera-seeded chunk-order publish, previous generation
stays on screen); the brush→commit-target table; the status line carrying the **edit target**.

---

## 7. Estimates and gates

**Estimates.** Only Risk's are usable. Re-baselined with its own stated contingency applied to S1 as
well as S3/S7, and assuming the prototypes in `scratchpad/probes/` are carried in rather than
rewritten (appendix B), a defensible plan for 3 engineers is: **S0–S2 (a usable deforming, freezable
groom) ≈ 11–12 calendar weeks**; **S0–S5 (an artist can groom) ≈ 25**; **S0–S7 (renderable on a show)
≈ 34**, with the v2 catalogue continuous after that. Artist's 21 engineer-weeks for the same scope
plus a v2 catalogue should be discarded, not adjusted.

**Gates.** Performance's table is the right artifact; Risk's per-slice exit criteria are the right
*framing*. Merge them: each slice exits on a named subset of the gate table, plus its own vertical
demo. Two corrections: (a) any gate whose tier is T4 (workstation) may not be a slice-exit criterion
— it is a release criterion; (b) add the three gates none of the three has: a `hairTangent`
publish-strategy gate (fastpath vs full path, measured through the EGL harness), a refineLevel 1-vs-2
LOD gate (8.17 vs 23.93 ms measured at 200 k — establish what a refineLevel switch actually costs in
index rebuild and batch revalidation before writing "pinned 2" into the schema), and a nanoflann kNN
capture benchmark at 100 k / 1 M roots before the operator catalogue is scheduled.

---

## 8. Unresolved questions

1. Who emits Hydra notices when the S18(c) `GetPrim` backstop commits on a Storm worker thread?
2. How is the scene index initially populated when it is appended to an already-populated chain?
3. Session/frame arbitration when two scene index instances share one registry session, and what a
   renderer switch (and a delegate-dependent `renderDensity`) does to the fixed prim set of S27/S28.
4. Is `hairTangent` republished every deforming frame (losing Storm's points fastpath and re-uploading
   every non-points primvar), or held stale / replaced by the derivative fallback during motion?
5. Is refineLevel 1 a legitimate interaction LOD? Measured 8.17 ms vs 23.93 ms at 200 k; what does the
   switch cost in cubic index rebuild and batch revalidation?
6. `usdGen:surface` on a `GeomSubset`: face restriction for scatter, face-index mapping to
   `primvars:skinprim` and to `Far::PtexIndices`, and invalidation on subset edits.
7. Cross-chunk capture cost (kNN over 100 k / 1 M rest roots on 8 threads) — the number both remaining
   engine designs assume.
8. If published arrays are double-buffered or patched in place (Performance P6), what retires a
   generation still referenced by an in-flight Storm sync? S19's torn-read result does not cover it.
9. Do ragged (mixed CV count) buffers ship in v1, or is `Resample`-on-import promoted to v1?
10. Does a `reorder nameChildren` produce any Hydra invalidation through the adapter? If not, no
    evaluated result may depend on namespace order beyond the Kahn tie-break.
11. Which material terminal Storm actually prefers (S36 UNVERIFIED) — resolved by one test in the
    first Storm slice; both hedges are acceptable, but the default must be chosen there.
12. Staffing. Two of the three plans give single-engineer numbers with no calendar; the totals span
    21 / 40 / 67 engineer-weeks for near-identical scope.

---

## 9. Recommendation

**Base: `proposal-risk.md`.** It is the only one of the three that answers the questions this lens
asks. Its C1–C5 contract freeze plus the installed/not-installed split (§10.2) is the mechanism that
makes "no rewrites" a property of the build rather than a hope: the artist-visible surface —
property names, the chunk contract, the frozen-curve contract, the C ABI, the glslfx inputs — is
frozen in the first two slices, and everything else is explicitly allowed to churn until S7. Its
slices are vertical, each with a demo and a stop condition; its estimates state their staffing
assumption and their contingency; and its small operational details (link-time S8 enforcement,
`_IsEnabled`, the tool-state dataclass, the `_env.sh` fix, `testUsdGenSkelInterop`) are the kind that
only appear in a plan written by someone expecting to be on call for it.

**Graft, in priority order.**

1. **Replace Risk §4.4's cross-chunk serial pre-pass with Performance's P3 reference lane**
   (`ReferenceInputs()`, un-chunked reference buffers evaluated to completion first, capture-resolved
   index/weight arrays). This removes the one unmeasured cost in the engine instead of gating it, and
   it removes `UpstreamChunks` from the operator v-table before that v-table has any users.
2. **Adopt Performance's P5 chunk-vs-tile split** (`chunkCurves` for the DAG, `chunksPerTile` → 32–256
   Hydra prims) in place of Risk's single `chunkCurves`/`chunkCountMax` pair, so S23 and S27 are both
   satisfied by construction.
3. **Adopt Performance's §11.2 gate table wholesale**, mapped onto Risk's slices, minus T4 gates as
   slice-exit criteria, plus the three missing gates in §7 above.
4. **Take Artist's D1 vocabulary and D4 catalogue** onto Risk's type hierarchy — the full
   `UsdGenMaskAPI` block, the seed-salting and `preserveLength` rules, the `__usdGenRender` scope, the
   reserved namespace scopes — but **keep Risk's explicit-only `usdGen:input`** and drop the
   implicit-sibling fallback.
5. **Take Artist's `usdGen:enabled` = non-structural pass-through** and Artist's `UsdGenNodeStats`
   stack profiler; wire the profiler's counters to Risk's `UsdGenImaging_GetStatsJson()` so the same
   numbers drive the artist panel and the CI gates.
6. **Take Performance's private `tbb::task_arena`, dirty router and eviction policy** into Risk's
   §4.5 thread/memory section, and resolve the thread-model contradiction there by settling question 1.
7. Add the four phase-1 blockers of §5 (backstop notices, initial population, session/renderer-switch
   arbitration, `GeomSubset` semantics) to S0/S1 as design tasks with owners, and add the
   `hairTangent` publish-strategy gate to the first Storm slice. Re-baseline the schedule to the
   §7 numbers before it is quoted to anyone.
