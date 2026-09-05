# usdGen — Operator catalogue

Date: 2026-09-04. Status: plan v1 (draft, pending review).

This document is the catalogue of usdGen's grooming operators. It states the model every operator
obeys (the base property block, the mask block, seed salting, the blend envelope, `usdGen:enabled`,
`usdGen:algorithmVersion`, and the three declarations `Space` / `ReadPhase` / `TopologyEffect`), the
rules for composing them into a chain in any order, then the exact parameter set, capture step,
evaluate kernel and emitted primvars of every v1 operator, the v2 tier at table depth, the v3 tier at
paragraph depth, the mask block's executable arithmetic, three worked chains with the dirty class of
each typical edit, the kernel-versioning policy that keeps an old groom looking old, and the tests
and gates that prove all of it. It is written so an engineer can implement an operator from it
without the proposals or the research reports at hand. `02-schema.md` is the normative property
registry (ADR §9.2 R7): where a name, type, default or token here and there disagree, 02 wins, and
§0.13 lists every correction this document raises against a sibling.

Reads with: `02-schema.md` (the schema definitions and `.usda` layout these names live in),
`03-execution-engine.md` (`UsdGenOp`, the capture/evaluate split, chunks, the dirty router,
`UsdGenGraphDesc`), `05-static-curves-and-deformation.md` (`UsdGenCurveSource`, `UsdGenFreeze`,
`UsdGenSculptLayer`, the `UsdGenDeform` transports and the C3 curve contract), `06-imaging.md` §4.1
and §4.3 (the tile contract C2 and the instancer contract, which fix the published names of the
primvars this document's operators emit), `07-look-maps-expressions.md` (the `UsdGenMap` prim types the mask block consumes, the SeExpr set,
and what shading does with the primvars operators emit), `08-tools.md` (the brushes that author these
parameters, the stack profiler, the mask visualisation mode and the C ABI),
`09-performance-and-benchmarks.md` §5 (the gate registry), `10-build-dependencies-testing.md` §3.5
and §3.7 (the env-var registry and the kernel build flags), `11-roadmap.md` §2 and §6.1 (which
milestone each tier lands in), `appendix-A-evidence-ledger.md` (the `EV-nnn` rows every number here
cites).

---

## 0. Operator model recap

### 0.1 Three things are called "operator"

| Thing | Where it lives | Owned by |
|---|---|---|
| The **prim** — a codeless typed prim under `<Description>/Ops/…` carrying `usdGen:*` properties | the stage | `02-schema.md` |
| The **node** — one `UsdGenOp` subclass instance in the compiled `UsdGenGraph`, with a capture cache and an output buffer | the engine | `03-execution-engine.md` |
| The **kernel** — a free function in `usdGenMath` over `RESTRICT` planar float pointers | `usdGenMath` | this document (pseudocode) and `10-build-dependencies-testing.md` (build flags) |

This document is normative for the *semantics* of all three; where a property name here and in
`02-schema.md` disagree, `02-schema.md` wins for the schema listing. Every operator type derives from
the abstract `UsdGenOperator` (ADR §2.1), so the prim adapter registered on `UsdGenOperator` with
`includeDerivedPrimTypes: true` publishes a new operator's properties with no adapter change (S10).

### 0.2 The base property block (on `UsdGenOperator`, every operator inherits it)

| Property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:input` | `rel` (ordered) | — | upstream operator prim(s). **Explicit only** — the implicit preceding-sibling fallback is rejected (ADR §2.1, S26). Empty = a source node. |
| `usdGen:enabled` | `bool` | `true` | `false` = the operator contributes nothing. Never named `usdGen:active` (collides with USD prim `active`, which S41 uses for freeze undo). §0.4 |
| `usdGen:blend` | `float` | `1.0` | envelope between the input and this operator's result, exact endpoints. §0.5 |
| `usdGen:seed` | `uniform int` | `0` | the authored seed folded into every hash the operator draws. §0.6 |
| `usdGen:space` | `uniform token` | `"auto"` | `auto` \| `rest` \| `deformed`; `auto` = the operator type's declared class (`02-schema.md` §2.5). Three tokens, no more (ADR §9.2 R9). §0.7 |
| `usdGen:readPhase` | `uniform token` | `"final"` | `base` \| `preceding` \| `final` \| `@<abs prim path>` — which generation of the bound surface is sampled (S26; `preceding` is an unconditional v1 alias for `final`, ADR §9.2 R9). §0.7 |
| `usdGen:surface` | `rel` | inherits the Description | per-operator surface override; accepts `Mesh` and `GeomSubset` targets (ADR §2.3) |
| `usdGen:label` | `string` | `""` | free text shown in the stack editor; affects nothing |
| `usdGen:algorithmVersion` | `uniform int` | `0` | `0` = track the newest kernel; any positive value pins that kernel revision. The tool, every freeze and every bake author the explicit current version, so a tool-authored asset is always look-preserved; a hand-written asset that omits the attribute follows the newest kernel (ADR §9.2 R17). §7 |

`uniform token usdGen:mode` is **not** a base property. It is declared per type by the four v1 types
that have a concept switch — `UsdGenScatter`, `UsdGenDeform`, `UsdGenSmooth`, `UsdGenDirection` —
and `02-schema.md` §2.5's `UsdGenOperator` table has no `usdGen:mode` row. §1.2 note ¹ lists the six
v1 types whose switch is spelled differently and the five that have none.

S24's per-node output buffer is never opted out per prim: there is no `usdGen:cacheOutput` property
(dropped by ADR §9.2 R8). Memory is bounded by `USDGEN_MEMORY_BUDGET_MB` and the eviction score
`recomputeCostMs/bytes` (ADR §4.1; `03-execution-engine.md` §6.4), which never evicts the tail base
or a live-override target. Machine tuning is env/config and is never authored into an asset
(ADR §2.3).

Plus the auto-applied `UsdGenMaskAPI` block (`usdGen:mask:*`, §5), applied through plugInfo
`AutoApplyAPISchemas` on `UsdGenOperator`, with the tool also applying it explicitly until gate SI-8
proves auto-apply works on a codeless type (ADR §2.1). An unapplied API schema is absent from
`UsdPrimDefinition::GetPropertyNames()` — the list the adapter's mappings are built from — so the
mask would be silently dropped (`design/judge-evidence.md` §2.1).

Grouped parameters are authored one level deeper (`usdGen:clump:noise:frequency`), which the adapter
turns into nested Hydra locators (`usdGen/clump/noise/frequency`) — exactly the granularity the dirty
router wants (MEASURED, ledger rows `EV-081`/`EV-082`;
`research/G-stage-free-parameter-and-time-transport.md` §2). Ungrouped parameters stay flat. A flat
name resolves per prim type, so two types may give one name different types; usdGen uses that
freedom exactly once — `usdGen:direction` is a `uniform token` on `UsdGenGrow` and a `vector3f` on
`UsdGenDirection` (`02-schema.md` §2.6, §2.7.1). Both spellings stand exactly as 02 declares them:
§2.6 declares `usdGen:direction` (`uniform token`, `"surfaceNormal"`, structural) on `UsdGenGrow`,
§2.7.1 declares `usdGen:direction` (`vector3f`, `(0,1,0)`, value) on `UsdGenDirection`, and §6.1
routes `usdGen/direction` per prim type ("`usdGen/direction`, `usdGen/segments` · `UsdGenGrow`").
**No rename is proposed** — there is no `usdGen:growDirection` in this design: 02 is the normative
registry (ADR §9.2 R7), per-type resolution already disambiguates the two, and a rename before C1
would buy nothing. Names the ADR fixes verbatim (`usdGen:density`, `usdGen:clump:centers`,
`usdGen:clump:density`, `usdGen:mask:region`) are used exactly as written there.

### 0.3 Modes

One prim type per operator **concept**, with a `uniform token usdGen:mode` where the concept has
variants (ADR §2.1). `UsdGenScatter` has four modes, not four prim types: a mode switch is a value
edit, not a prim delete/create resync that would walk into the `RemovePrim` + OpenExec bug at
`esfUsd/stageData.cpp:361` (S41, S46). Two consequences that must hold in code:

1. **`Bind()` pulls every mapped locator of the node regardless of mode.** A parameter that is only
   read in one mode (`usdGen:clump:noise:frequency` while `clump:noise:amount == 0`) must still be
   pulled once per topology generation, or an edit to it may not invalidate (S14;
   `design/judge-evidence.md` §2.1). This is a debug-build assertion under `USDGEN_OP_CHECKS=1`,
   whose registry row is `10-build-dependencies-testing.md` §3.5 — `USDGEN_OP_CHECKS`, default
   `0`, "debug-build operator contract wrapper (never compiled into Release)" — which ADR §9.4 R35
   makes the only place an env var may be declared.
2. **A mode token is a digest term, never a capture parameter.** `mode` is one of the terms of
   `d(n)` (ADR §4.2.1), so a mode edit **recompiles** that node's sub-graph and, because the capture
   set changes with the kernel branch, recaptures its cone (`02-schema.md` §6.1: "`usdGen/mode` ·
   `UsdGenScatter`, `UsdGenDeform`, `UsdGenSmooth`, `UsdGenDirection` · recompile (kernel branch and
   capture set change)"). The same holds for the differently-spelled concept switches of §1.2 note ¹
   — `usdGen:clump:method`, `usdGen:blendMethod`, `usdGen:distribution`, `usdGen:length:mode`,
   `usdGen:idSource`, `usdGen:frozen:mode` — each of which `02-schema.md` §6.1–§6.2 routes
   explicitly (`length:mode` is the one routed as topology rather than as a recompile, because
   entering or leaving `cull` changes the curve count).

### 0.4 `usdGen:enabled`

| Operator class | `enabled = false` costs | Why |
|---|---|---|
| Topology-preserving styler or deformer | a pass-through: **no recompile, no recapture**, the node keeps its kd-trees, clump ids, map samples and mask arrays (the engine implements it as an input-buffer alias, `03-execution-engine.md` §3.5) | ADR §2.3, §9.2 R14; `design/proposal-artist.md` §4.6. Toggling for A/B is the single most common artist gesture, and a recapture there is an estimated 10–150 ms (ASSUMPTION, `design/judge-artist.md` §2.1; gate **E-4** measures the capture term) |
| Generator, `UsdGenResample`, `UsdGenFreeze`, `UsdGenInstance` | topology-invalidating: the node's topology contribution vanishes, the downstream cone recaptures, one topology publish | `02-schema.md` §2.5 and §6.2 name exactly this set; for `UsdGenFreeze` the chain above re-enters and the counts may change, for `UsdGenInstance` the instancer's instance count changes |
| `UsdGenLength` with `length:mode = "cull"` or `cullThreshold > 0` | topology-invalidating, as a generator: the surviving id set is a capture product | `02-schema.md` §1 names Length's cull mode a declared exception to the styler rule; `design/proposal-artist.md` §6.4 rule 5 lists it among the topology-bumping operators |

Reconciliation with ADR §4.2.1, which excludes `enabled` from the Merkle structural digest: `enabled`
is never a *digest term* (ADR §9.2 R14). For the topology-preserving case the router raises
`UsdGenDirtyParameter` on the node — evaluate-only, this node's chunks. For generators,
`UsdGenResample`, `UsdGenFreeze`, `UsdGenInstance` and a culling `UsdGenLength` it raises
`UsdGenDirtyTopology`, which bumps the node's topology version and therefore every downstream capture
epoch (`03-execution-engine.md` §5.1). Neither path recompiles the graph, and a muted generator keeps
its capture, so ids survive mute/unmute (R14). (Recorded in the decisions list of
`12-risks-decisions-open-questions.md` §0.)

### 0.5 The blend envelope

`float usdGen:blend = 1` is Houdini's per-operator *Blend*. The contract is an **identity by
early-out, not by arithmetic**:

```
if (blend == 0.0f)             -> the node is a pass-through; output buffer is bitwise the input
if (blend == 1.0f)             -> no envelope arithmetic runs at all
otherwise, per curve c, per CV i:
   w(c,i) = blend * curveMask[c] * rampLUT[t_i]           // §5.2; rampLUT[t] is the lerped read
   P_out  = lerp(P_in, P_full, w(c,i))                    // continuous outputs only
```

Rules: the envelope applies to **continuous** outputs (`points`, `widths`, per-curve floats);
discrete outputs (`clumpId_<n>`, `guideIndex`, `guideWeight`) are written when `blend > 0` and passed
through at `blend == 0`, never interpolated. An operator whose maths is not a displacement applies
`w` to its *parameter* rather than its result (`UsdGenLength` scales by `lerp(1, s, w)`,
`UsdGenWidth` by `lerp(w_in, w_target, w)`); §2 says which form each uses. `preserveLength` runs
**after** the envelope, inside `Evaluate`, so a blended result still has the rest arc length.
Generators ignore `usdGen:blend` — you cannot lerp topology — and `blend != 1` on a generator is a
compile warning naming the prim.

### 0.6 Seeds and salting

Every random draw in usdGen is a pure integer hash — no RNG object, no hidden state, no dependence on
iteration order (`design/proposal-performance.md` §5.11 rule 5; usdRig's determinism contract,
`research/A1-usdrig-graph.md` §5). The hash is **pinned by ADR §9.2 R12**; changing it or any salt is
a look change that bumps `usdGen:schemaVersion` (§7), so it is stated here in full rather than left
to the implementation:

```cpp
// usdGenMath/hash.h — pinned by ADR §9.2 R12; identical to `02-schema.md` §2.19.1.
inline uint64_t UsdGenHash64(uint64_t key, uint32_t salt) {      // SplitMix64 finalizer
    uint64_t z = (key ^ (uint64_t(salt) * 0x9E3779B97F4A7C15ull)) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
inline uint32_t UsdGenHash32(uint64_t key, uint32_t salt) {      // the high 32 bits
    return uint32_t(UsdGenHash64(key, salt) >> 32);
}
inline float    UsdGenHash01(uint64_t key, uint32_t salt) {      // [0,1)
    return float(UsdGenHash32(key, salt) >> 8) * 0x1.0p-24f;
}
// The multi-key forms fold left with the same salt — the executable spelling of 02 §2.19.1's
// shorthand `UsdGenHash64(a, b, c, salt)`.
inline uint64_t UsdGenHash64(uint64_t a, uint64_t b, uint32_t salt) {
    return UsdGenHash64(UsdGenHash64(a, salt) ^ b, salt);
}
inline uint64_t UsdGenHash64(uint64_t a, uint64_t b, uint64_t c, uint32_t salt) {
    return UsdGenHash64(UsdGenHash64(a, b, salt) ^ c, salt);
}
/// A scatter root's stable id: R12's `UsdGenHash64(seed, faceIndex, k, kSaltScatter)`.
inline uint64_t UsdGenCurveId(int seed, uint32_t faceIndex, uint32_t k) {
    return UsdGenHash64(uint64_t(uint32_t(seed)), uint64_t(faceIndex), uint64_t(k), kSaltScatter);
}
/// The call-site form for a per-curve draw inside an operator.
inline float    UsdGenDraw01(int seed, uint64_t curveId, uint32_t salt) {
    return UsdGenHash01(UsdGenHash64(uint64_t(uint32_t(seed)), salt) ^ curveId, salt);
}
// hash3(seed, curveId, salt) = three UsdGenDraw01 draws with salts salt, salt+1, salt+2.
```

`UsdGenCurveId` is the function `02-schema.md` §2.6 and `03-execution-engine.md` §4.1 name in
shorthand as `hash64(seed, faceIndex, k)`, and `research/A7-prior-art-grooming.md` §9.1 G1 as
`id = hash64(seed,f,k)`. **`curveId` is 64-bit**: contract C3 carries
`uint64[] primvars:usdGen:curveId` (ADR §9.2 R12, which supersedes S42's `uniform int`), as
`02-schema.md` §5 and `05-static-curves-and-deformation.md` §2.4 both now declare. Every consumer in
this document — sculpt-layer ids, clump ids, the decimation predicate — keys on that 64-bit value.

`salt` is a **per-use compile-time constant** (R12) named `kSalt<Use>`; the numeric values live in
`usdGenMath/hash.h` and `02-schema.md` §2.19.1 delegates the per-operator table to this section.
Distinct salts are what keep two different operators drawing different numbers from the same
`curveId`, and what keep the decimation draw independent of `hairId`.

| Use | Salt | Use | Salt |
|---|---|---|---|
| `UsdGenScatter` root ids | `kSaltScatter` | `UsdGenClump` (level L) | `kSaltClump + L` |
| `UsdGenGrow` | `kSaltGrow` | `UsdGenLength` | `kSaltLength` |
| `UsdGenNoise` | `kSaltNoise` | `UsdGenWidth` | `kSaltWidth` |
| `UsdGenGuideInterpolate` | `kSaltGuideInterp` | `UsdGenDirection` | `kSaltDirection` |
| `UsdGenSmooth` | `kSaltSmooth` | `UsdGenResample` | `kSaltResample` |
| `UsdGenInstance` | `kSaltInstance` | mask block `random` (keyed by `usdGen:mask:randomSeed`) | `kSaltMaskRandom` |
| density decimation | `kSaltDensity` (≠ 0) | `hairId` | `0` |
| `UsdGenLookAPI` jitter bake (keyed by `usdGen:look:jitterSeed`, `07-look-maps-expressions.md` §1.2) | `kSaltLookJitter` | | |

**What the salt does and does not buy.** A per-use salt decorrelates *different* uses. It does **not**
decorrelate two prims of the *same* type: two `UsdGenClump` prims at the same level, with the same
authored `usdGen:seed` and the same input id set, draw identical numbers, because nothing in
`UsdGenDraw01(seed, curveId, kSaltClump + L)` distinguishes them. That is deliberate and matches
XGen's `Generator Seed` and Houdini's per-node seeds: **the artist varies `usdGen:seed`**, and the
tool authors a distinct random seed on every operator prim it creates, so the default case is
already decorrelated. A per-node term derived from the prim path was rejected — it would make a
rename a silent look change, defeat `usdGen:algorithmVersion` (§7) and break freeze reproducibility
across a re-path. `02-schema.md` §2.5's `usdGen:seed` doc line ("so two `UsdGenClump`s with the same
seed are uncorrelated") overstates this and is a required correction (§0.13); §8 test 6 asserts the
guarantee that actually holds.

Two derived quantities hash the curve id with **fixed** salts, so that independent consumers agree —
and they use *different* salts, which is load-bearing:

* `hairId` (uniform **float** on the published tile) `= UsdGenHash32(curveId, 0) * 2^-32` ∈ [0,1) —
  the shipped glslfx declares it `float` (ADR §1 S29, §9.2 R12). `05-static-curves-and-deformation.md`
  §2.4 and `02-schema.md` §2.19.1 state the same formula.
* Density decimation: `keep iff UsdGenHash32(curveId, kSaltDensity) < keepFraction * 2^32`, with
  `kSaltDensity ≠ 0` (R12). It is still "decimate by stable id" (ADR §2.3), so ids, sculpt deltas,
  locked-curve ids and clump ids survive every change of scale, and the smaller set is always a
  subset of the larger. The distinct salt is what stops the surviving set being
  `{hairId < keepFraction}`: at `kSaltDensity = 0` and `keepFraction = 0.25` every surviving hair
  would have `hairId ∈ [0, 0.25)` and the glslfx's per-curve hue and value jitter (S35) would be
  biased to a quarter of its range in the viewport and not at render. T0 `testUsdGenHash` asserts the
  surviving set's `hairId` stays uniform within 2 % at 0.25 (ASSUMPTION — a correctness argument).

**Composition of the two scales (normative, ADR §9.2 R13).**

```
scale        = (context == render) ? usdGen:renderDensityScale : usdGen:densityScale
keepFraction = clamp(scale_groom * scale_description, 0, 1)
```

The two contexts are **exclusive**: neither scale applies in the other, and both default to 1
(R13). `usdGen:density` — hairs per square stage unit on the rest surface — defines the id space and
is authored at the **full render count**; raising it later moves the id space. A product above 1 is
clamped with one `TF_WARN` per description. Context is explicit — `USDGEN_CONTEXT=render`,
`UsdGenImaging_SetContext()` or the usdview toggle — never inferred from the renderer's display name
(ADR §2.3). `02-schema.md` §2.3.1 states the same predicate and the same exclusivity; the two must
stay identical.

### 0.7 `Space`, read phase, topology effect

```cpp
enum class UsdGenSpace  : uint8_t { Inherit, Rest, Deformed };  // usdGen:space; Inherit <=> "auto"
enum class UsdGenTopoFx : uint8_t { None, CurveCount, CvCount, Both };  // declared, not authored
```

Both enums are `03-execution-engine.md` §1.2's, verbatim (`03-execution-engine.md:202-203`); code
written from this document compiles against that header. ADR §9.2 R9 names the first enumerator
**`Inherit`**, never `Auto`. The authored default token is `"auto"` (`02-schema.md` §2.5), which resolves to
`UsdGenSpace::Inherit` = the operator type's own declared class, i.e. S25's
`restSpace | deformedSpace` classification. **There is no `world` token**: after flattening, deformed
space *is* world space (S4, ADR §9.2 R9). A v3 `UsdGenWind` that needs a world class adds a token
under `02-schema.md` §8.2 with fallback `auto`.

* **`Space()`** partitions the topologically sorted node list into a **rest head** and a **deformed
  tail** (ADR §4.1 I4; S25). The tail is the earliest node whose resolved space is
  `UsdGenSpace::Deformed` **and every node downstream of it** — the downstream-closure form, so that
  a rest-space styler placed *after* a deform is still counted in the tail
  (`design/proposal-performance.md` §5.8's "at the last `restSpace` node" agrees whenever spaces are
  monotone). Only the tail re-runs per frame and per motion sample: the ledger budgets it at
  0.4–0.6 ms
  (**DERIVED from ledger rows `EV-014`–`EV-016` and `EV-001`**, `09-performance-and-benchmarks.md` §0.2,
  from a 0.158–0.545 ms single-thread styler pass; gate **E-1** measures it directly). The five-node
  chain at 100 k × 8 CV is **MEASURED** at 1.02 ms on 8 threads (ledger rows `EV-001` and `EV-008`;
  the thread sweep is `research/G-data-plane-engine-prototype-benchmark.md` §3.3) and at 1.72–1.91 ms
  on 20 (`prototypes/data-plane-benchmark/results_main.txt`, which records only `threads=20` runs).
  The stack editor shows a small motion icon on tail rows so an artist can see what a render
  will cost (`design/proposal-artist.md` §6.4 rule 4).
* **Read phase** selects *which generation of the surface prim* is sampled: `base` = the rest
  surface from the `UsdGenRestAPI` data source at `UsdTimeCode::Default()` (`usdGen/rest/points`,
  S12); `final` (the authored default) = the surface as usdGen sees it, post-flattening, world space
  with `resetXformStack = true` (S4); `@<abs prim path>` retargets to another mesh. **`preceding` is
  an unconditional v1 alias for `final`** (ADR §9.2 R9): the compiler rewrites the token to
  `UsdGenReadPhase::Final` and emits one `TF_WARN` naming the prim on first use, for every operator,
  in every space class. It is retained only so usdRig-shaped assets load. `02-schema.md` §2.5's
  "the surface before usdGen's own overlays on that prim" reading is a **v2** semantic and is not
  implemented in v1 (`03-execution-engine.md` §1.6). The C++ type-level fallback
  `UsdGenOp::ReadPhase()` returns `UsdGenReadPhase::Final` (`03-execution-engine.md` §8.1), which is
  the same value as the authored token default `"final"`, so the two can never disagree
  (`03-execution-engine.md` §1.6); the fallback is used only when a `UsdGenNodeDesc` carries no
  `readPhase` token at all. Which *time* is sampled is decided by
  `usdGen:space`, not by the read phase. `space = "rest"` with `readPhase = "final"` is a compile
  **warning**, not an error — a noise field that follows the animated surface is sometimes wanted.
* **`TopologyEffect()`** is declared by the C++ class, never authored (`03-execution-engine.md`
  §8.1: `virtual UsdGenTopoFx TopologyEffect() const`). `CurveCount` changes the number of curves,
  `CvCount` the CVs per curve, `Both` either. A node declaring anything but `None` forces the chunk
  partition to be recomputed and one topology publish; everything else never does
  (`design/proposal-artist.md` §6.4 rule 5). The declaration is **static**, the effect may be
  data-dependent: `UsdGenLength` declares `CurveCount` unconditionally, and when
  `length:mode != "cull"` and `cullThreshold == 0` its capture emits the identity id set, so the
  topology version does not change and no republish is triggered.

The engine chunk is 512 curves; the Hydra prim is a **tile** of
`chunksPerTile = max(1, ceil(nChunks / usdGen:tileTarget))` chunks, and
`nTiles = min(nChunks, clamp(ceil(nChunks / chunksPerTile), 32, 256))` (ADR §9.3 R21, which fixes the
ADR §1 wording this document previously quoted). Worked: 100 k curves at chunk 512 and
`tileTarget = 64` gives 196 chunks, 4 chunks per tile, **49 tiles**; the `min(nChunks, …)` term is
what keeps a 12 k-root groom (24 chunks) from being asked for 32 tiles. Operators never see tiles.

### 0.8 Capture and evaluate

Every operator is split (S25; ADR §4.1 I4):

| Phase | Runs | May | Must not |
|---|---|---|---|
| `Capture()` | once per **capture epoch** (a 128-bit digest over surface topology, `usdGen:seed`, guide ids, upstream topology version, and the operator's own `TopologyParameters()`) | allocate; use `WorkParallelForN`; build kd-trees; evaluate SeExpr, images and Ptex; take the graph mutex | — |
| `Evaluate()` | per frame, per chunk, inside `tbb::parallel_for` | read `in`, `cap`, `ctx` and the reference buffers | allocate; touch a `UsdStage`; write outside its own chunk's CV range; **call an expression or sample a texture** |

The last prohibition is load-bearing. All map/Ptex/SeExpr evaluation is CPU at capture, baked to
per-curve or per-CV arrays (S37): SeExpr costs 13–117 ns/eval and Ptex bilinear 23 ns (MEASURED,
ledger rows `EV-067` and `EV-070`, `research/A8-seexpr-ptex-libs.md` §1.6, §2.8) — fine once per
edit over 1 M roots, catastrophic per frame at 1.6 M CVs; both totals are **DERIVED from `EV-067`/
`EV-069`** (`design/judge-evidence.md` §2.2 item 5) and UNMEASURED end to end until gates
L-3/L-4/L-5 (M4).
`UsdGenExprOp` (v2) is therefore **capture-time only** (§3).

Cross-curve reads never cross a hair chunk. Guides, clump centres and card roots are **reference
lane** nodes: un-chunked, evaluated to completion before any consumer chunk, read-only during the
parallel region (ADR §4.1 I3). A consumer resolves at capture *which* reference curves each hair uses
and stores plain index/weight arrays (`guideIdx[3]`, `guideW[3]`, `clumpId[level]`). There is no
chunk fan-in set and no `UpstreamChunks` in the v1 v-table.

### 0.9 `usdGen:algorithmVersion`

`uniform int usdGen:algorithmVersion`, one per operator *type*, **schema fallback `0` = "track the
newest kernel"** (ADR §9.2 R17). Any positive value pins that kernel revision. The tool, every freeze
and every bake author the explicit current version, so look preservation is guaranteed for every
tool-authored asset; a hand-written asset that omits the attribute follows the newest kernel, and
that is documented behaviour rather than an accident. §7 states the policy in full. This is the
mechanism that lets a clump kernel be fixed in M8 without re-rendering a show (`design/judge-delivery.md`,
"small things that only come from having shipped").

### 0.10 Chain composition

Wiring is the explicit `rel usdGen:input`. Execution order is **Kahn topological order over
`usdGen:input`, with namespace order only as the tie-break**; cycles are compile errors reported on
the offending prim (S26; ADR §2.1). The stack editor's drag gesture rewrites `usdGen:input` — one
attribute edit per moved node — and never `reorder nameChildren`. The Description names its terminal
with `rel usdGen:terminal` (exactly one target, ADR §2.3).

**Any order is legal, including a generator after a styler.** Three composition rules make that work:

1. A **generator with no `usdGen:input`** is a source node: it builds topology from the surface
   (`UsdGenScatter`) or from a stage curve set (`UsdGenCurveSource`).
2. A **generator with an `usdGen:input`** consumes the upstream buffer as its *root set*
   (`UsdGenGuideInterpolate` takes roots from a `UsdGenScatter`) or replaces topology outright
   (`UsdGenScatter` with `mode = "atGuides"` uses the upstream curves' roots).
3. **`rel usdGen:guides` and `rel usdGen:clump:centers` may target either a `UsdGenGuideSet` prim or
   another `UsdGenOperator` prim.** Targeting an operator marks that node
   `UsdGenRole::Reference`: it is evaluated whole and un-chunked before its consumer (ADR §4.1 I3).
   This is what lets a styled curve set become the guides of the next generator, and it is the
   mechanism the reference lane already provides — no new machinery.

Rule 3 makes the chain a DAG, not a list — which is why namespace order is only a tie-break. It is a
DAG *inside one Description*: a `usdGen:input`, `usdGen:guides` or `usdGen:clump:centers` crossing a
Description boundary is a **compile error** in v1 (ASSUMPTION — the ADR fixes the reserved layout
(§2.2) and is silent on sharing; `12-risks-decisions-open-questions.md` §3 Q-13 leaves it open with
"not shared" in force). A shared node would have no unambiguous surface, density scale, tile target,
motion profile or `UsdGenNodeStats` row. Sharing a setup between brows and lashes is done by
*referencing* an `Ops` scope; a `<Groom>/Presets/` scope is the candidate answer, revisited at M5.

### 0.11 The request's example chain, stage by stage

`curve generator → clump styler → curve generator → clump styler → frizz styler` (R4). In usdGen:

| # | Prim | Sees | Emits |
|---|---|---|---|
| 1 | `UsdGenScatter "roots_a"` (`mode = random`, `density = 400`) | rest surface faces + a density map | 12 k roots: `curveId`, `rootPrim`, `rootUV`, `rootFrame`; CV count 0 |
| 2 | `UsdGenGuideInterpolate "gen_a"` (`usdGen:input = roots_a`, `usdGen:guides = </…/Guides/scalp>`) | roots from 1 + the authored guide set (reference lane) | 12 k curves × 8 CV; `guideIndex`, `guideWeight` (`elementSize = 3`) |
| 3 | `UsdGenClump "clump_big"` (`clump:size = 1.6`, `clump:levels = 1`) | curves from 2 + clump centres (a nested `UsdGenScatter` at `clump:density`) | same topology, clumped; `clumpId_0` |
| 4 | `UsdGenScatter "roots_b"` (`density = 6000`) — a **source** node, it does not read 3 | rest surface faces | 180 k roots |
| 5 | `UsdGenGuideInterpolate "gen_b"` (`usdGen:input = roots_b`, `usdGen:guides = </…/Ops/clump_big>`) | roots from 4 + **the output of 3 as its guide set** | 180 k curves × 8 CV; new `guideIndex/guideWeight` indexing into 3; `clumpId_0` inherited from the highest-weight guide |
| 6 | `UsdGenClump "clump_fine"` (`clump:size = 0.4`) | curves from 5 + its own centres | `clumpId_1` |
| 7 | `UsdGenNoise "frizz"` (`noise:magnitude = 0.09`) | curves from 6 | same topology, frizzed |

This is XGen's "guides at 10 %, then interpolate again" and Houdini's hair-from-hair pattern
(`research/A7-prior-art-grooming.md` §1.2, §3.1) expressed with nothing but `usdGen:input` and a
`usdGen:guides` retarget. Node 2's captured guide weights are unaffected by anything downstream;
node 5's are recaptured whenever node 3's topology or capture epoch changes, not when its
*parameters* change (§6.4). Node 5 emits `guideIndex`/`guideWeight` in the `elementSize = 3` encoding
of §2.3, never a `int3`/`float3`.

**Uniform per-curve primvar inheritance through a generator** (an addition; no proposal states it):
a generated curve inherits every `uniform` primvar of its **highest-weight** reference curve, ties
broken by lowest reference index; `vertex` primvars are interpolated with the weights used for
`points`. That is what makes `clumpId_0` survive step 5.

**`clumpId_<n>` numbering.** ADR §2.3 fixes the name `clumpId_<level>` (uniform int). Levels are
numbered **chain-wide** in topological order over **every** `UsdGenClump` node in the compiled graph
and its `clump:levels`, **regardless of `usdGen:enabled`**, unless the node authors
`uniform int usdGen:clump:level ≥ 0`, which pins its index (ADR §9.2 R10). The assignment is made at
compile and reported in the stack editor ("clump_big → clumpId_0", "clump_fine → clumpId_1"). A
disabled Clump keeps its indices reserved and republishes its `clumpId_<n>` primvar unchanged through
the pass-through, so muting a clump never renames a primvar, never rebinds a shader and never
recompiles — which is what §0.4, ADR §2.3 and ADR §4.2.1 require of a non-structural `enabled`. Only
adding, removing or rewiring a Clump, changing its `clump:levels`, or authoring `clump:level`,
renumbers.

### 0.12 Conventions in this document

Numbers are tagged **MEASURED**, **DERIVED from &lt;row&gt;**, **UNMEASURED** (with the gate that will
measure it) or **ASSUMPTION** — ADR §9.5 R42's four tags; there is no "ARITHMETIC" tag. Every
MEASURED and DERIVED figure names its `appendix-A-evidence-ledger.md` row id — `EV-001…`, which
ADR §9.1 R1 makes the only citation handle for a measurement. Every engine budget quotes the
**8-thread private-arena** number, with the 20-thread number in parentheses (ADR §9.3 R27).

Parameter tables give name, type, default, range and the schema `doc` field. Ramps follow ADR §9.2
R11: a scalar ramp is `float2[] <p>:knots` + `uniform token <p>:interpolation`, a colour ramp is
`float[] <p>:positions` + `color3f[] <p>:colors` + interpolation; a `.spline` on a *ramp* property is
a compile error, on a plain scalar parameter it means "animated"; the whole-`TsSpline` transport is
the dedicated `float <p>:spline`. Every ramp is baked to a **257-entry float LUT** at capture so the
inner loop is a lerp (`design/proposal-performance.md` §5.11 rule 2).
**Every `<p>:knots` array in the tables below is accompanied by a `uniform token <p>:interpolation`,
allowed `linear | catmullRom | bspline | constant`, default `catmullRom`** (ADR §9.2 R11); the tables
name only the knots array to stay readable. `catmullRom` and `bspline` are the two four-point basis
evaluations; `constant` and `linear` are the trivial ones. The token decides how the 257-entry LUT is
built, so it is part of the property set C1 freezes at the end of M1.
`02-schema.md` §2.13 and §2.17 declare the same set and the same default, and §2.17 states how each
token builds its LUT; if the two ever diverge, R11 is the ruling and 02 §2.17 is the registry row to
correct. The remaining cross-document corrections this document raises are collected in §0.13.

### 0.13 Corrections this document raises against its siblings

Everything below is a *sibling* edit, not a change to this document, and every row was re-verified
against the sibling as it stands today (ADR §9.5 R45). `02-schema.md` is the normative property
registry (ADR §9.2 R7) and gains what a sibling needs (R8). The other three registries already carry
everything this document needs and are cited, never restated:
`09-performance-and-benchmarks.md` §5 is the gate registry (R40);
`10-build-dependencies-testing.md` §3.5 is the single variable registry (R35) and already lists
`USDGEN_OP_CHECKS` (default `0`); `08-tools.md` §1.4 is the C ABI (R31) and already lists
`int UsdGenImaging_SetMaskVisualisation(const char *opPath);  /* "" clears (5.5) */`.

**No row is open.** The last one — `02-schema.md` §2.5's `usdGen:seed` doc line, which claimed that
two `UsdGenClump`s with the same seed are uncorrelated — now states what a per-use salt actually buys
(§0.6: a salt decorrelates *different uses*; the tool authors a distinct seed per operator prim), and
§8 test 6 asserts that guarantee.

**Rows the writing pass closed, re-verified closed, and not to be re-raised.**
`02-schema.md` §2.5's `usdGen:algorithmVersion` fallback is `0` (R17, §0.9). `02-schema.md` §5 and
`05-static-curves-and-deformation.md` §2.4 carry `uint64[] primvars:usdGen:curveId` (R12, §0.6).
`02-schema.md` §2.19.1 declares the multi-key fold-left `UsdGenHash64` / `UsdGenHash32` /
`UsdGenHash01` argument lists §0.6 spells out. `02-schema.md` §2.13 carries the whole mask block
§5.2 quotes — `remap`'s `max(y - x, 1e-6)` guard, `lockedCurveSuppression(c)` inside the clamp, the
`usdGen:blend` factor in `w(c, i)` — and declares `usdGen:mask:influenceWidth`. `02-schema.md`
§2.7.1 declares `usdGen:length:mode` with exactly the three tokens §2.10 uses, and carries no note
about a six-token spelling. `02-schema.md` §2.7.2 reserves every v2 name §3 uses, including
`UsdGenDisplace`'s and `UsdGenExprOp`'s `usdGen:mode`, `usdGen:expr:returnType` and `UsdGenPart`'s
`part:curves` / `part:radius` / `part:strength`. `02-schema.md` §2.6, §2.7.1 and §2.8 declare every
v1 parameter §2 lists, including `flip`, `perGuide`, `directionVector`, `directionPrimvar`,
`uvBlend`, `useUniqueGuide`, `lockRoots`, `searchRadius`, `numNeighbors`, `direction:source`,
`direction:knots`, `scaleRandom`, `widthToo` and `restoreSegmentLengths`.

---

## 1. Catalogue

### 1.1 Column definitions

**Kind** — `gen` (generator, `UsdGenGenerator`), `sty` (styler, `UsdGenStyler`), `def` (deformer,
`UsdGenDeformer`), `cap` (chain cap: `UsdGenFreeze`), `out` (`UsdGenInstance`). **Topo** — the
declared `UsdGenTopoFx`. **Space** — the class the authored default `usdGen:space = "auto"` resolves to for this type. **Surface read** — which surface
generation `Evaluate`/`Capture` samples, per §0.7 (`usdGen:readPhase = @<abs prim path>` retargets
*which prim*; the other three tokens select *which generation*).
**Capture cache** — what the capture epoch holds; the memory cost of a mute (§0.4). **Cost** — `A`
per-curve parallel, `B` needs a spatial structure at capture, `C` per-CV surface query
(`research/A7-prior-art-grooming.md` §9). **Tier** — v1 = the M0–M7 deliverable set, v2 = M8
breadth, v3 = later (ADR §9.5 R38, which amends ADR §6); **M** — the milestone that ships it
(ADR §7, as fixed by R38).

### 1.2 v1 — the shippable groom

**v1 is the M0–M7 deliverable set** (ADR §9.5 R38, which amends ADR §6's tier labels), so
`UsdGenPtexMap` (M4), `UsdGenInstance` (M6) and motion profiles P0/P1/P2 (M7) are v1, and
`UsdGenScatter`'s `uniform` mode is not.

| Type | Kind | `usdGen:mode` | Topo | Space | Surface read | Cost | Capture cache | Emitted primvars | Parity (XGen / Unreal / Houdini) | M |
|---|---|---|---|---|---|---|---|---|---|---|
| `UsdGenScatter` | gen | `random` (M1) \| `atGuides` (M3) \| `points` (M5) \| `uniform` (v2, M8) | `CurveCount` | rest | rest | A (+B relax) | face areas, per-face counts, ids, root frames, Morton order | `st` (uniform float2, root UV), `hairId` | Generator modes / — / Scatter + Hair Generate density | M1/M3/M5 |
| `UsdGenGrow` | gen | — | `CvCount` | rest | rest | A | per-curve length draws | — | Splines CV count/length / — / Guide Initialize | M1 |
| `UsdGenGuideInterpolate` | gen | — ¹ | `CvCount` | rest | rest | B → A | kd-tree, `guideIdx[3]`, `guideW[3]`, region ids | `guideIndex`, `guideWeight` (uniform, `elementSize = 3`, §2.3) | relative interpolation / `groom_closest_guides` + `groom_guide_weights` / Hair Generate | M3 |
| `UsdGenCurveSource` | gen | — ¹ | `Both` | rest | none | A | loaded buffer, id sort, epoch check | passes through C3 primvars | `xgmCurveToSpline` / Alembic import / Guide Groom input | M2 |
| `UsdGenDeform` | def | `rigidFrame` \| `rbf`(v2) \| `pointDeform`(v2) | `None` | **deformed** | rest + deformed | A | root→face binding, rest frames | — | patch follow / Binding / Guide Deform | M2 |
| `UsdGenFreeze` | cap | — ¹ | `None` | inherits | none | — | frozen buffer handle | — | Groom Bake / — / File Cache | M2 |
| `UsdGenSculptLayer` | sty | — | `None` | rest | none | A | id→slot map, root frames | — | `xgmModifierSculpt` / — / Guide Groom edits | M2 |
| `UsdGenClump` | sty | — ¹ | `None` | rest | rest | B → A×levels | clump centres, kd-tree, `clumpId[level]`, stray coins, rest lengths | `clumpId_<n>` (uniform int) | Clumping (classic + IGS) / `Clump ID` / Hair Clump 2.0 | M3 |
| `UsdGenNoise` | sty | — | `None` | rest | rest | A | correlation hashes, magnitude ramp LUT | — | Noise / — / Frizz | M1 |
| `UsdGenLength` | sty | — ¹ | `CurveCount` | rest | rest | A | rest arc lengths, surviving id set | — | Cut / `Clip Length` / Set Length + Cull Threshold | M1 |
| `UsdGenWidth` | sty | — | `None` | rest | rest | A | width ramp LUT | `widths` (vertex float) | Width Ramp / root+tip scale / Thickness | M1 |
| `UsdGenSmooth` | sty | `alongCurve` (v1) \| `neighbours` (v3) | `None` | rest | none | A | — (only the universal mask cache of §5.2) | — | Smooth brush / — / Smooth | M3 |
| `UsdGenDirection` | sty | `rigid` \| `perSegment` | `None` | rest | rest | A | root frames, `direction:knots` LUT | — | Tilt U/V/N, Around N / — / Set Direction + Lift | M3 |
| `UsdGenScale` | sty | — | `None` | rest | none | A | rest arc lengths | — | `xgmModifierScale` / — / Set Length (multiply) | M4 ² |
| `UsdGenResample` | sty | — ¹ | `CvCount` | rest | none | A | per-curve arc-length tables | — | `Uniform CVs` / Rebuild Type `Reparam` / Resample | M3 |
| `UsdGenInstance` | out | — (`usdGen:primitive = cards \| archives \| spheres`) | emits an instancer | **deformed** | none | A | prototype bounds, per-instance draws, the hashed prototype assignment from `usdGen:instance:weights` | `hydra:instanceTranslations/Rotations/Scales` (`instance` interpolation) plus `instancerTopology/prototypes` and `instancerTopology/instanceIndices` (`06-imaging.md` §4.3) | Cards / Archives / Spheres | M6 |

¹ **These six types have no `usdGen:mode`.** Their concept switch is a namespaced or differently
named token, because it is one parameter among many rather than the operator's identity:
`UsdGenGuideInterpolate` → `usdGen:blendMethod` (§2.3); `UsdGenClump` → `usdGen:clump:method`
(§2.8, ADR §9.2 R10); `UsdGenLength` → `usdGen:length:mode` (§2.10); `UsdGenResample` →
`usdGen:distribution` (§2.15); `UsdGenFreeze` → `usdGen:frozen:mode` (§2.6); `UsdGenCurveSource` →
`usdGen:idSource` plus `usdGen:lane` (§2.4). All six spellings are `02-schema.md` §2.6–§2.9's, and
all six are routed as recompiles or as topology by `02-schema.md` §6.1–§6.2, exactly like a
`usdGen:mode` (§0.3 rule 2). Only `UsdGenScatter`, `UsdGenDeform`, `UsdGenSmooth` and
`UsdGenDirection` carry a `usdGen:mode`.

² `UsdGenScale` is **M4**, scheduled there by `11-roadmap.md` §2.5 ("Plus `UsdGenScale`, the last
v1 styler moved in by ADR §6 and scheduled here") and listed as `UsdGenScale (M4)` in
`11-roadmap.md` §6.1; M4's exit gates include a T0 value test and a T1 invalidation test for it. The
placement is cheap because it is a one-kernel styler that needs no capture machinery beyond M3's
(rest arc lengths), and, in `11-roadmap.md` §2.5's own words, because its mask input is the map
block that milestone delivers.

`UsdGenLength` declares `CurveCount` **unconditionally** (§0.7): the declaration is static and the
effect is data-dependent, so with `length:mode != "cull"` and `cullThreshold == 0` the capture emits
the identity id set and no topology publish happens.

v1 also ships the container types `UsdGenGroom`, `UsdGenDescription`, `UsdGenGuideSet`, the API
schemas `UsdGenMaskAPI`, `UsdGenLookAPI`, `UsdGenRestAPI`, `UsdGenCurveAPI`, and the map prims
`UsdGenImageMap`, `UsdGenPtexMap`, `UsdGenExprMap`, `UsdGenPaintMap`, `UsdGenNoiseMap`,
`UsdGenCombineMap`, `UsdGenGuideProximityMap` (M4; parameters in `07-look-maps-expressions.md`).
`UsdGenPtexMap` is v1, scheduled M4 (ADR §9.5 R38, which amends ADR §6's v2 label); it is a
`UsdGenMap`, not a `UsdGenOperator`, so the operator columns do not apply to it — no `usdGen:input`,
no topology effect, no space, no capture cache of its own. Maps are not operators: they are fields
sampled at capture and are wired by `rel`, never by `usdGen:input`.

### 1.3 v2 — the M8 styling vocabulary

| Type | Kind | Concept switch | Topo | Space | Cost | Capture cache | Emitted | Parity | M |
|---|---|---|---|---|---|---|---|---|---|
| `UsdGenCurl` | sty | `usdGen:axisMode = curveTangent \| guide` | `None` | rest | A | minimal-twist frames, ramp LUTs | — | Coil / IGS Curl / Blender Curl / Hair Clump curling | M8 |
| `UsdGenBend` | sty | `usdGen:axisMode = rootDirection \| uniform \| attribute` | `None` | rest | A | ramp LUT, per-curve angle draws | — | Bend Param/U/V / — / Bend | M8 |
| `UsdGenStraighten` | sty | — | `None` | rest | A | rest chord vectors | — | — / — / Straighten | M8 |
| `UsdGenDisplace` | sty | `usdGen:mode = height \| vector` | `None` | rest | A | map samples per root, root normals | — | IGS Displacement / — / Displace | M8 |
| `UsdGenWave` | sty | — | `None` | rest | A | root frames, arc-length tables | — | — / — / Wave | M8 |
| `UsdGenPart` | sty | — | `None` | rest | B | parting-curve kd-tree, per-curve side + crossing weight | `partId` (uniform int) | Region maps / — / Guide Partition | M8 |
| `UsdGenExprOp` | sty | `usdGen:mode = cv \| curve` | `None` | authored | A | **the whole result** (capture-time only) | authored | SeExpr modifier | M8 |

`UsdGenScatter` mode `uniform` is also v2/M8 (R38) — the properties it reads (`usdGen:spacingU`,
`:spacingV`, `:jitter`) ship in the schema from M1, so the upgrade is a value edit (§2.1).
v2 also brings: `TsSpline` ramps in the UI (R11's optional whole-spline transport), sculpt-layer
**rebase**, progressive generation (gate T-5), the in-place overlay path (gate S-10), and the
region/parting map consumers on `UsdGenGuideInterpolate` and `UsdGenClump` growing an operator-based
source (`UsdGenPart`).

`UsdGenCurl` and `UsdGenBend` spell their switch **`usdGen:axisMode`** (`02-schema.md` §2.7's
spelling), not `usdGen:mode`: both branches read the same capture and differ only in which frame the
displacement uses, so the token is an ordinary structural token rather than a term of `d(n)`
(§0.3 rule 2, ADR §4.2.1). `UsdGenDisplace` and `UsdGenExprOp` carry a real `usdGen:mode` because the
branch changes what the capture holds.

### 1.4 v3 — needs a mechanism v1 and v2 do not have

| Type | Kind | Space | Cost | Engine feature it waits for |
|---|---|---|---|---|
| `UsdGenCollide` (Shrinkwrap) | def | deformed | C | a collider BVH rebuilt per surface epoch |
| `UsdGenWind` / `UsdGenForce` | def | deformed | A | a time integrator and a time-dependent tail; a world-space class, if one is ever needed, is a `02-schema.md` §8.2 token addition with fallback `auto` (ADR §9.2 R9) |
| `UsdGenSmooth` `mode = "neighbours"` | sty | deformed | B | the two-pass gather/scatter node kind on a per-frame grid (ADR §4.2 item 5) |
| `UsdGenBraid` | sty | rest | A | a topology multiplier (×3) inside a styler |
| `UsdGenSimSource` | gen | rest | A | cache re-timing and the ragged import path at scale |

Also v3: an OpenExec backend behind the same `UsdGenOp` interface (S16), a third-party operator ABI
(contract C4 covers only the C ABI and the pxr_boost array surface; `UsdGenOpRegistry` is internal in
v1/v2, ADR §3), and a GPU tail. The **P2 sampled** motion profile is v1/M7 (R38), not v3; what v3
adds is P2 across time-dependent v3 deformers.

### 1.5 What is deliberately not an operator

Density scaling, `usdGen:tileTarget`, `usdGen:motion:*`, curve basis/type/wrap and
`displayStyle/refineLevel` live on `UsdGenGroom`/`UsdGenDescription` — publication policy, not
grooming (ADR §2.3). Machine tuning (`USDGEN_CHUNK_SIZE`, `USDGEN_THREAD_LIMIT`,
`USDGEN_MEMORY_BUDGET_MB`) is env/config, never authored into an asset.

---

## 2. The v1 operators

Each subsection gives purpose, parameters, modes, the capture step, the evaluate kernel in
pseudocode, emitted primvars and interactions. The parameter tables omit the base block of §0.2 and
the mask block of §5, which every operator has. `A7 §9.x` references name the reference algorithm in
`research/A7-prior-art-grooming.md`.

### 2.1 `UsdGenScatter` — roots on the surface (A7 §9.1 G1–G4)

**Purpose.** Produce the root set: one entry per future curve with a stable id, a bound face, a
barycentric/UV position on the rest surface and a root frame. Every other generator either consumes
roots or replaces them.

| Property | Type | Default | Range | Doc |
|---|---|---|---|---|
| `usdGen:mode` | `uniform token` | `"random"` | `random`,`uniform`,`points`,`atGuides` | placement rule. `random` ships M1, `atGuides` M3, `points` M5; **`uniform` is v2 (M8)** (ADR §9.5 R38). An unimplemented mode is a compile error naming the prim and the mode, never a silent downgrade |
| `usdGen:density` | `float` | `100` | ≥ 0 | hairs per square **stage unit on the rest surface** (ADR §2.3); non-uniform surface scale does not change counts |
| `usdGen:relaxIterations` | `int` | `0` | 0–50 | Poisson-disk relaxation passes on the surface |
| `usdGen:areaCompensation` | `bool` | `true` | — | divide by rest face area so mixed face sizes give even density |
| `usdGen:spacingU`, `:spacingV` | `float` | `0.02` | > 0 | **v2 (M8)**: `uniform` mode row/column spacing in UV. Declared from M1 so the v2 upgrade is a value edit (`02-schema.md` §2.6) |
| `usdGen:jitter` | `float` | `0` | 0–1 | **v2 (M8)**: `uniform` mode positional jitter |
| `usdGen:rootPrims` | `int[]` | `[]` | — | `points` mode: face index per root (what the Place brush authors) |
| `usdGen:rootUVs` | `texCoord2f[]` | `[]` | — | `points` mode: UV per root, same length as `rootPrims` (a compile error otherwise) |
| `usdGen:guides` | `rel` | — | — | `atGuides` mode: the guide set whose roots seed this one |
| `usdGen:flip` | `bool` | `false` | — | grow on the back side of the surface (A7 §9.1 G1) |
| `usdGen:perGuide` | `int` | `1` | ≥ 1 | `atGuides` mode: roots emitted per guide root |

Every row above is declared in `02-schema.md` §2.6 with the same type and default; this table adds
only the range column and the reference-algorithm citation. The same holds for every v1 parameter
table in §2: **§2 proposes no property `02-schema.md` does not already declare**, and neither does
§3's v2 table (`02-schema.md` §2.7 and §2.7.2). Where a range, a token set or a default here and in
02 ever disagree, 02 wins (ADR §9.2 R7) and §0.13 records the correction.

**Capture** (the whole operator is capture; `Evaluate` is a copy):

```
# mode = "random" (A7 §9.1 G1)
for face f in surface (rest topology, restricted to bound GeomSubsets):
    expected = density * areaRest(f) * meanMask(f)
    n_f      = floor(expected) + (UsdGenHash01(UsdGenHash64(seed,f,kSaltScatter), kSaltScatter) < frac(expected))
    for k in 0..n_f-1:
        curveId    = UsdGenCurveId(seed, f, k)                # uint64, stable across density edits
        (b0,b1,b2) = low-discrepancy sample k seeded by curveId
        emit root(prim=f, uv=b, frame = orthonormal(N_rest, dPdu_rest))
if relaxIterations > 0: Poisson-disk relax on the surface (cost class B, kd-tree over roots)
sort roots by Morton code of the rest position           # surface-major chunk order (ADR §4.1)
# densityScale / renderDensityScale are NOT applied here: they decimate the captured root set by
# stable id at publish time (§0.6, ADR §2.3), so scrubbing a density slider never re-runs Capture.
```

The other three modes replace only the emission loop; the relax, the Morton sort and the decimation
rule are identical.

```
# mode = "points" (M5; A7 §9.1 G3) — rootPrims[] and rootUVs[] must be the same length
for i in 0..rootPrims.size()-1:
    curveId = UsdGenCurveId(seed, uint32(rootPrims[i]), uint32(i))   # inserting a root renumbers none
# mode = "atGuides" (M3; A7 §9.1 G4)
for guide g, for k in 0..perGuide-1:
    uv      = guideRootUV[g] + lowDiscrepancyDisc(k) * spacingU
    curveId = UsdGenHash64(guideCurveId[g], uint64(k), kSaltScatter)   # follows the guide id, not its index
# mode = "uniform" — v2 (M8); A7 §9.1 G2
for face f: grid the face's UV extent at (spacingU, spacingV); for cell (i,j):
    uv      = cellCentre(i,j) + jitter * (UsdGenHash01(UsdGenCurveId(seed,f,i*gridV+j), kSaltScatter) - 0.5) * cellSize
    curveId = UsdGenCurveId(seed, f, i * gridV + j)
```

**Stable ids.** In `random` mode `curveId = UsdGenCurveId(seed, faceIndex, k)` (§0.6, a `uint64`),
so raising `density` only appends to each face's tail: existing ids, sculpt deltas and clump ids
survive (XGen `Generator Seed` semantics, A7 §1.1). In `points` mode the id keys on the authored face index and slot, so the Place brush can insert a root
without renumbering the others. In `atGuides` mode the id keys on the *guide's* `curveId`, so
reordering the guide set does not renumber the hairs. In the v2 `uniform` mode the id keys on the
grid cell, so a `spacingU`/`spacingV` edit renumbers and the tool warns exactly as it does for a seed
edit. A **seed** edit invalidates ids in every mode; the tool warns first.

**Emitted primvars.** `st` (uniform `float2`, the root UV — what UV-mapped scalp colour reads in
Storm, S37) and `hairId` (uniform float, §0.6). `rootPrim`, `rootUV` and `rootFrame` stay internal
and reach Hydra only through the C3 contract on a freeze.

**Interactions.** `usdGen:density` multiplies the resolved mask (§5), so a paint map on the mask
block *is* XGen's density Mask — which also means `mask:amount = 0` on a Scatter yields zero roots,
not a pass-through (§8). `mode = "atGuides"` takes guide roots from `usdGen:guides`, or from
`usdGen:input` when that carries `role = guide` curves. Decimation by
`densityScale`/`renderDensityScale` happens **after** capture, by the salted hash of §0.6, so it
never re-runs the scatter. A `GeomSubset` edit on `usdGen:surface` is a recapture (ADR §2.3).

### 2.2 `UsdGenGrow` — roots to straight strands (A7 §9.1 G5)

**Purpose.** Turn a root set into curves without guides: the shortest path from a scatter to
something drawable, and the operator M1 ships with.

| Property | Type | Default | Range | Doc |
|---|---|---|---|---|
| `usdGen:segments` | `int` | `8` | 2–64 | CVs per strand (pinned cubic: `curveVertexCounts[i] ≥ 2`, `pxr/usd/usdGeom/schema.usda:1678`) |
| `usdGen:length` | `float` | `1.0` | ≥ 0 | strand length in stage units |
| `usdGen:lengthRandom` | `float2` | `(1,1)` | ≥ 0 | per-curve multiplier drawn uniformly in [x,y] |
| `usdGen:length:source` | `rel` | — | — | a `UsdGenMap` sampled per root, multiplying `length` |
| `usdGen:direction` | `uniform token` | `"surfaceNormal"` | `surfaceNormal`,`attribute`,`vector` | growth direction (`02-schema.md` §2.6, which declares it `structural`). This is the one flat name with two types across the catalogue (§0.2); it keeps 02's spelling and is routed per prim type by `02-schema.md` §6.1 |
| `usdGen:directionVector` | `vector3f` | `(0,1,0)` | — | used when `direction = "vector"` |
| `usdGen:directionPrimvar` | `token` | `""` | — | surface primvar name when `direction = "attribute"` |
| `usdGen:lift` | `float` | `0` | −90…90 | degrees rotated away from the surface tangent plane |
| `usdGen:uvBlend` | `float` | `0` | 0–1 | blend the direction toward `dPdu`/`dPdv` (tangential to skin) |

**Capture.** Nothing but the CV count and the per-curve length draws
(`len[c] = length * lerp(lengthRandom.x, lengthRandom.y, UsdGenDraw01(seed, curveId[c], kSaltGrow)) * map(c)`),
stored as one `VtFloatArray`. **Evaluate** (cost A, the cheapest kernel in the set):

```
for curve c in chunk:
    dir = rotate(baseDir(c), axis = rootFrame[c].B, angle = lift)      # then uvBlend toward dPdu
    for i in 0..cvCount-1:
        s = i / (cvCount-1)
        P[c][i] = root[c] + dir * len[c] * s
```

**Emitted primvars.** None; it *sets* `curveVertexCounts` and therefore the vertex `hairT`
(root→tip, ADR §5.3). **Interactions.** `Grow` and `GuideInterpolate` are alternatives, not a
sequence — whichever runs last sets the CV count; a `Grow` after a `GuideInterpolate` discards the
interpolated shape, and the stack editor flags it "replaces upstream shape".

### 2.3 `UsdGenGuideInterpolate` — hair from guides (A7 §9.1 G6)

**Purpose.** The production "curve generator": fill a root set with curves interpolated from a sparse
guide set, in the guides' root frames, with region and parting constraints.

| Property | Type | Default | Range | Doc |
|---|---|---|---|---|
| `usdGen:guides` | `rel` | — | — | a `UsdGenGuideSet` **or** an operator (reference lane, §0.10 rule 3) |
| `usdGen:maxGuides` | `int` | `3` | 1–8 | guides blended per hair; 3 is Unreal's arity and the arity of the emitted primvars |
| `usdGen:influenceRadius` | `float` | `4.0` | > 0 | search radius in rest stage units (`02-schema.md` §2.6) |
| `usdGen:influenceDecay` | `float` | `2.0` | ≥ 0 | weight = `(1 − d/R)^decay` |
| `usdGen:maxGuideAngle` | `float` | `90` | 0–180 | reject a guide whose rest normal differs by more than this |
| `usdGen:blendInSkinSpace` | `float` | `1.0` | 0–1 | 1 = blend guide offsets in the guide's root frame (XGen relative interpolation); 0 = blend world offsets |
| `usdGen:blendMethod` | `uniform token` | `"linearBlend"` | `linearBlend`,`extrudeAndBlend` | Houdini's two shapes (`02-schema.md` §2.6) |
| `usdGen:useUniqueGuide` | `bool` | `false` | — | 1 guide per hair (Unreal) |
| `usdGen:randomizeGuide` | `float` | `0` | 0–1 | jitter the guide choice per hair |
| `usdGen:cvCount` | `int` | `8` | 2–64 | CVs per generated curve (`02-schema.md` §2.6) |
| `usdGen:length:source` | `rel` | — | — | a `UsdGenMap` whose value multiplies the interpolated strand length |
| `usdGen:clumpCrossover` | `float` | `0` | 0–1 | allow weight to cross a clump/region boundary (Houdini `Clump Crossover`) |
| `usdGen:mask:region` | `rel` | — | — | region map: guides only influence hairs of their own region id (§5.3) |

**Capture** (cost B; the operator gate E-4 is written for):

```
tree = UsdGenKdTree(guideRootsRest)                                    # nanoflann 1.12.1 (S38)
WorkParallelForN over roots r:
    cand = tree.knn(r, maxCandidates = 8, radius = influenceRadius)
    drop g if angle(N_rest(r), N_rest(g)) > maxGuideAngle
    drop g if regionId(g) != regionId(r)                               # region map, capture-time
    drop g if a parting curve separates r from g       (v2 UsdGenPart; weight *= 1 - strength)
    w_g = (1 - d_g / influenceRadius)^influenceDecay
    if useUniqueGuide: keep argmax(w) [jittered by randomizeGuide]
    else: keep top maxGuides, normalise, w *= guideBlend[g]             # per-guide usdGen:blend
    cap.i0/i1/i2 = guideIdx[0..2] ; cap.f0/f1/f2 = guideW[0..2]
resample every active guide to cvCount into ref.local[] (root-local offsets), once per capture
```

**Evaluate** (cost A, three multiply-adds per CV over contiguous planar data):

```
for curve c in chunk:
    F = rootFrame[c]
    for i in 0..cvCount-1:
        local   = w0*ref.local[i0][i] + w1*ref.local[i1][i] + w2*ref.local[i2][i]
        P[c][i] = root[c] + (blendInSkinSpace ? F * local : local)
```

`ref.local` is recomputed once per frame when the reference lane runs, so the hair loop never inverts
a matrix and never touches the tree.

**Emitted primvars.** `guideIndex` (`int[]`, `uniform`, `elementSize = 3`) and `guideWeight`
(`float[]`, `uniform`, `elementSize = 3`) — the bare names ADR §9.3 R24 fixes, published on the tile
as the Hydra locators `primvars/guideIndex` and `primvars/guideWeight` (`06-imaging.md` §4.1;
`02-schema.md` §2.6 declares the same unprefixed spelling), which is what §0.5, §0.11 and §1.2 of
this document use. `elementSize` is the USD
encoding for "three values per element" — `pxr/usd/usdGeom/primvar.h:322-331` declares
`int GetElementSize()` / `bool SetElementSize(int)`. It is **not** `int3`/`float3`: `int3` is a
`GfVec3i` (`pxr/usd/sdf/types.h:367`), a different value type. The arity is Unreal's
`groom_closest_guides` / `groom_guide_weights`, so a bake round-trips (ADR §2.3; A7 §2.1;
`02-schema.md` §2.6).

**Interactions.** Region maps and parting are one mechanism at capture: `usdGen:mask:region` (§5.3)
gives every root and every guide a region id, and a mismatch rejects the candidate;
`clumpCrossover > 0` lets that fraction of the weight survive a mismatch, which is how Houdini avoids
hard seams. **Failure mode reported, not hidden:** a root with zero candidates is a bald patch; the
node counts them and `UsdGenNodeStats::warnings` surfaces "guide angle rejected N % of candidates" in
the stack profiler (`design/proposal-artist.md` §4.10) instead of emitting a straight-up hair.

### 2.4 `UsdGenCurveSource` — frozen, imported and simulated curves enter here (A7 §9.3)

**Purpose.** Bring a stage `BasisCurves` set into the graph as if a generator had produced it. A
frozen prim and a generated prim are the same kind of styler input (S42) — this is what makes
"freeze an operator's output and comb it" a configuration rather than a mechanism.

| Property | Type | Default | Doc |
|---|---|---|---|
| `usdGen:curves` | `rel` | — | exactly one C3 `BasisCurves` target (`05-static-curves-and-deformation.md` §2.1) |
| `usdGen:useRest` | `bool` | `true` | `true` = the loaded `points` are a rest pose and `primvars:rest` (or `points` at `Default()`) is the rest buffer; `false` = the points are already deformed |
| `usdGen:idSource` | `uniform token` | `"primvar"` | `primvar` \| `index` — where `curveId` comes from; `index` fabricates `curveId = i` for curves with no `primvars:usdGen:curveId` (`02-schema.md` §2.6) |
| `usdGen:lane` | `uniform token` | `"hair"` | `hair` \| `reference` — `reference` puts the set in the un-chunked reference lane (ADR §4.1 I3) |
| `usdGen:expectEpoch` | `uniform string` | `""` | empty = never stale; otherwise must match `primvars:usdGen:frozenEpoch`, prefix `usdgen1:sha1:` (ADR §2.3) |
| `usdGen:staleAction` | `uniform token` | `"warn"` | `warn` \| `ignore` \| `block` |
| `usdGen:resampleTo` | `uniform int` | `0` | `0` = keep the source CV counts (ragged); `> 0` = resample every curve to that count at capture — the import tool's "resample to N", with the measured E-1r penalty shown |
| `usdGen:rebind` | `uniform token` | `"onError"` | `never` \| `onError` \| `always` — recompute `skinprim`/`skinprimuv` from the rest surface |

There is **no `usdGen:mode` on `UsdGenCurveSource`**: provenance (frozen / imported / simulated) is
documentation, not a token, and the behavioural forks are `usdGen:useRest` (already-deformed points),
`usdGen:idSource` and `usdGen:lane`. The five rows `usdGen:lane`, `usdGen:expectEpoch`,
`usdGen:staleAction`, `usdGen:resampleTo` and `usdGen:rebind` are ADR §9.2 R8's fold-ins and are
declared in `02-schema.md` §2.6.

**Capture.** Load the buffer from the scene index (never from a `UsdStage`, S8), sort by
`primvars:usdGen:curveId`, verify the epoch, and record whether the result is uniform-CV (fast path)
or **ragged** (`cvCount == 0` + `cvOffsets`). Ragged is v1 because imports and sim caches are ragged
by nature (ADR §4.2.2); gate **E-1r** holds it to ≤ 2× the uniform path. **Evaluate.** A copy. A
time-sampled source authored with `usdGen:useRest = false` is classified `deformedSpace`, so it is
re-read at `time + shutterOffset` and pushes everything downstream into the motion tail
(`05-static-curves-and-deformation.md` §2.6). **Emitted primvars.** Whatever
C3 carries. Consumers test for a *value*, never for presence — `velocities`, `accelerations` and
`normals` are always listed by the gprim data source with size 0 (MEASURED,
`research/G-freeze-bake-undo-and-frozen-reentry.md` §2.1 — a probe observation that has no `EV-nnn`
row in `appendix-A-evidence-ledger.md` yet and should get one).

### 2.5 `UsdGenDeform` — curves follow the surface (A7 §9.1 G7, R3)

**Purpose.** Transport rest-space curves onto the deformed surface, whoever deformed it — usdRig,
UsdSkel, or any other modifier upstream in the scene index (R5, S1, S3).

| Property | Type | Default | Doc |
|---|---|---|---|
| `usdGen:mode` | `uniform token` | `"rigidFrame"` | `rigidFrame` (v1, per-root frame transport) \| `rbf` (v2, an Unreal-style displacement field from ≤ `rbfSamples` surface samples) \| `pointDeform` (v2, per-CV weights over the nearest surface points). An unimplemented mode is a compile error naming the prim and the mode, never a silent downgrade (`05-static-curves-and-deformation.md` §4.1) |
| `usdGen:twistAware` | `bool` | `true` | build the root frame from `dPdu` so the strand twists with the surface |
| `usdGen:rbfSamples` | `int` | `100` | `mode = "rbf"` only; ≤ 100, the Unreal binding arity. Cost class B |
| `usdGen:preserveShape` | `float` | `0.0` | 0 = off. `> 0` runs the Cosserat stretch/bend relaxation (A7 §9.1 G7, §3.5). **v2**; in v1 a non-zero value is one warning and is ignored |
| `usdGen:preserveShape:iterations` | `int` | `0` | Cosserat iteration count, v2 |
| `usdGen:lockRoots` | `bool` | `true` | pin CV 0 exactly to the deformed root position (A7 §9.1 G7) |

`rbf`, `pointDeform` and `preserveShape` exist in the schema from v1 so that the v2 upgrade is a
value edit, not a prim swap (`05-static-curves-and-deformation.md` §4.3); the v2 catalogue ships in
M8 (ADR §7). A `pointDeform` neighbourhood size lands with that mode, not before.

**Capture.** Root→face binding (`skinprim`, `skinprimuv`) and the **rest** root frames
`F_rest[c] = orthonormal(N_rest, dPdu_rest)`; for `pointDeform`, the k nearest rest surface points and
their weights. All of it survives a frame change; none of it survives a surface *topology* change.
**Evaluate** (deformed space, re-runs per frame and per motion sample):

```
for curve c in chunk:
    F_anim = orthonormal(N_anim(skinprim[c], skinprimuv[c]), dPdu_anim(...))   # twistAware
    M      = F_anim * inverse(F_rest[c])
    for i: P[c][i] = rootAnim[c] + M * (P_rest[c][i] - rootRest[c])
    if lockRoots: P[c][0] = rootAnim[c]
```

**Interactions.** `UsdGenDeform` is the boundary between the rest head and the deformed tail (§0.7):
everything below it re-runs per shutter offset, everything above it is cached across the frame range.
Put stylers above it when their look should be rest-stable (the normal case), below it only when they
must react to the pose. The surface it reads is the post-flattening world-space mesh with
`resetXformStack = true`; the tile carries `xform = surface world matrix` plus surface-local points
and re-dirties its own `xform` when the surface's is dirtied (S4).

### 2.6 `UsdGenFreeze` — the chain cap (A7 §1.4 Groom Bake, §9.3)

**Purpose.** Snapshot the upstream result and read it back instead of evaluating upstream. The freeze
**caps** the chain: upstream operators stay authored and greyed in the stack editor, and unfreezing
is one token edit — never a delete (ADR §2.3).

| Property | Type | Default | Doc |
|---|---|---|---|
| `usdGen:frozen:curves` | `rel` | — | the sibling `BasisCurves` snapshot under contract C3 |
| `usdGen:frozen:mode` | `uniform token` | `"frozen"` | `frozen` (read the snapshot, do not evaluate upstream) \| `live` (pass through; the snapshot is kept, marked stale) |
| `usdGen:frozen:epoch` | `uniform string` | `""` | must equal the target's `primvars:usdGen:frozenEpoch`; on mismatch usdGen emits one `TF_WARN` and a stack-editor badge and **keeps rendering the frozen buffer** — it never falls back to live evaluation (ADR §9.2 R18). Re-freezing is an explicit artist action |
| `usdGen:frozen:tier` | `uniform token` | `"session"` | `session` \| `sublayer` \| `payload` — advisory, tool-facing; the evaluator never reads it (ADR §9.2 R18). The tokens are the only spelling; S42's "T1/T2/T3" names are retired (ADR §9.1 R1) |

ADR §2.3 writes these in brace shorthand without the namespace prefix; the full names carry the
mandatory `usdGen:` prefix (ADR §2.1, §9.2 R6). A stale epoch never changes the evaluated mode: the
node keeps reading the snapshot, and `usdGen:staleAction` — which *does* select warn/ignore/block —
exists only on `UsdGenCurveSource` (R18). `usdGen:enabled = false` on a `UsdGenFreeze` is
topology-structural (§0.4): the chain above re-enters and the counts may change, which is why
unfreezing is a `usdGen:frozen:mode` token edit on a still-enabled node and not a mute.
`Capture` shares `UsdGenCurveSource`'s load path; `Evaluate` is a copy. Landing tiers, undo and the epoch algebra are in `05-static-curves-and-deformation.md`.
Freezes are siblings under `<Description>/Frozen/…` and the tool never re-authors that parent scope
(re-authoring a parent scope's `typeName` resyncs the whole subtree — **203** `PrimsAdded` over 200
sibling curve prims and 1.3–1.5 ms of scene-index work, MEASURED, ledger row `EV-052`,
`research/G-freeze-bake-undo-and-frozen-reentry.md` §4.2).

### 2.7 `UsdGenSculptLayer` — hand work that survives (A7 §9.3, XGen sculpt layers)

**Purpose.** Per-CV deltas in the root frame keyed by stable `curveId`, so surface deformation and
upstream parameter tweaks that keep ids still apply. This is what the Comb, Grab, Smooth, Length, Cut
and Freeze-paint brushes commit to (`08-tools.md`).

| Property | Type | Default | Doc |
|---|---|---|---|
| `usdGen:sculpt:weight` | `float` | `1.0` | layer weight 0–1 (XGen sculpt-layer semantics) |
| `usdGen:sculpt:curveIds` | `uint64[]` | `[]` | sorted ids that have deltas — 64-bit, matching `primvars:usdGen:curveId` (ADR §9.2 R12) |
| `usdGen:sculpt:cvOffsets` | `int[]` | `[]` | prefix offsets into `deltas`, size `curveIds.size() + 1` |
| `usdGen:sculpt:deltas` | `vector3f[]` | `[]` | per-CV delta |
| `usdGen:sculpt:space` | `uniform token` | `"rootFrame"` | `rootFrame` \| `object` |
| `usdGen:sculpt:epoch` | `uniform string` | `""` | the epoch the deltas were authored against |
| `usdGen:sculpt:lockedCurves` | `uint64[]` | `[]` | Freeze-brush ids: downstream stylers are zeroed for these curves |
| `usdGen:sculpt:rootPrims` | `int[]` | `[]` | per-delta root face index, parallel to `curveIds`, written with the deltas so "Rebase sculpt" can re-match by nearest root UV (ADR §9.2 R8) |
| `usdGen:sculpt:rootUVs` | `texCoord2f[]` | `[]` | per-delta root UV, same length as `curveIds` (ADR §9.2 R8) |

**Capture.** One `id → slot` map (a sorted-array binary search, not a hash map — determinism) plus
the root frames. **Evaluate.** `P[c][i] += weight * (space == rootFrame ? F[c] * delta : delta)`
with `weight = usdGen:blend * usdGen:sculpt:weight` (`05-static-curves-and-deformation.md` §6.2),
skipping curves with no slot. Multiple sculpt layers stack in chain order and blend additively.
`lockedCurves` is published to the graph as a per-curve suppression mask that every downstream styler
multiplies into its resolved mask. A `sculpt:epoch` mismatch is a badge plus one undoable
"Rebase sculpt" action that re-matches by nearest root UV (ADR §2.3; rebase itself is v2).

### 2.8 `UsdGenClump` — the operator the request is really about (A7 §9.2 S1)

**Purpose.** Pull hairs toward clump centres, with the full XGen parameter set and Houdini's fractal
multi-level scheme. Two `UsdGenClump` prims in one chain at different sizes are the canonical groom.

| Property | Type | Default | Range | Doc |
|---|---|---|---|---|
| `usdGen:clump:amount` | `float` | `0.5` | 0–1 | strength of the pull toward the clump curve |
| `usdGen:clump:profile:knots` | `float2[]` | `[(0,0),(1,1)]` | — | root→tip ramp on `amount` (XGen `Clump Scale`) |
| `usdGen:clump:centers` | `rel` | — | — | a curve set, a nested `UsdGenScatter`, or a map — the artist-visible clump source (ADR §2.3) |
| `usdGen:clump:density` | `float` | `4.0` | > 0 | clump points per square **stage unit on the rest surface**, used when `centers` is absent (`02-schema.md` §2.7.1 is the normative row, ADR §9.2 R7) |
| `usdGen:clump:size` | `float` | `1.0` | > 0 | clump radius in rest stage units (alternative parameterisation of `density`) |
| `usdGen:clump:seed` | `uniform int` | `0` | — | seed for the implicit centre scatter and every per-hair draw |
| `usdGen:clump:method` | `uniform token` | `"linearBlend"` | `linearBlend`,`extrudeAndBlend` | see the kernel |
| `usdGen:clump:volumize` | `float` | `0` | 0–1 | push hairs radially outward before clumping (XGen `Clump Volumize`) |
| `usdGen:clump:stray:amount` / `:rate` / `:falloff` | `float` | `0` / `0` / `1` | 0–1, 0–1, ≥0 | fraction of the pull removed / share of hairs chosen / along-curve falloff |
| `usdGen:clump:copy` / `:copyVariance` | `float` | `0` / `0` | 0–1 | blend a hair's shape toward its clump's neighbours; per-hair variance |
| `usdGen:clump:cut` | `float` | `0` | 0–1 | shorten a hash-chosen share of the clump's hairs by this fraction |
| `usdGen:clump:noise:amount` / `:frequency` / `:correlation` | `float` | `0` / `1` / `0` | — | per-clump noise, correlated between neighbouring clumps |
| `usdGen:clump:flatness` | `float` | `0` | 0–1 | flatten the clump cross-section (XGen `Flatness`) |
| `usdGen:clump:offset` | `float` | `0` | — | offset the clump centre along the surface normal |
| `usdGen:clump:curl:amplitude` / `:frequency` | `float` | `0` / `1` | ≥ 0 | curl applied to the clump curve *before* clumping (XGen `Curl`; `02-schema.md` §2.7). One float cannot express a curl: it needs a radius and a turn rate |
| `usdGen:clump:crossover` | `float` | `0` | 0–1 | let a hair be pulled by a neighbouring clump across a region boundary |
| `usdGen:clump:levels` | `int` | `1` | 1–4 | fractal levels |
| `usdGen:clump:level` | `uniform int` | `-1` | ≥ −1 | `-1` = auto: this node's ordinal among **all** `UsdGenClump` nodes of the compiled chain, disabled ones included, so enable/disable never renumbers a published primvar. An authored value ≥ 0 pins the emitted `clumpId_<n>` index (ADR §9.2 R10; `02-schema.md` §2.7.1) |
| `usdGen:clump:sizeReduction` | `float` | `0.5` | 0–1 | size multiplier per level (Houdini) |
| `usdGen:clump:tightnessReduction` | `float` | `0.8` | 0–1 | amount multiplier per level |
| `usdGen:clump:goalFeedback` | `float` | `1` | 0–1 | 1 = level L's centres derive from level L−1's **output**; 0 = from the node's input |
| `usdGen:preserveLength` | `float` | `1` | 0–1 | restore rest segment lengths root-locked after the pull. **Flat, shared with `UsdGenNoise`** (`02-schema.md` §2.7; artist §6.4 rule 3) — not `clump:preserveLength` |

`UsdGenClump` carries **no `usdGen:mode`**; its kernel branch is `usdGen:clump:method`, and its
`clumpId_<n>` index is `usdGen:clump:level` (ADR §9.2 R10). Both are graph-structural
(`02-schema.md` §6.1; §0.3 rule 2). There is no `usdGen:clump:levelBase`.

**Capture** (cost B): resolve the centre set (reference lane), build a kd-tree over centre roots in
rest space, assign `clumpId[level][c]` **and** `neighbourClumpId[level][c]` (the second-nearest
centre, which `copy` blends toward) by nearest centre — constrained by `usdGen:mask:region` unless
`crossover` allows a cross — draw the stray coin
`stray[c] = (UsdGenDraw01(clump:seed, curveId[c], kSaltClump+L) < stray:rate) ? stray:amount : 0`, draw the cut coin
the same way, and store rest segment lengths for `preserveLength`. Levels are a small topological
order inside the reference lane when `goalFeedback = 1`.

**Evaluate** (cost A × levels; level barriers are `tbb::parallel_for` fences, never chunk
dependencies):

```
for L in 0..levels-1:
  sizeL  = size  * sizeReduction^L ;  amtL = amount * tightnessReduction^L
  for curve c in chunk:
    C   = centreCurve[clumpId[L][c]]                       # reference buffer, read-only
    for i in 0..cv-1:
      t      = hairT[i]
      target = (method == linearBlend) ? C(t)
                                       : C(t) + frameC(t) * localOffset(P[c][i])   # extrudeAndBlend
      target += offset * N_root(c) + flatten(flatness, frameC(t))
      if copy > 0:                                          # blend toward the neighbouring clump
        target = lerp(target, C_neighbour(t), copy * (1 - copyVariance*UsdGenDraw01(clump:seed,curveId[c],kSaltClump+L)))
      w = blend * amtL * profileLUT[t] * mask(c) * ramp(t) * (1 - stray[c]*strayFalloff(t))
      P[c][i] = lerp(P[c][i], target + volumize * radialDir(c) * (1-t), w)
    if clump:noise:amount: P[c] += noiseVec(centreRoot*corr + hash3(clump:seed,curveId[c],kSaltClump+L)*(1-corr), t*freq) * ramp(t)
    if cutCoin(c):          truncate P[c] to (1 - cut) * arcLength
    if preserveLength:      restoreSegmentLengths(P[c], restLen[c], rootLocked)
```

The four helpers, one line each. `C` is the clump curve, `C_neighbour` the curve of
`neighbourClumpId[L][c]`, and `frameC(t)` its minimal-twist frame `(T, N, B)`:
`flatten(f, F) = -f * dot(P - C(t), F.B) * F.B`;
`radialDir(c) = unit vector in the root tangent plane at angle 2π·UsdGenDraw01(clump:seed,
curveId[c], kSaltClump+7)`; `strayFalloff(t) = pow(t, clump:stray:falloff)`;
`localOffset(P) = frameC(t)^-1 * (P - C(t))`. `blend * mask(c) * ramp(t)` is exactly §5.2's
`w(c,i)`, so `w` here is `w(c,i)` scaled by the level amount and the stray/profile terms.

**Emitted primvars.** `clumpId_<n>` (uniform int) per level, numbered chain-wide (§0.11); a shader
drives per-clump variation from it (Unreal's `Clump ID`), and `UsdGenInstance` (v1, M6) can carry it
into the instancer by naming it in `usdGen:instance:variationPrimvars`, which publishes it as an
`instance`-interpolated primvar (`02-schema.md` §2.11; `06-imaging.md` §4.3). There is no
`usdGen:protoSelect` property: prototype choice is the hashed draw over `usdGen:instance:weights`,
and material variety across cards is **several prototypes with partitioned
`instancerTopology/instanceIndices`**, because Storm has no per-instance material binding
(`06-imaging.md` §4.3).

**Interactions.** `clump:centers` targeting an operator marks that node `UsdGenRole::Reference`;
targeting a `UsdGenMap` reads its value as a clump-id field (XGen's `.xuv` point maps). `clump:amount`
is a **value** edit; `clump:size`, `:density`, `:seed`, `:levels` and the region map are **capture**
edits (§6.4). `cut` truncates within the existing CV count and never removes CVs.

### 2.9 `UsdGenNoise` — frizz (A7 §9.2 S2)

**Purpose.** Multi-octave displacement in the root frame; the last operator in almost every chain.

| Property | Type | Default | Range | Doc |
|---|---|---|---|---|
| `usdGen:noise:magnitude` | `float` | `0.05` | ≥ 0 | maximum CV displacement in stage units (`02-schema.md` §2.7.1 is the normative row, ADR §9.2 R7) |
| `usdGen:noise:magnitude:knots` | `float2[]` | `[(0,0),(1,1)]` | — | root→tip ramp on magnitude |
| `usdGen:noise:frequency` | `float` | `3.0` | > 0 | cycles per stage unit along the curve (`02-schema.md` §2.7.1) |
| `usdGen:noise:correlation` | `float` | `0.5` | 0–1 | how much neighbouring hairs share the field (1 = fully correlated, a wave; 0 = independent) |
| `usdGen:noise:octaves` | `int` | `1` | 1–6 | fBm octaves (`02-schema.md` §2.7.1) |
| `usdGen:noise:lacunarity` | `float` | `2` | > 1 | frequency multiplier per octave |
| `usdGen:noise:gain` | `float` | `0.5` | 0–1 | amplitude multiplier per octave |
| `usdGen:cumulative` | `bool` | `false` | — | accumulate the offset along the curve (Blender frizz) instead of displacing each CV independently. **Flat** (`02-schema.md` §2.7) |
| `usdGen:preserveLength` | `float` | `1` | 0–1 | restore rest segment lengths after the displacement. **Flat, the same property `UsdGenClump` carries** |

**Capture.** The correlation hash per curve and the 257-entry magnitude LUT. **Evaluate:**

```
for curve c: h = (1 - correlation) * hash3(seed, curveId[c], kSaltNoise)
  for i: v = vfbm(rootRest[c]*correlation + h + (0,0,hairT[i]*frequency),
                  octaves, lacunarity, gain)                       # SeExpr's noise (S38)
         P[c][i] += magnitude * magLUT[hairT[i]] * w(c,i) * (cumulative ? accum(v) : v)
  if preserveLength: restoreSegmentLengths(...)
```

SeExpr's noise is the **single** noise implementation for expressions and C++ stylers (S38); `noise`
keeps 0..1 semantics, `snoise` is the signed variant (ADR §6). Calling it here does not violate §0.8:
this is the vectorised C++ `vfbm`, not the SeExpr interpreter. `usdGen:space` resolves to `rest` for
`UsdGenNoise` under the authored default `"auto"` (§0.7), so the field is evaluated at the rest root
and frizz does not swim when the surface animates; authoring `"deformed"` is the deliberate opt-out
and moves the node into the motion tail. There is no `world` token (ADR §9.2 R9).

### 2.10 `UsdGenLength` — cut, scale and cull (A7 §9.2 S6)

| Property | Type | Default | Doc |
|---|---|---|---|
| `usdGen:length:mode` | `uniform token` | `"scale"` | `set` \| `scale` \| `cull` — three tokens, the `02-schema.md` §2.7.1 set (ADR §9.2 R7) |
| `usdGen:length:value` | `float` | `1` | the operand: stage units for `set`, a multiplier for `scale`, unused by `cull` |
| `usdGen:length:random` | `float2` | `(1,1)` | per-curve multiplier drawn in [x,y] |
| `usdGen:length:source` | `rel` | — | a `UsdGenMap` sampled per root, multiplying `length:value` |
| `usdGen:length:method` | `uniform token` | `"scale"` | `scale` (stretch the whole curve) \| `cutExtend` (truncate/extend along the existing shape) |
| `usdGen:rebuild` | `uniform token` | `"keepParam"` | `keepParam` (CVs collapse to the cut point, XGen) \| `reparam` (redistribute CVs over the new length) |
| `usdGen:minRemainingLength` | `float` | `0` | floor on the resulting length |
| `usdGen:cullThreshold` | `float` | `0` | curves shorter than this are removed — the **only** topology effect in this operator |

**Deviation from ADR §6's source, recorded rather than hidden.** `design/proposal-risk.md` §6.1 —
which ADR §6 names as the source of the v1 parameter lists — and `research/A7-prior-art-grooming.md`
§9.2 S6 both give six tokens (`set|add|subtract|multiply|cutAbsolute|cutRelative`). **ADR §9.2 R7
supersedes ADR §6 here** (02 is the registry and 04 may not disagree with it), so this document
adopts 02 §2.7.1's three-token set. Nothing is lost: `add`/`subtract` are `set` with a value the
tool's panel computes from the rest length, and `cutAbsolute`/`cutRelative` are `set`/`scale` with
`usdGen:length:method = "cutExtend"`. `usdGen:cullThreshold` sits alongside `mode = "cull"` because
02 §2.7.1 declares both and §6.2 routes both as topology — `cull` selects the culling kernel,
`cullThreshold` is the length below which a curve is removed, and a non-zero threshold culls under
`set`/`scale` too, which is why `TopologyEffect()` is `CurveCount` unconditionally (§0.7).

Every `UsdGenLength` parameter except `minRemainingLength`, `cullThreshold` and `rebuild` is
namespaced under `usdGen:length:`, so the type carries no flat `usdGen:mode`. The three tokens are
read in one combination each: `length:method` only in the
`set`/`scale` modes, and `rebuild` only when `length:method = "cutExtend"`. Other
combinations are ignored and the stack editor greys the row; `Bind()` still pulls every mapped
locator regardless (§0.3 rule 1).

**Capture.** Rest arc-length tables per curve and the per-curve target lengths; when
`mode == "cull"` or `cullThreshold > 0`, the surviving id set (which is what makes the operator
`CurveCount`). **Evaluate:** with `length:method = "scale"` the kernel is
`P[i] = root + (P[i] - root) * s`, `s = lerp(1, target/current, w(c,i))`; with `cutExtend` it walks
the arc-length table to `s_cut` and then either collapses the trailing CVs onto the cut point
(`rebuild = "keepParam"`, XGen) or redistributes them over the new length (`"reparam"`). Culling is
applied at capture, so a frame change never changes counts.
**Interaction with density scrubbing:** during a drag the buffer is held at the maximum count and
culled curves are *parked* (CVs collapsed to the root, `widths = 0`), so element counts never change
mid-drag; the real count commits on release (S28, gate S-7). Padding arrays is never an option —
padded arrays render fallback red in 26.08 (S28).

### 2.11 `UsdGenWidth` — the strand profile (A7 §9.2 S7)

| Property | Type | Default | Doc |
|---|---|---|---|
| `usdGen:width` | `float` | `0.01` | base strand width in stage units |
| `usdGen:width:knots` | `float2[]` | `[(0,1),(1,1)]` | root→tip multiplier ramp (XGen `Width Ramp`) |
| `usdGen:taper` | `float` | `0` | 0–1 tip narrowing applied after `taperStart` |
| `usdGen:taperStart` | `float` | `0.5` | where the taper begins (0 = root, 1 = tip) |
| `usdGen:rootScale` / `usdGen:tipScale` | `float` | `1` / `1` | Unreal's root/tip radius scales |
| `usdGen:replace` | `bool` | `true` | `true` = set widths, `false` = multiply the upstream widths |

**Capture.** The 257-entry `width:knots` LUT. **Evaluate** — the target profile first, then §0.5's
envelope form for a non-displacement operator:

```
target_i = base * rampLUT[t_i] * lerp(rootScale, tipScale, t_i) * taperTerm(t_i)
w_i      = replace ? lerp(w_in_i, target_i, w(c,i))
                   : w_in_i * lerp(1, target_i, w(c,i))
```

`w(c,i)` is §5.2's envelope (`usdGen:blend` × `curveMask[c]` × `rampLUT[t_i]`), so `blend = 0` or a
zero mask is a bitwise pass-through of the upstream widths in both branches. The result is written to
the vertex `widths` array. **Emitted primvars.** `widths` — **vertex or constant, never varying** (S29). It is one of the
static primvars that must *not* change per deforming frame if the `hairTangent` Variant A fast path
is to hold (ADR §5.4). Storm chooses RIBBON/HALFTUBE by refine level and the presence of user widths
(`pxr/imaging/hdSt/basisCurves.cpp:292-341`), which is why the tile pins `refineLevel = 2`.

### 2.12 `UsdGenSmooth` — along-curve Laplacian (A7 §9.2 S8)

| Property | Type | Default | Doc |
|---|---|---|---|
| `usdGen:mode` | `uniform token` | `"alongCurve"` | `alongCurve` (v1) \| `neighbours` (v3, §4) |
| `usdGen:strength` | `float` | `0.5` | −1…1; negative sharpens |
| `usdGen:iterations` | `int` | `1` | 1–16 |
| `usdGen:lockRoot` | `bool` | `true` | hold CV 0 |
| `usdGen:searchRadius` / `usdGen:numNeighbors` | `float` / `int` | `0` / `4` | reserved for `neighbours` mode; authored now so the v3 upgrade is not a schema change |

**Capture.** Nothing type-specific — only the universal mask array and 257-entry mask ramp LUT of
§5.2, which every operator has. `UsdGenSmooth` declares no ramp of its own in this document or in
`02-schema.md` §2.7 (`strength`, `iterations`, `mode`, `lockRoot`, plus the two v3-reserved rows
`searchRadius` and `numNeighbors`), so the §1.2 capture-cache cell reads "—". **Evaluate:** `iterations` passes of
`P[i] += strength * w(c,i) * (0.5*(P[i-1] + P[i+1]) - P[i])` over interior CVs, root locked. In v1 the
node declares no reference inputs and `mode = "neighbours"` is a compile error naming the prim —
better a hard diagnostic than a silent wrong look (`design/proposal-risk.md` §5's hard-diagnostic
rule).

### 2.13 `UsdGenDirection` / Lift — XGen Tilt (A7 §9.2 S5)

| Property | Type | Default | Doc |
|---|---|---|---|
| `usdGen:direction` | `vector3f` | `(0,1,0)` | target direction in the read-phase surface's space (the `vector3f` half of the §0.2 name collision) |
| `usdGen:amount` | `float` | `0` | 0–1 rotation toward the target direction |
| `usdGen:lift` | `float` | `0` | degrees away from the surface tangent plane (XGen Tilt N) |
| `usdGen:tiltU` / `:tiltV` / `:tiltN` / `:aroundN` | `float` | `0` | degrees, the four XGen Tilt axes expressed directly (`02-schema.md` §2.7); each is a pre-rotation of `direction` in the root frame |
| `usdGen:mode` | `uniform token` | `"rigid"` | `rigid` (rotate the whole curve about the root) \| `perSegment` (accumulate per segment) |
| `usdGen:followSkinContour` | `float` | `0` | 0–1 blend of the target toward `dPdu`/`dPdv` at the root |
| `usdGen:direction:source` | `rel` | — | a `UsdGenMap` supplying a per-root direction (`returnType = "color"` read as a vector) |
| `usdGen:direction:knots` | `float2[]` | `[(0,1),(1,1)]` | along-curve ramp on `amount` (only meaningful in `perSegment`) |

**Capture.** Per-root frames and, for a map-driven direction, the sampled vectors. **Evaluate:**
project the target into the tangent plane at the root, build the rotation about `N_root` (plus the
lift rotation about `N_root × T`), and either apply it rigidly to every CV or accumulate it per
segment weighted by the ramp. XGen's Tilt U / Tilt V / Tilt N / Around N are the same operator: those four axis angles named
directly, with `direction`/`lift` the general form they compose into — one prim type, not four.

### 2.14 `UsdGenScale` — global length multiplier (A7 §9.2 S13)

`float usdGen:scale = 1` (≥ 0, value), `float2[] usdGen:scale:knots = [(0,1),(1,1)]` (value),
`float2 usdGen:scaleRandom = (1,1)` (≥ 0, capture) and `bool usdGen:widthToo = false` (structural —
it changes the published primvar set); all four are `02-schema.md` §2.7.1's rows.
Capture stores the rest arc lengths; evaluate scales each curve about its root by
`lerp(1, scale * scaleLUT[t] * random(c), w(c,i))` and, when `widthToo`, scales `widths` by the same
factor. It is XGen IGS's `xgmModifierScale` and it exists because "make the whole groom 10 % longer
for this shot" must not be a `UsdGenLength` mode edit that a shot-level override then fights with.

### 2.15 `UsdGenResample` — change the CV count (A7 §9.2 S14)

| Property | Type | Default | Doc |
|---|---|---|---|
| `usdGen:cvCount` | `int` | `8` | 2–64 target CVs per curve |
| `usdGen:distribution` | `uniform token` | `"uniform"` | `uniform` (equal arc length) \| `keepParam` (equal parameter). Named `distribution`, not `mode` (`02-schema.md` §2.7) |
| `usdGen:restoreSegmentLengths` | `bool` | `false` | re-impose the source segment lengths after resampling |

All three rows are `02-schema.md` §2.7.1's: `cvCount` (structural/topology), `distribution`
(structural) and `restoreSegmentLengths` (`bool`, `false`, capture — "the source lengths are stored
at capture").

**Capture.** Per-curve cumulative arc-length tables over the *input* CVs (`s[0..n-1]`, `s[0] = 0`,
`s[n-1]` = the curve's length), the output count `cvCount`, and, when `restoreSegmentLengths` is on,
the source segment lengths. **Evaluate** (cost A; no allocation — the output buffer is sized by the
topology publish):

```
for curve c in chunk:
    n = cvCountIn[c]
    for j in 0..cvCount-1:
        u = j / (cvCount - 1)
        t = (distribution == uniform) ? invArcLength(s[c], u * s[c][n-1])   # equal arc length
                                      : u * (n - 1)                         # equal parameter
        P_out[c][j] = catmullRom(P_in[c], t)
    if restoreSegmentLengths: restoreSegmentLengths(P_out[c], sourceSegLen[c], rootLocked)
```

`invArcLength` is a binary search in `s[c]` plus one linear interpolation inside the found segment.
Every **vertex** primvar is resampled with the same `t` (`widths` linearly), except `hairT`, which is
recomputed as `j/(cvCount-1)` because it is by definition the normalised parameter of the *output*
curve; **uniform** and **constant** primvars pass through untouched, so `curveId`, `st`, `hairId` and
every `clumpId_<n>` survive a resample unchanged.

`TopologyEffect() == CvCount`, so `usdGen:enabled` is structural here (§0.4) and a `cvCount` edit is
a full re-capture plus one topology publish; the tile *set* does not change (S27). Sculpt-layer
deltas authored against the pre-resample CV count are stale downstream of a `cvCount` edit — the
badge and "Rebase sculpt" of §2.7 apply. Resample is v1, not v2, because `UsdGenCurveSource` is v1
and imported grooms are ragged (ADR §4.2.2; `design/judge-delivery.md`); the import tool offers
"resample to N" next to the measured E-1r penalty.

### 2.16 `UsdGenInstance` — cards, archives and spheres (S33)

**Purpose.** Replace the description's curve publication with a Hydra `instancer`: one instance per
surviving curve, a prototype set under `<Description>/Prototypes/…`, and per-instance transforms
drawn at capture. It is v1, scheduled **M6** (ADR §9.5 R38, §7). Rows below are `02-schema.md`
§2.11's (R7).

| Property | Type | Default | Doc |
|---|---|---|---|
| `usdGen:primitive` | `uniform token` | `"cards"` | `cards` \| `archives` \| `spheres` — what a prototype is. Spelled `primitive`, not `mode`; the `usdGen:primitive` ADR §9.2 R8 drops is the one on `UsdGenDescription`, not this one |
| `usdGen:prototypes` | `rel` (ordered) | — | prototype prims, re-rooted as namespace children of the synthesized instancer (S33) |
| `usdGen:instance:weights` | `float[]` | `[]` | prototype choice by hashed weight; empty = uniform. Material variety is multiple prototypes — Storm has no per-instance material |
| `usdGen:instance:orient` | `uniform token` | `"surfaceFrame"` | `surfaceFrame` \| `curveTangent` \| `camera` \| `world` |
| `usdGen:instance:scale` / `:scaleRandom` | `float` / `float2` | `1.0` / `(1,1)` | per-instance scale and its draw range |
| `usdGen:instance:twist` / `:twistRandom` | `float` | `0.0` / `0.0` degrees | rotation about the instance's own axis |
| `usdGen:instance:normalOffset` | `float` | `0.0` | push along the surface normal, in stage units |
| `usdGen:instance:width` / `:length` | `float` | `0.02` / `0.09` | card dimensions; ignored for `archives` |
| `usdGen:instance:width:knots` | `float2[]` | `[(0,1),(1,1)]` | card width along the strand |
| `usdGen:instance:variationPrimvars` | `token[]` | `["displayColor"]` | which baked per-curve values are published as `instance`-interpolated primvars |

**Capture.** Prototype bounds, the hashed prototype assignment from `instance:weights`, and every
per-instance random draw (`scaleRandom`, `twistRandom`, salt `kSaltInstance`) — never per frame.
**Evaluate** (deformed space) writes the `instance`-interpolated `hydra:instanceTranslations`,
`hydra:instanceRotations` and `hydra:instanceScales`; the prototype assignment reaches Hydra as
`instancerTopology/instanceIndices` — an `HdIntArrayVectorSchema`, one `VtIntArray` per entry of
`instancerTopology/prototypes` — never as a flat `protoIndices` array, which is not a name this
design uses (`06-imaging.md` §4.3;
`pxr/imaging/hd/instancerTopologySchema.h:125-126`). An interactive card edit dirties
`primvars/hydra:instanceTranslations`, never `instancerTopology` (S33). `usdGen:enabled = false`
here is topology-structural: the instance count changes (`02-schema.md` §6.2; §0.4). The instancer
prim, `instancedBy`, prototype re-rooting and `primOrigin` are `06-imaging.md`'s; this section owns
the parameters and the capture/evaluate split. Gates **T-INST-1/2** (instancer pick round-trip,
prototype rebasing) are its exits.

---

## 3. v2 operators

Every v2 operator is a variation on a kernel shape v1 already has — a per-CV displacement in a frame
— so each is days of work, not weeks (**ASSUMPTION**, `design/proposal-risk.md` §6.3; no gate
measures it). Every v2 operator ships in **M8** (ADR §9.5 R38). The parameter names below are
`02-schema.md` §2.7's flat spellings for the shapes and `02-schema.md` §2.7.2's for the tokens 02
reserves at M1, so that §3 names exactly what C1 freezes at the end of M1. **Every name below is
declared in 02**; this table adds only the kernels.

| Type | Parameters (type, default) | Modes | Capture | Evaluate |
|---|---|---|---|---|
| `UsdGenCurl` | `radius` float 0.02 + `radius:knots` ramp; `frequency` float 2 (turns per stage unit); `phase` float 0; `phaseRandom` float 0; `taper` bool true; `clockwise` bool true | `usdGen:axisMode = curveTangent \| guide` | minimal-twist (RMF) frames along the smoothed curve; `rigExec::rigExecMath`'s `RigExecSampleCurveRMF` is reusable (`research/A1-usdrig-graph.md` §7) | `P[i] += r(t) * (cos(2πf·s_i + φ) N_i + sin(...) B_i)`, `s` = arc length |
| `UsdGenBend` | `angle` float 0 (deg) + `angle:knots`; `angleRandom` float2 (1,1); `axis` vector3f (1,0,0) | `usdGen:axisMode = rootDirection \| uniform \| attribute` | per-curve angle draws, ramp LUT | cumulative per-segment rotation of CVs `i ≥ k` about `P[k]` by `angle*(ramp(t_k) − ramp(t_{k-1}))` |
| `UsdGenStraighten` | `tangentStraightness` float 0; `normalStraightness` float 0 | — | rest chord vectors | `P[i] = lerp(P[i], root + chord*s_i, k)` decomposed per plane |
| `UsdGenDisplace` | `displace:amount` float 0; `displace:base` float 0.5; `displace:scale` float 1; `displace:offset` float 0; `rel displace:map` | `usdGen:mode = height \| vector` (default `height`) | map sampled per root (CPU, capture, S37); root normals | `P[i] += N_root * (sample − base) * scale + offset` |
| `UsdGenWave` | `frequencyU` / `frequencyN` float 1; `amplitudeU` / `amplitudeN` float 0 | — | root frames, arc-length tables | `P[i] += A_T sin(2πf_T s_i) T + A_N sin(2πf_N s_i) N` |
| `UsdGenPart` | `part:curves` rel; `part:radius` float 0.05; `part:strength` float 1 | — | kd-tree over parting-curve samples; per-curve side id and crossing weight | multiplies the guide/clump weight the consumers already store; emits `partId` (uniform int) |
| `UsdGenExprOp` | `expr:source` string; `rel expr:maps`; `expr:returnType` token `displacement\|width\|color` (default `displacement`) | `usdGen:mode = cv \| curve` (default `cv`) | **the entire result**: the expression is evaluated once per capture into a per-CV or per-curve array | a memcpy/add of the captured array |

`UsdGenCurl` and `UsdGenBend` name their switch `usdGen:axisMode`, not `usdGen:mode` (§1.3): a
`usdGen:mode` is a term of the structural digest `d(n)` (§0.3 rule 2, ADR §4.2.1) and neither branch
changes what these two capture; `02-schema.md` §2.7.2 says the same and classes `usdGen:axisMode`
`structural` all the same, because it selects a kernel branch. The four tokens that were once
proposals — `UsdGenDisplace`'s `usdGen:mode` (`height \| vector`), `UsdGenExprOp`'s `usdGen:mode`
(`cv \| curve`) and `usdGen:expr:returnType` (`displacement \| width \| color`), and `UsdGenPart`'s
`part:curves` / `part:radius` (`float`, `0.05`) / `part:strength` (`float`, `1.0`) — are now
declared in `02-schema.md` §2.7.2, so §3 reserves nothing 02 does not. `UsdGenPart` **emits**
`partId` (uniform `int`, unprefixed, ADR §9.3 R24).

`UsdGenExprOp` is **capture-time only** and the schema doc says so. At 1.6 M CVs × 100 ns a per-frame
SeExpr styler is ≈160 ms (**DERIVED from ledger row `EV-067`**; S37, `design/judge-evidence.md` §2.2
item 5); the rule of §0.8 — no operator calls an expression or samples a texture inside `Evaluate` —
is checked by the debug wrapper.

**Hooks reserved in v1 so v2 is not a schema break** (C1 freezes `usdGen:` names at the end of M1):
`usdGen:searchRadius`/`usdGen:numNeighbors` on `UsdGenSmooth` (§2.12); `usdGen:mask:region` on the
mask block, consumed by `GuideInterpolate` and `Clump` from day one and *produced* by `UsdGenPart`
later (ADR §2.3); `usdGen:clump:curl:amplitude`/`:frequency` on `UsdGenClump`, whose kernel is the
same RMF code `UsdGenCurl` will use; `usdGen:preserveShape`, `usdGen:preserveShape:iterations` and
`usdGen:rbfSamples` on `UsdGenDeform` (v2, §2.5); `usdGen:mask:ramp:spline` and the other `<p>:spline`
ramp forms of ADR §9.2 R11, which the adapter factory can publish without a schema change.

---

## 4. v3 operators

§1.4 lists what each waits for; the parameter sets are recorded here so the reserved names are known
before C1 freezes.

* **`UsdGenCollide` / Shrinkwrap** — `target = skin|colliders|sdf`, `offset`, `pushRange`,
  `pushAmount`, `iterations`, `resolveType = flexible|stiff`, `lockRoots`. The only operator that
  breaks the "no per-CV surface query" rule (cost C); without a collider BVH rebuilt per surface
  epoch and a memory budget for it, a per-CV closest-point query over an animated mesh is unbounded.
* **`UsdGenWind` / `UsdGenForce`** — `direction`, `constStrength`, `gustStrength`, `shearStrength`,
  `shearFreq`, `stiffness` + ramp, `seed`, `loopFrames`. **Time-dependent**, so every downstream node
  is in the motion tail and the per-offset cache is mandatory. Both declare `deformedSpace`; if one
  ever needs a genuinely world-space class it comes as a new `usdGen:space` token added under
  `02-schema.md` §8.2 with fallback `auto`, never as a fourth v1 token (ADR §9.2 R9). It waits on a
  time integrator with per-curve state that survives a scrub, which the engine has in neither v1
  nor v2.
* **`UsdGenSmooth` `mode = "neighbours"`** — needs a per-frame spatial grid over deformed roots and
  the **two-pass gather/scatter node kind** of ADR §4.2 item 5: never a chunk fan-in, and never
  "widen chunks" (4096-curve chunks are a measured 5× cliff, 1.83 ms at 512 against 9.40 ms at 4096
  at 20 threads; the 8-thread figure is UNMEASURED, gate E-2. MEASURED, ledger row `EV-009`,
  `research/G-data-plane-engine-prototype-benchmark.md` §4; S23).
* **`UsdGenBraid`** — `strands = 3`, `radius`, `frequency`, `flare`. Three sub-strands per input
  curve at 0/120/240° phase: a topology multiplier inside a *styler*, which `UsdGenTopoFx` can
  declare but the chunk partitioner cannot yet handle mid-chain without a re-partition.
* **`UsdGenSimSource`** — `cache` (rel), `timeOffset`, `timeScale`, `loop`. `UsdGenCurveSource` plus
  a time transform and the ragged path at scale; it waits on E-1r and on a decision about caching
  whole frames of imported curves against `USDGEN_MEMORY_BUDGET_MB`.

---

## 5. Masks

### 5.1 The block

`UsdGenMaskAPI` is auto-applied to `UsdGenOperator` (ADR §2.1), so every operator has it. The schema
listing is in `02-schema.md`'s `UsdGenMaskAPI` section; the properties and their semantics are:

| Property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:mask:source` | `rel` | — | at most one `UsdGenMap`; composition is a `UsdGenCombineMap` |
| `usdGen:mask:amount` | `float` | `1.0` | scale on the whole resolved mask |
| `usdGen:mask:invert` | `bool` | `false` | `x → 1 − x` after the range remap |
| `usdGen:mask:range` | `float2` | `(0,1)` | remap of the source value before use |
| `usdGen:mask:combine` | `uniform token` | `"multiply"` | `multiply`\|`add`\|`subtract`\|`max`\|`min`\|`average`\|`replace` — how the **random** term folds into the map term (`02-schema.md` §2.13) |
| `usdGen:mask:random` | `float` | `0.0` | per-curve `rand` multiplier amount (XGen `rand()` masks) |
| `usdGen:mask:randomSeed` | `uniform int` | `0` | seed for that draw |
| `usdGen:mask:ramp:knots` | `float2[]` | `[(0,1),(1,1)]` | along-curve ramp, `(t, value)` sorted by `t` |
| `usdGen:mask:ramp:interpolation` | `uniform token` | `"catmullRom"` | `linear`\|`catmullRom`\|`bspline`\|`constant` (ADR §9.2 R11; `02-schema.md` §2.13, §2.17) |
| `usdGen:mask:ramp:spline` | `float` | — | **v2.** The whole-`TsSpline` transport of ADR §9.2 R11, authored with `.spline`; never flagged time-varying |
| `usdGen:mask:rangeMin` / `:rangeMax` / `:effectPosition` / `:falloff` | `float` | `0`/`1`/`0.5`/`0.5` | Houdini's curve-mask shortcut (§5.4) |
| `usdGen:mask:influenceWidth` | `float` | `0.5` | 0–1 width of the shortcut's band, the fifth Houdini parameter (`research/A7-prior-art-grooming.md` §7); declared in `02-schema.md` §2.13, which delegates the shortcut formula to §5.4 |
| `usdGen:mask:rangeMode` | `uniform token` | `"normalized"` | `normalized` \| `absoluteLength` — is `t` normalised arc length or stage units. Spelled `rangeMode`, not `range:mode` |
| `usdGen:mask:noise:amount` / `:frequency` / `:gain` / `:bias` / `:seed` | `float`/`int` | `0`/`1`/`0.5`/`0.5`/`0` | Houdini's Noise Mask over the rest root position |
| `usdGen:mask:region` | `rel` | — | region/parting map (§5.3) |

### 5.2 Evaluation order and arithmetic

Resolved **once per capture** into one `VtFloatArray` per operator (one float per curve) plus one
257-entry LUT over `hairT` (`design/proposal-performance.md` §7.4; `design/proposal-artist.md` §3.3).
**`02-schema.md` §2.13 is canonical for the mask block's names, tokens and arithmetic** (ADR §9.2
R16), and delegates the `combine` truth table and the `bias`/`gain`/`biasGain` definitions to this
section. The block below is 02 §2.13's, quoted term for term:

```
remap(s, x, y) = clamp( (s - x) / max(y - x, 1e-6), 0, 1 )      # the guard: x == y must not divide by 0

s      = mask:source ? sample(mask:source, rootUV(c)) : 1.0
s      = remap( s, mask:range.x, mask:range.y )
s      = mask:invert ? 1 - s : s
r      = lerp( 1.0, UsdGenDraw01(mask:randomSeed, curveId(c), kSaltMaskRandom), mask:random )
n      = 1 + mask:noise:amount * (biasGain(fbm(restRoot(c) * mask:noise:frequency,
                                                mask:noise:seed),
                                            mask:noise:bias, mask:noise:gain) * 2 - 1)
curveMask(c) = clamp( combine( mask:combine, mask:amount * s, r )
                      * n * region(c) * lockedCurveSuppression(c), 0, 1 )
rampLUT[i]   = ramp(i / 256)                                   i = 0..256
t            = hairT(i) * 256 ;  j = min(int(t), 255) ;  a = t - j
rampWeight   = (1 - a) * rampLUT[j] + a * rampLUT[j + 1]        # lerp; the 257th entry keeps j+1 in range
w(c, i)      = usdGen:blend * curveMask(c) * rampWeight
# Elsewhere in this document `rampLUT[t_i]` is shorthand for this interpolated read, never a nearest sample.
```

`UsdGenDraw01(seed, curveId, salt)` is §0.6's call-site form of `02-schema.md` §2.19.1's per-curve
draw; `sample(mask:source, rootUV(c))` selects the channel the map prim's `usdGen:map:channel`
names (`02-schema.md` §2.12); `region(c)` is 1 unless `usdGen:mask:region` rejects the curve (§5.3);
`lockedCurveSuppression(c)` is the per-curve suppression mask `UsdGenSculptLayer` publishes from
`usdGen:sculpt:lockedCurves` (§2.7), which is how one mechanism implements XGen's Freeze brush for
every downstream styler at once. `w(c, i)` — the `usdGen:blend` envelope of §0.5 applied to the
mask — is the weight every kernel in §2 writes as `w(c,i)`; `w(c, i) == 0` is an early-out and the
operator writes the input value bitwise.

The helpers 02 delegates to this section, written out so two implementations agree bit for bit:

```
combine(t, m, r) =  t == "multiply" ? m * r
                  : t == "add"      ? clamp(m + r, 0, 1)
                  : t == "subtract" ? clamp(m - r, 0, 1)
                  : t == "max"      ? max(m, r)
                  : t == "min"      ? min(m, r)
                  : t == "average"  ? 0.5 * (m + r)
                  :                   r                              # "replace"

bias(x, b)       = pow(x, log(b) / log(0.5))
gain(x, g)       = x < 0.5 ? 0.5 * bias(2*x, 1 - g)
                           : 1 - 0.5 * bias(2 - 2*x, 1 - g)
biasGain(x,b,g)  = gain(bias(x, b), g)                               # Houdini Noise Mask convention
```

Four properties of the order matter. (1) The **source is remapped before it is inverted**, so
`range = (0.2, 0.8)` and `invert` compose the way an artist expects. (2) `combine` selects how the
**random** term `r` folds into the map term — which is what the token's schema `doc` says — not how
the source folds into `amount`; the noise term `n` always multiplies, so a `curveMask` of 0 from the
map stays 0 whatever the noise does. (3) `mask:amount = 0` guarantees a pass-through **only** under
`combine = "multiply"` (the default) or `mask:random = 0`: with `add`, `max`, `average` or `replace`
and `mask:random > 0` the mask is `r ≠ 0` and the operator still does work. The panel says so next to
the token, and §8 test 5 asserts the identity in exactly those terms. (4) The whole thing is a
**capture** cost: SeExpr evaluates at 13–117 ns and a Ptex bilinear lookup costs 23 ns (both MEASURED, ledger
rows `EV-067` and `EV-070`, `research/A8-seexpr-ptex-libs.md` §1.6, §2.8). At 1 M roots that is
0.013–0.117 s single-threaded and **≈ 20 ms on the measured 8-thread aggregate of 50 M evals/s**
(ledger row `EV-069`) — **DERIVED from `EV-067`/`EV-069`**, UNMEASURED end to end until gates
**L-3/L-4/L-5** (T0–T1, M4 — the map, Ptex and expression gates; `09-performance-and-benchmarks.md`
§5 is the registry, R40). Gate E-4 covers only the nanoflann kNN capture that dominates
`UsdGenGuideInterpolate` and `UsdGenClump`. `02-schema.md` §2.13 prints the same ≈ 20 ms for the same
derivation, and the two must stay equal.
Per frame it would be several times the entire evaluate budget (`design/proposal-performance.md`
§7.4).

The `w(c,i) == 0` early-out is what makes the mask identity tests of §8 exact.

### 5.3 Region maps and parting

`rel usdGen:mask:region` targets a `UsdGenMap` whose value is read as a **region id**, not a weight:
an integer channel, or a colour quantised by `UsdGenImageMap`'s `map:channel = "rgb"` into a stable
id via `hash32(quantise(rgb, 1/64))`. At capture every root and every guide/clump centre gets a
region id from the same map. `GuideInterpolate` and `Clump` then reject a candidate whose region id
differs, except for the fraction allowed by `clumpCrossover` / `clump:crossover`. This is XGen's
Region Map, whose stated purpose is that "clumps cannot cross the boundaries defined by the Region
Map" (`research/A7-prior-art-grooming.md` §1.4). `design/judge-artist.md` flagged that the ADR's
`usdGen:mask:region` had no defined semantics; this paragraph is that definition. `UsdGenPart` (v2)
produces the same per-root ids from a curve set instead of a map, so the consumers do not change.

### 5.4 The Houdini shortcut, the guide-proximity source, and expression masks

When `usdGen:mask:ramp:knots` has fewer than two authored knots, the shortcut applies instead:
`rangeMin`/`rangeMax` bound the effect along `t` (in normalised arc length, or in stage units when
`rangeMode = "absoluteLength"`), `effectPosition` places the peak inside that band,
`influenceWidth` sets the width of the band the falloff shapes, and `falloff` shapes the shoulders.
The formula, so two implementations agree:

```
p        = lerp(rangeMin, rangeMax, effectPosition)
halfW    = 0.5 * influenceWidth * (rangeMax - rangeMin)
x        = clamp(1 - abs(t - p) / max(halfW, 1e-6), 0, 1)
shortcut(t) = pow(x, max(falloff, 1e-6) * 4)
```

It exists because it is one slider drag rather than a ramp edit
(`research/A7-prior-art-grooming.md` §7 lists all five parameters).

`UsdGenGuideProximityMap` (`rel usdGen:guides`, `float proximity:radius`, `float proximity:decay`)
yields the normalised distance from each root to the nearest guide root — the mask for "fade this
styler out near hand-placed guides". `UsdGenExprMap` supplies the XGen SeExpr variable set
(`$u $v $id $faceId $P $N $dPdu $dPdv $Pref $Nref $t $frame $cLength`) with `map()`, `ptex()` and the
added `rand()`, which is **not** a SeExpr2 builtin (MEASURED absent,
`research/A8-seexpr-ptex-libs.md` §1.7: "`Function rand has no definition`"; ADR §6 adds it). One
thread-safe `VarBlock` and one `PtexFilter` per TBB worker (MEASURED, ledger rows `EV-069` and
`EV-070`).

### 5.5 The mask visualisation hook

The tool needs to answer "why is this operator not doing anything here?" (`design/judge-artist.md`
unresolved #10). The engine already computes `curveMask[]` and `rampLUT[]` per node, so the hook is a
**read of existing data**: no extra evaluation, no schema, no new per-frame work.

The tool turns it on with `int UsdGenImaging_SetMaskVisualisation(const char *opPath)` — an empty
string clears it — which ADR §9.4 R31 makes a **minimum entry point of contract C4**.
`08-tools.md` §1.4 is the single source for its signature and already carries it verbatim —
`int UsdGenImaging_SetMaskVisualisation(const char *opPath);  /* "" clears (5.5) */` — so this
section states only the behaviour. While the mode is on, the session publishes the
named operator's `curveMask` as a uniform greyscale `displayColor` on the tiles instead of the shaded
colour: one `primvars/displayColor` dirty when the mode is switched on or the target changes, and
`primvars/displayColor/primvarValue` per change afterwards, never co-dirtied with `points` (S30). The
usdview plugin binds the call to hotkey `M` on the selected operator (`08-tools.md` §5.5).

The "show driving guides" overlay is the same shape: `GuideInterpolate` already emits
`guideIndex`/`guideWeight`, and the overlay colours the **guides** — writing their `displayColor` in
the `__usdGenRender/guides/<setName>` publication — not the hairs (`08-tools.md` §5.5; ADR §6).

---

## 6. Worked chains

### 6.1 Chain A — the request's example (R4)

`Scatter(random, 400) → GuideInterpolate(guides=Guides/scalp) → Clump(size 1.6) → Scatter(random,
6000) → GuideInterpolate(guides=Ops/clump_big) → Clump(size 0.4) → Noise`; stage-by-stage effects are
in §0.11. The measured five-styler baseline at 100 k × 8 CV is 1.02 ms on 8 threads (ledger row
`EV-001`/`EV-008`) and 1.72–1.91 ms on 20
(`prototypes/data-plane-benchmark/results_main.txt`). Scaled by 1.8× for 180 k roots and by 7/5 for
the two extra nodes, 180 k × 8 CV should cost ≈ 2.6 ms full on 8 threads (≈ 4.5 ms on 20), ≈ 0.4 ms
per terminal edit and ≈ 0.05 ms sparse — all three **DERIVED from `EV-001`/`EV-002`/`EV-004`/`EV-008`** and
UNMEASURED at this size until gate E-1 runs the real chain.

### 6.2 Chain B — frozen curves that deform, plus hand work (R3, R9)

`CurveSource(curves=</Char/Hair/imported>, useRest=true) → Deform(rigidFrame) → SculptLayer →
Width`. `CurveSource` and `Width` are rest-space; `Deform` opens the deformed tail, so per frame only
`Deform → SculptLayer → Width` re-runs — and `SculptLayer`/`Width` are `A`-class memcpy-scale kernels.
A comb stroke commits into `SculptLayer`'s delta arrays inside one `Sdf.ChangeBlock` on release; per
move it is a live override with only the touched leaves dirtied (S40). Freezing belongs on **chain
A**, not here: a `UsdGenFreeze` inserted between `clump_fine` and `frizz`
(`frozen:mode = "frozen"`) snapshots the expensive generator/clump head, `frizz` keeps running on the
snapshot, and everything above stays authored and greyed — XGen Groom Bake exactly
(`research/A7-prior-art-grooming.md` §1.4). Unfreezing is one token edit, never a prim removal
(S41).

### 6.3 Chain C — short fur driven by maps, at two densities

`Scatter(random, density=8000, mask:source=PaintMap densityPaint) → Grow(segments=6,
length:source=ExprMap lengthVar) → Direction(lift=25) → Clump(levels=2, goalFeedback=1) →
Width(taper=0.8) → Length(length:mode=scale, length:method=cutExtend, mask:source=ImageMap trim)`.
The Description carries
`usdGen:densityScale = 0.25` for the viewport and leaves `usdGen:renderDensityScale = 1`, because
the two contexts are **exclusive** (§0.6, ADR §9.2 R13): `keepFraction = 0.25` interactive and `1.0`
at render, with `usdGen:density = 8000` the full render count. Authoring `renderDensityScale = 4`
here would be the old multiplicative reading; under R13 it renders at `clamp(4) = 1` with one
`TF_WARN` and buys nothing. Both contexts run one monotone predicate on the same salted stable id,
so the viewport set is a subset of the render set and every clump id, root UV and sculpt delta is
identical between them. Repainting `densityPaint` is a recapture of the whole chain (the root set moves); editing
`Width.taper` is a value edit on one node.

### 6.4 Dirty class of a typical edit

| Edit | Class | What runs | Notice |
|---|---|---|---|
| `usdGen:label` | none | nothing | none |
| `usdGen:blend`, `usdGen:enabled` on a styler | **toggle** | re-evaluate that node's chunks + the tail | `primvars/points/primvarValue` + `extent/*` per dirty tile |
| `clump:amount`, `noise:magnitude`, `width`, `scale` | **value** | same as a toggle | same |
| a brush move | **value, sparse** | terminal node only, `k` touched chunks (0.035–0.044 ms at 1 % of chunks on 20 threads, MEASURED, ledger row `EV-002`; the 8-thread figure is UNMEASURED, gate E-2) | the touched tiles only |
| `seed`, `density`, `clump:size/density/seed`, `guide` radii and angles, a mask source or ramp, a map file, `mask:region` | **capture** | recapture that node's cone, then re-evaluate | as above; plus a topology publish if counts changed |
| `clump:levels` | **capture + topology publish** | recapture that node's cone | as above, **plus** a topology publish because a `clumpId_<n>` primvar appears or disappears (a primvar appearing is dirtied as `primvars/<name>` once, ADR §5.2) |
| a surface `points` dirty | **value, deformed tail** | tail chunks whose roots are on that surface (surface-major chunk order makes it a range) | per dirty tile |
| a surface *topology* resync, a `GeomSubset` edit | **capture + topology** | full recapture from the first generator | topology publish; tile set unchanged |
| `cvCount` on `UsdGenResample`, `cullThreshold` crossing 0 | **capture + topology** | as above | as above |
| `usdGen:enabled` on a generator, `UsdGenResample`, `UsdGenFreeze`, `UsdGenInstance`, or a culling `UsdGenLength` | **capture + topology** | as above | as above |
| `usdGen:mode` on `Scatter`/`Deform`/`Smooth`/`Direction`, or a concept token of §1.2 note ¹ (`clump:method`, `blendMethod`, `distribution`, `idSource`, `frozen:mode`) | **structural** | recompile that node's sub-graph, then recapture its cone — the kernel branch and the capture set change together (`02-schema.md` §6.1; §0.3 rule 2). `length:mode` is the one exception, routed as topology (`02-schema.md` §6.2) | as below |
| `cvCount` or `blendMethod` on `UsdGenGuideInterpolate` | **structural** | recompile, then recapture — the guide resample arity is compiled into the kernel (`02-schema.md` §6.1) | as below |
| add/remove an operator, rewire `usdGen:input`, edit `usdGen:space`, `usdGen:readPhase` or `usdGen:algorithmVersion` | **structural** | sub-graph recompile (only the changed node and its descendants, ≤ 0.2 ms for a 200-node groom per gate E-6) then recapture | as above |

Commit triggers are ADR §4.3 as amended by **§9.4 R32**: an operator-parameter, map or
surface-topology dirty commits **synchronously at the end of the `_PrimsDirtied` batch that carried
it**, with or without an application driver — exactly one cook per edit batch, counted by gate SI-3.
A tool's release sequence is: close the live override without republishing → author the edit in one
`Sdf.ChangeBlock` → request a repaint → `ApplyPendingUpdates` delivers the dirty → the index cooks
and publishes. Trigger (a) (`Commit()` / `SetTime`) is for time changes and for explicit commits
after live-override changes only; the app never calls `Commit()` for a stage edit, and no
`Usd.Notice` listener ordering rule exists in the design (R32).

---

## 7. Look preservation and kernel versioning

1. **`uniform int usdGen:algorithmVersion` per operator type, schema fallback `0`.** `0` = track
   the newest kernel; any positive value pins that kernel revision (ADR §9.2 R17). The tool, every
   freeze and every bake author the explicit current version, so look preservation is guaranteed for
   every tool-authored asset; a hand-written asset that omits the attribute follows the newest
   kernel, and that is documented, not accidental. The registry maps `(primType, algorithmVersion)`
   to a kernel entry point; an unknown (newer) version is a hard diagnostic naming the prim and the
   operator publishes pass-through — a silent partial groom is worse than an absent one
   (`design/proposal-risk.md` §3.9).
2. **What counts as a look change and forces a bump:** any change to a hash function or a salt
   (§0.6), to the noise implementation, to the order of a reduction, to a default that is not
   behaviour-neutral, or to a kernel's arithmetic. Fixing a *crash* or a NaN does not bump.
3. **Adding a parameter never bumps.** A new property must have a default that reproduces the old
   behaviour exactly — `usdGen:clump:method = "linearBlend"` today with `"extrudeAndBlend"` opt-in is
   the model (`design/proposal-artist.md` §3.10). Removing a property is never done in place: it keeps its schema entry
   marked `hidden = true` for one release and the evaluator ignores it.
4. **`algorithmVersion` is in the structural digest** (ADR §4.2.1 lists it in `d(n)`), so a bump
   recompiles that node and recaptures its cone — correct, because a new kernel may capture
   different things.
5. **Frozen data outranks kernels.** A `UsdGenFreeze` or `UsdGenCurveSource` reproduces the shot
   regardless of kernel version, and `primvars:usdGen:frozenEpoch`'s `usdgen1:` prefix makes a freeze
   written by a newer plugin *stale*, never silently wrong (ADR §2.3, S42).
6. **Golden images are per `(primType, algorithmVersion)`.** The T2 golden set is keyed by that pair,
   so an old version keeps its golden image forever and a bump adds one rather than replacing one.

---

## 8. Testing

Tiers are `T0` engine (pure `UsdGenGraph` over synthetic buffers, no Hydra, no USD), `T1` headless
scene index over the real `UsdImagingCreateSceneIndices` chain, `T2` Storm through the EGL
device-platform harness on this host, `T3` `testusdview` under Xvfb (CPU numbers only), `T4`
workstation protocol, release criteria only (`design/proposal-risk.md` §9.1; ADR §9.1 R2;
`research/ENVIRONMENT.md` CORRECTIONS).

**Per operator, every operator, no exceptions.** Tests 1, 2, 6, 7 and 8 are identical for all 16 v1
operator types. Tests 3, 4 and 5 assert an *identity*, and an identity means different things for a generator
than for a styler, so each names its class — asserting a bitwise pass-through on a generator would be
a test that cannot pass against §0.4 and §0.5.

| # | Tier | Test | Applies to | Assertion |
|---|---|---|---|---|
| 1 | T0 | golden buffer | all | the operator over a fixed 1 000-curve synthetic input matches a checked-in `.npy` buffer to 1 ULP; one golden per `(primType, algorithmVersion)` (§7 item 6) |
| 2 | T0 | determinism across thread counts | all | identical bytes at 1, 2, 8 and `USDGEN_THREAD_LIMIT` threads, built `-ffp-contract=off` — gate **E-8** |
| 3 | T0 | blend identity | stylers and deformers | `usdGen:blend = 0` ⇒ output buffer **bitwise** equals the input; `blend = 1` ⇒ equals the unenveloped kernel. On a generator the test instead asserts that `blend != 1` emits **exactly one** compile warning naming the prim, and that the result is unchanged (§0.5) |
| 4 | T0 | enabled identity | topology-preserving types | `usdGen:enabled = false` ⇒ bitwise pass-through **and** an unchanged capture-cache pointer (proves §0.4's "no recapture"). For generators, `UsdGenResample`, `UsdGenFreeze`, `UsdGenInstance` and a culling `UsdGenLength` (`length:mode = "cull"` or `cullThreshold > 0`), the test instead asserts that the downstream curve/CV/instance counts change and **exactly one** topology publish is emitted |
| 5 | T0 | mask identity and arithmetic | operators whose mask weights a displacement | `mask:amount = 0` **with `mask:combine = "multiply"` or `mask:random = 0`** ⇒ bitwise pass-through (§5.2 property 3 — under `add`/`max`/`average`/`replace` with `mask:random > 0` the mask is `r ≠ 0` and the operator is *not* a pass-through); a map that is 1 everywhere ⇒ identical to no mask; `mask:invert` twice ⇒ identity; and, for each of the seven `combine` tokens, `curveMask` matches §5.2's truth table on a 5-value fixture. For `UsdGenScatter`, where the mask multiplies density (§2.1), the identity is: `mask:amount = 0` ⇒ **zero roots**, and a map that is 1 everywhere ⇒ the same root id set as no mask |
| 6 | T0 | salt independence | all | two operators of **different** types with the same `usdGen:seed` over the same id set produce uncorrelated draws (Pearson \|r\| < 0.05 over 10⁴ curves) — the per-use salt rule of §0.6. Two prims of the **same** type with the same seed draw the *same* numbers by construction; the test asserts that too, so the property is pinned rather than assumed away |
| 7 | T0 | capture/evaluate split | all | `Evaluate` allocates nothing (an allocation hook fails the test) and touches no `UsdStage` symbol (link-time: `usdGen` links no `usd`/`usdImaging`/`hd`, ADR §4.2.3) |
| 8 | T1 | value **and** notice | all | an edit to each `ValueParameters()`/`TopologyParameters()` token changes the published value **and** emits exactly the expected dirty locator set — gate **SI-2** |

One more T0 test is global rather than per operator: `testUsdGenHash` asserts that `hairId` stays
uniform within 2 % after decimation to `densityScale = 0.25`, which is what the separate
`kSaltDensity` draw of §0.6 buys (`05-static-curves-and-deformation.md` §2.4).

Three more T1 tests per type: every value of the type's concept token exercised — `usdGen:mode`, or
its named switch per §1.2 note ¹ — with `Bind()` pulling every mapped locator in every mode (S14,
§0.3); every `usdGen:*` property appearing in the `usdGen` container and dirtying (gate **SI-7**);
and, for generators, `points.size() == Σ curveVertexCounts` (gate **SI-1**) plus a stable-id test (a
density change adds and removes ids without renumbering survivors). `UsdGenClump` and
`UsdGenGuideInterpolate` add a reference-lane assertion that a hair chunk reads no other hair chunk
(an address-range check in the debug wrapper).

**Gates this document is accountable for.** **E-1** 5-op chain 100 k × 8 CV in the private arena at
8 threads **≤ 1.5 ms** (ADR §9.3 R27; MEASURED baseline 1.02 ms at 8 threads, ledger rows `EV-001`
and `EV-008`, `research/G-data-plane-engine-prototype-benchmark.md` §3.3; 1.72–1.91 ms at 20), and
the gate that settles the 0.4–0.6 ms tail budget of §0.7. `09-performance-and-benchmarks.md` §5 is
the single gate registry (R40) and states the same threshold; where this list and 09 ever disagree,
09 stands. **E-1r** ragged path ≤ 2× uniform (`UsdGenCurveSource`,
`UsdGenResample`); **E-2** 1 % sparse ≤ 0.10 ms; **E-3** terminal-parameter edit ≤ 0.6 ms; **E-4**
`GuideInterpolate` capture over 100 k roots / 4 k guides at 8 threads ≤ 25 ms and linear in roots —
run as **M0 pre-work `PW-1`** because the whole operator schedule assumes it, and **binding at M3**
against the real `UsdGenGuideInterpolate` capture (ADR §9.5 R40; `09-performance-and-benchmarks.md`
§5); **E-6** append a node to a 200-node groom ≤ 0.2 ms, 1 node rebuilt; **E-8** determinism;
**SI-1/2/3/7/8**;
**S-7** density scrub (the parked-CV path of §2.10) ≤ 1.5× a points-only frame; **T-INST-1/2** for
`UsdGenInstance` (§2.16). Milestone exits: M1 owns `UsdGenScatter` (`random`), `UsdGenGrow`,
`UsdGenNoise`, `UsdGenLength`, `UsdGenWidth`; M2 owns `UsdGenCurveSource`, `UsdGenDeform`,
`UsdGenFreeze`, `UsdGenSculptLayer`; M3 owns `UsdGenGuideInterpolate`, `UsdGenClump`,
`UsdGenSmooth`, `UsdGenResample`, `UsdGenDirection` and `Scatter(atGuides)`; M4 owns `UsdGenScale`,
the maps and `UsdGenPtexMap`; M5 owns `Scatter(points)` with the Place brush; M6 owns
`UsdGenInstance`; M8 owns `Scatter(uniform)` and the rest of the v2 catalogue (ADR §9.5 R38).

---

## 9. Out of scope

Not in this document: the schema files and `.usda` layout (`02-schema.md`); `UsdGenOp`'s C++
signatures, the chunk arena, the dirty router and the motion cache (`03-execution-engine.md`); the C3
curve contract, freeze tiers and undo (`05-static-curves-and-deformation.md`); the tile contract,
invalidation locators, the instancer prim and prototype re-rooting (`06-imaging.md`); map prim
parameters, the SeExpr function set, Ptex face mapping and the three material terminals
(`07-look-maps-expressions.md`); the brushes, panels and the C ABI signatures, including
`UsdGenImaging_SetMaskVisualisation` (`08-tools.md` §1.4); gate thresholds and benchmark protocols
(`09-performance-and-benchmarks.md` §5); the env-var registry
(`10-build-dependencies-testing.md` §3.5);
the measured rows themselves (`appendix-A-evidence-ledger.md`).
Not in the product at all in v1–v3: a third-party operator ABI (ADR §3), operators that author to the
stage (R1 — nothing is authored by the evaluator), per-operator chunk sizing or "widen chunks"
(measured 5× cliff), and any operator that samples a texture or evaluates an expression per frame.

## 10. Sources

**Design.** `design/adr-v1.md` §1 (S23/S27, S11, S29), §2.1–2.3 (hierarchy, layout, contested
properties), §3 (C1–C5), §4.1–4.3 (I3 reference lane, I4 capture/evaluate, digests, commit
triggers), §5.3–5.4, §6 (catalogue split, operator rules), §7 (milestones, gates), §8 (document
map), and the **§9 addendum**: R1/R2 (I-numbering, `PW-n`, `EV-nnn`, test tiers and gate families),
R6–R18 (namespaces, 02 normative, fold-ins, `space` and the unconditional v1 `preceding` → `final`
alias, `clump:level`, ramps, the pinned hash and 64-bit `curveId`, the density pair, `enabled`, the
mask block, `algorithmVersion` fallback `0`, stale freezes), R21/R24/R27 (tile arithmetic, the
emitted extra planes `clumpId_<n>` / `guideIndex` / `guideWeight`, 8-thread baselines),
R31/R32/R35 (C ABI, commit triggers, env vars), R38/R40/R42/R45 (tiers, gate registry, number tags,
cross-references).
`design/brief-v1.md` §2 (S1, S4, S5, S8, S10–S14, S17–S18, S21–S32, S37, S38, S40–S42, S45), §3 D4.
`design/proposal-risk.md` §3.2, §3.4–3.9 (base block, mask, ramps, maps, freeze/sculpt, versioning),
§4.2–4.3, §6.1–6.3 (parameter lists, emitted primvars, tier split), §9.1 (test tiers).
`design/proposal-artist.md` §3.1, §3.3–3.5, §3.7, §4.4–4.10 (op interface, dirty hops,
non-structural `enabled`, reference lane, `UsdGenNodeStats`), §6.1–6.4 (catalogue, parity, the rules
every operator obeys), §8.3.
`design/proposal-performance.md` §5.2, §5.6–5.8, §5.11 (kernel authoring rules), §7.1–7.4 (catalogue
columns, mask arithmetic), §11.1–11.2.
`design/judge-artist.md` (§D1/D4, unresolved #3 `enabled`, #4 region maps, #10 mask debugging),
`design/judge-evidence.md` §2.1–2.2 (S14 pull rule, adapter coverage, unapplied `UsdGenMaskAPI`,
`ExprOp` capture-time), `design/judge-delivery.md` (§D4, `Resample` to v1, `algorithmVersion`).

**Research and evidence.** `research/A7-prior-art-grooming.md` §1.1–1.5, §2.1–2.3, §3.1–3.5, §6, §7,
§8, §9.1–9.4 (the canonical operator set this catalogue implements);
`research/A8-seexpr-ptex-libs.md` §1.6, §1.7 (`rand()` verified absent), §2.8;
`research/A1-usdrig-graph.md` §5–§7; `research/G-data-plane-engine-prototype-benchmark.md` §3.3,
§4–§6; `research/G-freeze-bake-undo-and-frozen-reentry.md` §2.1–2.3, §4.2;
`research/G-stage-free-parameter-and-time-transport.md` §1–2;
`research/G-storm-throughput-and-prim-granularity.md` §1.6, §2; `research/ENVIRONMENT.md`
(CORRECTIONS block). `appendix-A-evidence-ledger.md` rows `EV-001`, `EV-002`, `EV-004`, `EV-008`,
`EV-009`, `EV-014`–`EV-016`, `EV-052`, `EV-067`, `EV-069`, `EV-070`, `EV-081`, `EV-082`.
`prototypes/data-plane-benchmark/results_main.txt` (20-thread runs only).

**OpenUSD 26.08.** `pxr/usd/usdGeom/schema.usda:1670-1732` (curve vertex-count rules; `:1678` is the
pinned-cubic rule `curveVertexCounts[i] - 2 >= 0`); `pxr/usd/usdGeom/primvar.h:322-331`
(`GetElementSize`/`SetElementSize`, the `guideIndex`/`guideWeight` encoding);
`pxr/usd/sdf/types.h:367` (`int3` is `GfVec3i`, not an elementSize-3 array);
`pxr/imaging/hd/basisCurves.cpp:29-37` (built-in primvars are exactly `points`, `normals`,
`widths`); `pxr/imaging/hd/instancerTopologySchema.h:123-126`
(`HdPathArrayDataSourceHandle GetPrototypes()` and
`HdIntArrayVectorSchema GetInstanceIndices()` — the `UsdGenInstance` publication of §2.16, never a
flat `protoIndices`); `pxr/imaging/hdSt/basisCurves.cpp:292-341` (refine level and user widths select
RIBBON/HALFTUBE) and `:1356-1387` (`minScreenSpaceWidths` survives primvar filtering);
`pxr/base/work/loops.h:165,207` (`WorkParallelForN`, used by every `Capture`);
`pxr/exec/esfUsd/stageData.cpp:361` (the invalid-prim predicate on resync,
`if (!UsdPrimDefaultPredicate(resyncedPrim))`, S41/S46; the brief and
`research/G-freeze-bake-undo-and-frozen-reentry.md` §1.4 write `:360`, and
`appendix-A-evidence-ledger.md` §3.10 records `:361` as the verified line in this checkout).
