# Static curves, deformation, freezing and sculpting

Date: 2026-09-04. Status: plan v1 (draft, pending review).

This document specifies the half of usdGen that does not generate curves: how curves that already
exist — a freeze, an imported `UsdGeomBasisCurves`, an Alembic cache converted to USD, a simulation
cache, a guide set, the output of a sculpt brush — enter the evaluator, how they are bound to a rest
surface, how they are transported to the deformed surface every frame and every motion sample, how
an operator's output is frozen and unfrozen, and how hand-authored deltas survive on top of all of
it. It is the specification for milestone **M2** ("Deform + freeze"), which freezes contract **C3**,
and it carries requirement **R3** end to end.

Reads with: `01-architecture.md` (chain placement), `02-schema.md` (§5 is the
normative home of contract C3 and of every property table quoted here), `03-execution-engine.md`
(chunk arena, capture/evaluate, digests, the motion tail), `04-operators.md` (the styler catalogue
and the rules every operator obeys), `06-imaging.md` (publication, tiles, invalidation, extents),
`08-tools.md` (the freeze/commit flows and the brush loop that authors sculpt deltas),
`09-performance-and-benchmarks.md` (§0.2 is the only frame ledger, §5 the only gate registry —
ADR §9 R40, R41), `10-build-dependencies-testing.md` (§3.5 the env-var registry, §5.6 the CTest
names), `11-roadmap.md` (§2.3 M2's exit criteria, §5 the stop-condition register),
`12-risks-decisions-open-questions.md` (RK-08's `Tf.Error.Mark` correction, RK-09's SI-10 request),
`appendix-A-evidence-ledger.md` (§2 is the `EV-nnn` row every measured number here cites).

---

## 0. Requirements, measured basis and vocabulary

### 0.1 The requirements this document carries

| Req | Text (brief §1) | Where it is answered here |
|---|---|---|
| **R3** | "Static (frozen, cached, imported) curves load quickly and deform with a deforming surface." | §1 (the contract they arrive in), §2 (loading), §3 (rest binding), §4 (deformation), §8 (the cost of both halves) |
| **R4** | "…applicable to generated, rigged or simulated curves." | §1.4 and §5.7: a frozen prim and a generated prim are the same kind of styler input, so the whole styler catalogue applies to imported and simulated curves without a second code path |
| **R5** | "reads deformed surfaces from the scene index, whoever produced them" | §4.2: the private pruning wrapper (S3) plus the `readPhase` ladder |
| **R9** | "freeze an operator's output, then comb/groom manually … commit to the stage via the tool" | §5 (freeze), §6 (sculpt), §5.6 (undo and unfreeze) |
| R8 | "precise dirty propagation on interactive prim edits" | §4.7.1 (the normative locator → dirty-class table) and §5.8: which locators a frozen prim emits and what each one invalidates |

### 0.2 The measured basis

Every number below was produced on this host during the verification round; each carries its
`EV-nnn` row id from `appendix-A-evidence-ledger.md` §2, which is the citation handle for every
MEASURED number (ADR §9 R42). The app-side
freeze costs (`_resetGUI`, `SubtreeSnapshot`, `SetActive` vs `RemovePrim`, the resync blast radius)
are tabulated where they are used, in §5.5, §5.6, §5.9 and §8.1, and are not repeated here.

| Fact | Value | Source |
|---|---|---|
| USD → Hydra cost of one freeze (author + `ApplyPendingUpdates` + `GetPrim` + full data pull) | **0.55 / 0.59 / 0.59 ms** at 10 k / 100 k / 1 M curves — flat in curve count; second pull 0.01 ms | MEASURED, **EV-045** (`research/G-freeze-bake-undo-and-frozen-reentry.md` §4.1) |
| Reopen a sidecar `.usdc` and read `points` (800 k CVs) | 0.4–0.8 ms; the same data as `.usda` costs 532 ms | MEASURED, **EV-047** (same report §3) |
| `primvars:rest` authored from the same `VtArray` object as `points` | **+26 bytes** in crate; a distinct buffer costs +9 600 046 bytes | MEASURED, **EV-048** (same report §3) |
| A frozen `BasisCurves` read from a filtering scene index's **input** | `points`, `rest`, `skinprim`, `skinprimuv`, `usdGen:curveId` all present with correct interpolations, no `UsdStage` access | MEASURED (qualitative, no ledger row), same report §2.2 |
| Relationships and custom (non-primvar) attributes on a curve prim | never reach Hydra, never invalidate — on typed *and* untyped prims | MEASURED (qualitative, no ledger row), same report §2.3 |
| One styler pass over 800 k CVs | 0.16–0.55 ms single-thread (0.158 ms `soa_strip` **EV-016**, 0.545 ms `soa_streamed` **EV-015**); 5-node chain **1.02 ms at 8 threads** (**EV-008**), 1.72–1.91 ms at 20 (**EV-001**) | MEASURED, `research/G-data-plane-engine-prototype-benchmark.md` §6, §3.3, §4 |
| One memory-bound pass over 1.6 M points (lerp, velocity extrapolation, copy) | 0.99 / 0.97 / 0.49 ms (**EV-072** / **EV-071** / **EV-074**); 19.2 MB per motion sample (**EV-075**) | MEASURED, `research/G-motion-blur-sampling-strategy.md` §5 |
| UsdSkel-skinned surfaces | `primvars/points` is **blocked**; the values live behind `extComputationPrimvars/points` | MEASURED (source read, no ledger row), `research/A2-usdrig-imaging.md` §8, `pxr/usdImaging/usdSkelImaging/dataSourceResolvedPointsBasedPrim.cpp:291,506` |

### 0.3 Vocabulary

Milestones are **M0–M8** (ADR §7). Gate families are ADR §9 R2's: `E-` engine, `SI-` scene index,
`S-` Storm, `L-` look / render context, `T-` tools, `T-INST-` instancing, `R-` render time, `B-`
build; plus `E-1r` (the ragged path). Gate ids are always hyphenated, and
`09-performance-and-benchmarks.md` §5 is the single registry of id → metric → tier → milestone
(ADR §9 R40). The link rule asserted by §2.2's header comment is gate **B-1** (T0, M0, ADR §9 R37).

**Test tiers are T0–T4**, a different axis from the tool gates `T-1…T-5`: T0 = `usdGen`/`usdGenMath`
units with no stage and no Hydra (possible because the engine input is `UsdGenGraphDesc`,
ADR §4.2.3); T1 = headless scene index over the real `UsdImagingCreateSceneIndices` chain; T2 = Storm
through the EGL harness; T3 = `testusdview`; T4 = workstation protocol (S45), which supplies
**release criteria only, never a milestone exit** (ADR §7). "Tier T1" is the harness; "gate T-1" is
the brush-move gate. The brief's S42 freeze **landing-tier** labels T1/T2/T3 are **retired**
(ADR §9 R1); landing places are named only by the `usdGen:frozen:tier` token — `session`, `sublayer`,
`payload` (§5.4).

Engine invariants are **I1–I8** (ADR §9 R1, renaming ADR §4.1's P1–P8); **P0/P1/P2** are the motion
profiles of S32 and nothing else (§4.5).

Numbers carry one of four tags (ADR §9 R42): **MEASURED** (with its `EV-nnn` row in
`appendix-A-evidence-ledger.md` §2, and the report section behind it), **DERIVED from `EV-nnn`**
(scaled or interpolated — never MEASURED), **UNMEASURED** (with the gate that will measure it) or
**ASSUMPTION**. A handful of MEASURED facts here are qualitative — a primvar is present, a
relationship never arrives — and carry no ledger row; they are marked as such.

---

## 1. Contract C3 — the curve contract, in full

### 1.1 The contract

C3 is the shape every curve prim must have to be usable as a usdGen input. It is a plain
`UsdGeomBasisCurves`: **no new prim type is required for the data**, only for the operator that
points at it (MEASURED, `research/G-freeze-bake-undo-and-frozen-reentry.md` §2.1). The normative
copy lives in `02-schema.md` §5; it is repeated here in full because every section below depends on
it field by field.

```usda
def BasisCurves "fur_v003" (
    prepend apiSchemas = ["UsdGenCurveAPI"]
)
{
    uniform token type  = "cubic"                    # S29; never "bezier"
    uniform token basis = "bspline"                  # catmullRom accepted; never centripetalCatmullRom
    uniform token wrap  = "pinned"                   # survives to Hydra: basisCurvesAdapter.cpp:367-368
                                                     # (the Hydra 1.0 adapter); confirmed end to end in
                                                     # the terminal SI dump, freeze report §2.1
    int[]      curveVertexCounts = [8, 8, 8, ...]    # Sum == points.size()  (S28, exact size)
    point3f[]  points            = [...]

    float[]      primvars:widths            ( interpolation = "vertex" )    # or "constant"; never "varying"
    point3f[]    primvars:rest              ( interpolation = "vertex" )    # +26 B while it shares points
    int[]        primvars:skinprim          ( interpolation = "uniform" )   # face index into the PARENT mesh
    texCoord2f[] primvars:skinprimuv        ( interpolation = "uniform" )   # NOT named "st"
    uint64[]     primvars:usdGen:curveId    ( interpolation = "uniform" )   # 64-bit stable id (ADR §9 R12),
                                                                            # unique per description
    string       primvars:usdGen:frozenEpoch ( interpolation = "constant" ) # "usdgen1:sha1:…"
    token        primvars:usdGen:role       ( interpolation = "constant" )  # "hair" | "guide"
    matrix4d[]   primvars:usdGen:rootFrame  ( interpolation = "uniform" )   # OPTIONAL, rest frame per curve
    uniform token purpose = "guide"                                         # guide sets only
}
```

### 1.2 Why each channel is shaped that way

* **The staleness digest is a `constant` primvar, never `customData`, never a custom attribute.**
  MEASURED: a `string` attribute named `usdGen:frozenEpoch` does not appear in the scene index and
  emits no notice on create or on value change, while `primvars:usdGen:frozenEpoch` appears with
  `interp=constant` and dirties as `primvars/usdGen:frozenEpoch`
  (`research/G-freeze-bake-undo-and-frozen-reentry.md` §2.3). `customData` is worse:
  `Usd.Prim.GetCustomDataByKey("usdGen:frozenEpoch")` returns `None` because the key path splits on
  `:` (same section).
* **Namespaced primvars survive verbatim.** `primvars:usdGen:curveId` reaches the scene index under
  the name `usdGen:curveId` and dirties as `primvars/usdGen:curveId` (MEASURED, same §2.3). No
  flattening, no renaming.
* **`primvars:skinprimuv` must not be called `st`.** It picks up `role = textureCoordinate` from its
  `TexCoord2f` type (harmless), but `st` is the root-UV primvar usdGen publishes on its own tiles
  (S29) and the two must not collide (MEASURED, same §2.1).
* **`primvars:rest` is authored at every freeze** because it is free: crate deduplicates identical
  value buffers, so handing `points` and `primvars:rest` the *same* `VtArray` object costs 26 bytes
  (MEASURED, **EV-048**). It only costs a second 9.6 MB per 800 k CVs once the two differ.
* **`widths` is `vertex` or `constant`, never `varying`** — a `varying` width costs a 6.48 ms CPU
  expansion per 100 k curves (MEASURED, **EV-033**,
  `research/G-storm-throughput-and-prim-granularity.md` §2 "Topology strategy" and Key facts, S29). It is
  authored as `primvars:widths`, which wins over the schema `widths` attribute
  (`pxr/usd/usdGeom/curves.h:155-168`; the adapter falls back `primvars:widths` → inherited →
  `widths`).
* **No `normals`.** Storm switches curves to the ORIENTED patch layout when `normals` are present
  (S29); on its **own tiles** usdGen's look derives the tangent frame from the patch layout itself
  (ADR §5.4 variant A). A C3 prim usdGen overlays rather than owns takes variant B and a published
  `hairTangent` instead (§5.7).
* **`primvars:usdGen:role`** (`"hair"` or `"guide"`) is the marker that `UsdGenCurveAPI` applies. It
  tells the loader whether the prim is a reference-lane input (guides, §7) or a hair set.

### 1.3 Rules every consumer of C3 obeys

1. **Test for a value, never for the primvar's presence.** `velocities`, `accelerations` and
   `normals` are *always* listed by the gprim data source, with size 0, even when unauthored, and a
   declared-but-unauthored `primvars:usdGen:rootFrame` appears with no interpolation and no value
   (MEASURED, `research/G-freeze-bake-undo-and-frozen-reentry.md` §2.1). Any walk over
   `HdPrimvarsSchema::GetPrimvarNames()` skips primvars whose `GetPrimvarValue()` is empty or absent.
2. **`points.size() == Σ curveVertexCounts`, always.** A longer `points` array without
   `curveIndices` is discarded wholesale and replaced by the fallback (1,0,0) with a `TF_WARN`
   (`pxr/imaging/hdSt/basisCurvesComputations.h:212-234`, MEASURED in
   `research/G-storm-throughput-and-prim-granularity.md`). Gate SI-1 asserts it on every published
   tile; the loader asserts it on every C3 input.
3. **Never read the contract through a `UsdStage`.** All of it is reachable from
   `_GetInputSceneIndex()->GetPrim(path)` inside the hair scene index (MEASURED, §2.2 of the freeze
   report), which is what S8 requires and what makes `usdrecord`/hdPrman correctness independent of
   the usdview C API.

### 1.4 The four producers, and how each satisfies C3

| Producer | Enters through | `rest` | `skinprim` / `skinprimuv` | `curveId` | `frozenEpoch` | Notes |
|---|---|---|---|---|---|---|
| **Guides** (`UsdGenGuideSet` children) | the reference lane (§7) | authored by the guide tool; equals `points` at creation | authored at plant time by `UsdGenImaging_ClosestSurfacePoint` | authored, unique within the set | optional | `role = "guide"`, `purpose = "guide"` |
| **Freezes** (`UsdGenFreeze`) | `usdGen:frozen:curves` (§5) | always authored, 26 B — a freeze may only cap a `restSpace` node (§5.5), so `rest` always shares the `points` array | always authored, copied from the buffer | always authored | **required** | written by the tool; `usdGen:frozen:tier` = `session` \| `sublayer` \| `payload` (§5.4) |
| **Imported USD `BasisCurves`** | `UsdGenCurveSource` (§2) | authored if the exporter wrote it; else `usdGen:useRest=false` | often absent → computed at capture (§3.4) | often absent → assigned (§2.4) | absent → never stale | the common Alembic-via-USD case |
| **Simulation caches** (time-sampled `points`, usually ragged) | `UsdGenCurveSource` with `usdGen:useRest = false` | not meaningful (already deformed) | copied from the pre-sim groom when present | must be authored or the sim must preserve order | absent | classified `deformedSpace`; §2.6 |

A sculpt layer is **not** a fifth producer: it produces no C3 prim at all. Its deltas live on
`UsdGenSculptLayer` (§6), keyed by `curveId` and matched against `usdGen:sculpt:epoch`, on top of
whichever of the four producers is upstream; the brush edits `points` on a live frozen prim and
re-freezes only on commit. C3 has four producers exactly as ADR §3 and `02-schema.md` §5 define it.

The single most useful result of the freeze research is that these are **not four code paths**. A
frozen prim, an imported prim and a generated buffer are the same kind of input to a styler
(MEASURED, `research/G-freeze-bake-undo-and-frozen-reentry.md` §2.2), so "freeze an operator's
output and comb it" (R9) is a *configuration* of the chain, not a mechanism bolted onto it.

### 1.5 What C3 deliberately does not carry

* **No baked motion samples.** 24 point time samples cost 24× the static size — 230 402 598 B at
  100 k × 8 CV — because crate stores float arrays raw (MEASURED, **EV-046**). Motion is
  `velocities` (2×, 19 202 035 B, same row) or the retained per-offset cache (§4.5), never a baked
  sample set.
* **No `.usda`.** 40 317 359 B, 368 ms to write (MEASURED, **EV-046**), 532 ms to reopen
  (**EV-047**), against 9 601 971 B / 10.6 ms / 0.4–0.8 ms for `.usdc` (the same two rows).
* **No relationships and no custom attributes** carrying evaluator state: they are invisible to
  Hydra (§1.2).
* **No per-CV ids.** Identity is per curve; CV identity is the index within the curve, which is why
  a `UsdGenResample` downstream of a sculpt layer invalidates that layer's `cvOffsets` (§6.5).

### 1.6 When C3 freezes

C3 freezes at the **end of M2** (ADR §3). Everything in §1.1 is then an installed, versioned
contract; the `usdgen1:` prefix on `frozenEpoch` is what makes a later major version *stale* rather
than silently wrong (ADR §2.3 row "Versioning"; `02-schema.md` §2.19 and §8.5).

One term of C3 changed after the research reports were written, and it is the only one: the curve id
is **64-bit** (`uint64[] primvars:usdGen:curveId`, ADR §9 R12), not the `int[]` that S42 declared and
that `research/G-freeze-bake-undo-and-frozen-reentry.md` §2.1's probe dump shows. `02-schema.md` §5
already declares `uint64[] primvars:usdGen:curveId`, and everything downstream of the id —
`usdGen:sculpt:curveIds`, `usdGen:sculpt:lockedCurves`, the loader's id plane, the hash — is
`uint64` here for the same reason.

---

## 2. `UsdGenCurveSource` — loading curves that already exist

### 2.1 The prim

`UsdGenCurveSource` derives from `UsdGenGenerator` (ADR §2.1): it is a generator because it may
change curve and CV counts, and it has no `usdGen:input`.

| Property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:curves` | `rel` | — | exactly one target, a C3 `BasisCurves` (or one `UsdGenGuideSet`'s child scope) |
| `usdGen:useRest` | `bool` | `true` | `true` = the loaded `points` are a rest pose and `primvars:rest` (or `points` at `Default()`) is the rest buffer; `false` = the points are already deformed |
| `usdGen:lane` | `uniform token` | `"hair"` | `hair` \| `reference` — `reference` puts the set in the un-chunked reference lane (I3, §7.2). Named `lane`, not `role`, so it cannot be confused with `primvars:usdGen:role` on the data (ADR §2.1) |
| `usdGen:staleAction` | `uniform token` | `"warn"` | `warn` \| `ignore` \| `block` — what to do when `usdGen:expectEpoch` and the prim's `primvars:usdGen:frozenEpoch` disagree |
| `usdGen:expectEpoch` | `uniform string` | `""` | empty = never stale |
| `usdGen:idSource` | `uniform token` | `"primvar"` | `primvar` \| `index` — where `curveId` comes from (§2.4) |
| `usdGen:resampleTo` | `uniform int` | `0` | `0` = keep the source's CV counts (ragged if they differ); `> 0` = resample every curve to that CV count at capture |
| `usdGen:rebind` | `uniform token` | `"onError"` | `never` \| `onError` \| `always` — recompute `skinprim`/`skinprimuv` from the rest surface (§3.5) |

Inherited from `UsdGenOperator`: `usdGen:enabled`, `usdGen:seed`, `usdGen:space`,
`usdGen:readPhase`, `usdGen:blend`, `usdGen:label`, and the `UsdGenMaskAPI` block.

`02-schema.md` §2.6 is the normative property registry (ADR §9 R7) and declares all eight rows; the
table above restates them and may not disagree with it. Five of the eight — `usdGen:lane
(token hair|reference, hair)`, `usdGen:staleAction (token warn|ignore|block, warn)`,
`usdGen:expectEpoch (string)`, `usdGen:resampleTo (int, 0 = keep)`,
`usdGen:rebind (token never|onError|always, onError)` — reached 02 as **ADR §9 R8** fold-ins. Their
provenance, for anyone chasing it: `staleAction` and `expectEpoch` come from
`design/proposal-performance.md` §4.6; `usdGen:lane` renames that proposal's
`uniform token usdGen:role = "curves"` so it cannot be read as `primvars:usdGen:role` on the data;
`resampleTo` is the "resample to N" offer of ADR §4.2.2 and `rebind` is §3.5 below.

### 2.2 The load path

`UsdGenCurveSource` is **a source node whose buffer is loaded from the scene index instead of
computed** (S24). Loading happens in `Capture()`, keyed by the node's capture epoch, and never in
`Evaluate()`:

```cpp
// usdGen/curveLoader.h — no usd, no usdImaging, no hd (ADR §4.2.3): the loader consumes the
// pure-value UsdGenGraphDesc that usdGenImaging built from the data sources.
struct UsdGenSourceArrays {
    VtIntArray   curveVertexCounts;
    VtVec3fArray points, rest;
    VtFloatArray widths;
    VtIntArray   skinPrim;
    VtUInt64Array curveId;          // 64-bit ids, ADR §9 R12 — the spelling 03 §2.2 uses
    VtVec2fArray skinPrimUv;
    VtMatrix4dArray rootFrame;      // may be empty
    std::string  frozenEpoch;       // constant string primvar, "usdgen1:sha1:..."
    TfToken      curveRole;         // primvars:usdGen:role: hair | guide (the C3 marker)
};
class UsdGenCurveLoader {
public:
    /// Re-lays a C3 source into the SoA chunk arena. Returns false and fills `diag`
    /// on any hard error in the ladder of §2.3.
    bool Load(const UsdGenSourceArrays &in, const UsdGenLoadParams &p,
              UsdGenCurveBuffer *out, UsdGenDiagnostics *diag);
};
```

`UsdGenSourceArrays` is the **loader's** argument; `UsdGenCurveSetDesc`
(`03-execution-engine.md` §2.2) is the **graph's** entry for the same prim, and the two are related
by construction, not by field identity — the description adds `path`, `guideBlend` and
`curveGeneration`, which are graph-level. Where a field appears in both it has one spelling and one
type: `VtUInt64Array curveId` and `TfToken curveRole`. Contract C3 freezes the array subset only
(end of M2, ADR §3).

`usdGenImaging` fills `UsdGenSourceArrays` with one `GetPrim` on the hair scene index's **input**
and one `GetValue(0)` per primvar; the arrays are `VtArray`s, so this is a refcount bump, not a copy
(MEASURED: the whole pull is 0.19–0.20 ms at 10 k, 100 k and 1 M curves, and the second pull is
0.01 ms — **EV-090**, the pull term of **EV-045**, freeze report §4.1). `Load` then writes the planar
SoA arrays `px/py/pz`, the per-curve
AoS arrays and the chunk descriptors (`03-execution-engine.md` §1). That transpose is one pass over
the points and is the only per-CV work in the load; it is folded into the E-1r measurement.

### 2.3 The validation ladder

Diagnostics are **hard by default** (risk §5): a description that cannot be evaluated publishes
nothing and emits one message naming the prim, rather than publishing something wrong.

| # | Condition | Severity | Behaviour |
|---|---|---|---|
| 1 | `usdGen:curves` has zero or more than one target, or the target is absent from the scene index | error | the description publishes no tiles; one `TF_WARN` naming both paths |
| 2 | target's prim type is not `basisCurves` | error | as above |
| 3 | `type != "cubic"`, or `basis` not in {`bspline`, `catmullRom`}, or `wrap` = `periodic` | error, naming the offending token | `type = "linear"` is *not* silently promoted; the import tool's "Convert to cubic (resample to N)" action authors a new C3 prim instead |
| 3b | `wrap = "nonperiodic"` | warning, once | accepted and republished as `pinned` (contract C2). The CVs are unchanged; the curve now reaches its first and last CV, so the root sits on the scalp — state it in the warning, because it is a visible change. **Row 3b runs before row 5**, and row 5 is evaluated against the *promoted* wrap |
| 4 | `points.size() != Σ curveVertexCounts` | error | S28 (exact-size arrays); the same assertion gate SI-1 makes on output. A longer array is discarded wholesale for the fallback (1,0,0) (`pxr/imaging/hdSt/basisCurvesComputations.h:212-234`) |
| 5 | any `curveVertexCounts[i] < 2` | error | Row 3b runs first, so **every** prim reaching this row is `pinned` and the check is exactly `curveVertexCounts[i] >= 2` (`pxr/usd/usdGeom/basisCurves.h:120`, `cubic\|pinned` needs `(count - 2) >= 0`). The `cubic\|nonperiodic` rule `(count - 4) % vstep == 0` (`basisCurves.h:118`) is therefore never evaluated. usdGen rejects at the boundary because Storm does not: `pxr/imaging/hdSt/basisCurvesComputations.cpp:312-313` silently skips any curve with `count < 2` |
| 6 | `primvars:widths` absent or size 0 | warning, once | width falls back to the Description's default width; the artist sees it in the stack profiler column |
| 7 | `usdGen:useRest = true` and `primvars:rest` has no value | warning | `points` sampled at `UsdTimeCode::Default()` becomes the rest buffer (S12) |
| 8 | `primvars:skinprim` / `skinprimuv` absent, or `usdGen:rebind = "always"` | info | bindings are computed at capture from the rest surface (§3.4); the cost is reported by `UsdGenNodeStats` |
| 9 | `usdGen:idSource = "primvar"` and `primvars:usdGen:curveId` absent | warning, once | ids fall back to `index` (§2.4) |
| 10 | duplicate ids within the description | error | detected by one sort during capture; duplicates would make sculpt deltas ambiguous |
| 11 | `usdGen:expectEpoch` non-empty and unequal to the prim's `primvars:usdGen:frozenEpoch` | per `usdGen:staleAction` | `warn` = evaluate anyway and badge the node; `ignore` = evaluate silently; `block` = publish nothing |

### 2.4 Identity

Sculpt deltas, clump ids, `hairId` and the decimation predicate all key on the curve id, so identity
is the most load-bearing thing the loader produces.

* Authored `primvars:usdGen:curveId` is used verbatim, and it is the artist's contract: re-exporting
  the cache with different ids moves every sculpt delta.
* `usdGen:idSource = "index"` (or `"primvar"` with the primvar absent) gives
  `curveId = index in the source array` — stable for any producer that preserves curve order, which
  is what an Alembic-via-USD import gives. The loader warns once and the import panel offers
  **"Bake curve ids"**, which authors `primvars:usdGen:curveId` back onto the source prim in one
  `Sdf.ChangeBlock` so the ids stop depending on array order.
* Ids are **`uint64`** (ADR §9 R12, amending S42) and are **not** `hairId`. Scatter mints them as
  `UsdGenHash64(seed, faceIndex, k, kSaltScatter)`; `hairId` is a uniform **float** in `[0,1)`
  published on the tiles, `= UsdGenHash32(curveId, 0) / 2^32` (ADR §9 R12; ADR §1, S29 amendment).
* **The decimation predicate draws a *different* number from the same id**, and both draws are
  pinned. `UsdGenHash32(key, salt)` is the high 32 bits of the SplitMix64 finalizer over
  `key ^ (uint64(salt) * 0x9E3779B97F4A7C15)`; salts are per-use compile-time constants
  (`kSaltScatter`, `kSaltDensity`, `kSaltNoise`, …). Decimation keeps a curve iff
  `UsdGenHash32(curveId, kSaltDensity) < keepFraction · 2^32` with `kSaltDensity ≠ 0`, while
  `hairId` uses salt `0` (ADR §9 R12, R13). Changing the hash or any salt bumps
  `usdGen:schemaVersion`. The two draws must differ for a correctness reason: with one draw, at
  `densityScale = 0.25` every surviving hair would have `hairId ∈ [0, 0.25)` and the per-curve hue
  and value jitter in the glslfx would be biased to a quarter of its range in the viewport and not
  at render. T0 `testUsdGenHash` asserts that the surviving set's `hairId` distribution at
  `keepFraction = 0.25` stays uniform within 2 %. `04-operators.md` §0.6 carries the executable
  spelling of exactly this hash and of `kSaltDensity`, and `02-schema.md` §2.19.1 is its normative
  twin. Nothing else about the predicate changes: it is a pure function of the stable id, so
  the smaller set is always a subset of the larger.

### 2.5 Ragged curves, and the "resample to N" offer

Uniform-CV chunks are the fast path; **ragged chunks are supported in v1** (ADR §4.2.2), because
`UsdGenCurveSource` is v1 and imports and sim caches are ragged by nature. A ragged chunk sets
`UsdGenChunkDesc::cvCount = 0` and uses the per-curve `cvOffsets` prefix array
(`03-execution-engine.md` §1); every kernel gets the CV base from `cvOffsets[c]` instead of
`c * cvCount`, so the inner loop loses one strength-reduced multiply and gains one dependent load
per curve.

* The ragged penalty is **UNMEASURED**. Gate **E-1r** measures it: the E-1 chain (5 operators,
  100 k × 8 CV) re-run with a ragged buffer of the same total CV count, and the threshold is
  **≤ 2× the uniform run** (ADR §7). E-1r runs in M2, at tier T0.
* The import panel offers **"Resample to N"** (`usdGen:resampleTo`, backed by the v1
  `UsdGenResample` styler, ADR §4.2.2). It shows the *measured* E-1r ratio next to the checkbox once
  E-1r has run; until then the panel says "ragged cost not yet measured" rather than a made-up
  number.
* Chunking stays curve-aligned in both cases (I2). A ragged chunk holds 512 curves whatever their CV
  counts, so a chunk's CV span varies; the arena allocates per-chunk CV ranges from the prefix sum at
  capture. Chunk sizes of 128–1024 curves are the measured sweet spot and 4096 is a 5× cliff
  (MEASURED, **EV-009**, `research/G-data-plane-engine-prototype-benchmark.md` §4) — that is a curve-count
  statement, so it is unaffected by raggedness.

### 2.6 Time-sampled sources

A simulation cache authored as time-sampled `points` on a C3 prim is loaded through the same path,
with three consequences that must be stated to the artist:

1. **The node is classified `deformedSpace`.** Its output changes every frame, and the rest/tail
   split of `03-execution-engine.md` §1.6 opens the tail `T` at the **first** node in topological
   order whose resolved space is `deformed` (I4), so the source *and everything below it* is in the
   tail. A sim-cache chain therefore costs `k · tail` per motion sample, where a generated chain
   costs `head + k · tail` with a much shorter tail. The stack profiler shows the split.
2. **`usdGen:useRest` must be `false`.** The points are already in the deformed pose; treating them
   as rest and then running `UsdGenDeform` would deform twice. The compiler emits a warning naming
   both prims when a `UsdGenDeform` is downstream of a `UsdGenCurveSource` with `useRest = false`,
   and the stack editor badges the deformer.
3. **Ids and topology must not animate.** `primvars:usdGen:curveId` and `curveVertexCounts` are read
   at `UsdTimeCode::Default()`. Any `points` sample whose size disagrees with `Σ curveVertexCounts`
   is rejected with an error naming the time (row 4 of §2.3, applied per sample), so a cache that
   adds or removes curves over the shutter fails loudly instead of rendering fallback red. Sim
   caches with animating curve counts are a v3 concern (see §10).

The points themselves are pulled through the sampled-data-source protocol
(`pxr/imaging/hd/dataSource.h:213-228`): `GetValue(shutterOffset)` at the session's current frame in
P0/P1, and once per retained offset in P2 (§4.5).

### 2.7 Load cost

| Path | Cost | Status |
|---|---|---|
| Scene-index pull of a C3 prim (topology + every primvar), 10 k / 100 k / 1 M curves | 0.19 / 0.19 / 0.20 ms; second pull 0.01 ms | MEASURED, **EV-090** (the pull term of **EV-045**, freeze report §4.1) |
| Reopening a `.usdc` sidecar and reading `points` (800 k CVs) | 0.4–0.8 ms | MEASURED, **EV-047** |
| SoA transpose + chunk descriptors at 800 k CVs | one memory-bound pass over 9.6 MB in and 9.6 MB out, ≈0.25 ms | **DERIVED from EV-074** (0.49 ms per 19.2 MB `VtArray` pass, `research/G-motion-blur-sampling-strategy.md` §5; that measurement moved twice the bytes). Gate E-1r reports it separately |
| Binding computation when `skinprim` is absent, 100 k roots | kd-tree build + kNN + patch solve | Production C3 benchmark (4096 faces, 8-way arena): cold 8.599 ms triangles / 15.708 ms bilinear quads; warm 6.992 / 15.440 ms. See `15-resource-aware-execution.md`, conservative quad culling checkpoint. These surface-binding measurements do not close GuideInterpolate **E-4**. |

---

## 3. Rest binding: `UsdGenRestAPI`, root frames and the skin primvars

### 3.1 `UsdGenRestAPI`

Applied to the bound surface `Mesh`, and **only** to the parent `Mesh` — never to a `GeomSubset`
(ADR §9 R15, `02-schema.md` §2.15). The rest channel is a whole-mesh points array and a subset
carries only face indices into that mesh; applying the API to a `geomSubset` prim is a hard
diagnostic (`06-imaging.md` §2.6), asserted by `testUsdGenRestAdapter` (`06-imaging.md` §10.2).
It names where the rest pose comes from, which is the one thing the evaluator cannot infer.

| Property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:rest:source` | `uniform token` | `"default"` | `default` = the surface's `points` sampled at `UsdTimeCode::Default()`; `primvar` = an authored rest primvar; `asset` = a rest mesh in a separate file |
| `usdGen:rest:primvar` | `token` | `"rest"` | the primvar name used when `source = "primvar"` |
| `usdGen:rest:file` | `asset` | `@@` | used when `source = "asset"`; one `asset`, never `asset[]` (S13). The empty asset path is the codeless-schema fallback (`02-schema.md` §2.15) |

The rest surface is delivered by an **API-schema adapter data source that samples
`UsdTimeCode::Default()`** and honours an authored `primvars:rest` when present (S12, MEASURED in
`research/G-stage-free-parameter-and-time-transport.md` §3). Capture-on-first-cook is rejected: it
makes the rest pose depend on which frame the user happened to open the file on.

The adapter is **not** one of the five prim adapters. It is the **sixth** registration in
`02-schema.md` §7.3 — **`UsdGenRestAPIAdapter`** (ADR §9 R3), whose base is
`UsdImagingAPISchemaAdapter` and whose plugInfo key is `apiSchemaName: "UsdGenRestAPI"`, distinct
from the five `UsdImagingSceneIndexPrimAdapter` entries keyed by `primTypeName` (ADR §2.3, §5.1).
ADR §9 R3 pins the whole set as `UsdGenPrimAdapterBase` plus `UsdGenOperatorAdapter`,
`UsdGenMapAdapter`, `UsdGenGroomAdapter`, `UsdGenDescriptionAdapter`, `UsdGenGuideSetAdapter` and
`UsdGenRestAPIAdapter`, and `02-schema.md` §7.3's plugInfo block spells exactly those names.
Registering the rest surface as a prim adapter is the mistake to avoid: it would never fire, because
`UsdGenRestAPI` is an *applied API schema* on someone else's `Mesh`. Gate **SI-7** covers it
alongside the five: every `usdGen:*` property of every registered type, `UsdGenRestAPI` included,
appears in Hydra and dirties.

### 3.2 The root frame

For a curve bound to face `f` at surface parameters `uv`, the root frame is an orthonormal basis
plus an origin:

```
P    = surface position at (f, uv)                        # interpolated from the face's vertices
Nraw = interpolated authored normal at (f, uv), else the geometric face normal
Tuv  = dP/du at (f, uv)                                   # from the face's parameterisation
N = normalize(Nraw)
T = normalize(Tuv - N * dot(N, Tuv))                      # Gram-Schmidt against N
B = cross(N, T)
F = [ T | B | N ]  with origin P                          # column-major 3x3 plus translation
```

`F_rest` is computed once per capture epoch from the rest surface; `F_anim` is computed every frame
(and every motion sample) from the deformed surface. If `primvars:usdGen:rootFrame` carries a value,
it is used as `F_rest` verbatim and the rest computation is skipped — that is the whole reason the
optional primvar exists (A7 §9.1 G7).

**Degenerate cases, decided:** if `|Tuv| < 1e-6` (a pole, a collapsed UV), `T` falls back to the
projection of the face's first edge; if that also degenerates the curve is dropped and counted in
`UsdGenNodeStats`. If `dot(Nraw, geometric normal) < 0` the authored normal is used as-is (the
artist's mesh is inverted on purpose); usdGen never flips it silently.

### 3.3 `skinprim` and `skinprimuv`

`primvars:skinprim` (uniform `int`) is the bound face index and `primvars:skinprimuv` (uniform
`texCoord2f`) is the position within that face. The naming follows Houdini's *Prim Num Attribute* /
*Prim UVW Attribute* so that a Houdini-authored groom round-trips (A7 §3.6, §3.7).

**Face indices are always indices into the parent mesh** (ADR §2.3, "Surface targets"). When
`usdGen:surface` targets a `GeomSubset`, the subset restricts *scatter* to its `indices` (a face set;
`pxr/imaging/hd/geomSubsetSchema.h:37-43`, `GetIndices()` at `:81`), but `skinprim`, Ptex face ids
and the rest lookup are never subset-local. Two consequences:

1. A curve set bound through a subset stays valid if the subset is edited to a different face list —
   only the *scatter* changes, and only for generators.
2. Editing a subset's `indices` is a **recapture** of the nodes that read it, not a rebind of the
   curves that already exist: a dirty on the subset prim's `geomSubset/indices` routes as
   `UsdGenDirtySurfaceTopo` (ADR §9 R15). Because ids hash the **parent mesh's** face index, the
   edit preserves ids on every face that remains.

`skinprimuv` is the face's parametric `(u, v)`: barycentric `(u, v, 1-u-v)` on a triangle, the
bilinear parameter on a quad. For an n-gon (n > 4) the face is triangulated in the same fan order
UsdImaging uses and `u`'s integer part addresses the fan triangle — ASSUMPTION, and the only n-gon
convention that survives a mesh whose triangulation is not stored. `testUsdGenRootFrame` round-trips
a random point on a 5-gon through `(f, uv)` to within 1e-5.

### 3.4 Computing bindings when they are absent

Imports frequently have no `skinprim`. The loader then binds by closest point on the **rest** surface:

1. Build (or reuse) a `UsdGenBindingCache` per surface capture epoch: a nanoflann kd-tree over the
   rest positions of the surface's face centroids (S38, nanoflann 1.12.1).
2. For each curve root (its first CV in the rest buffer), query the `k = 8` nearest centroids and run
   an exact point-in-triangle / closest-point-on-triangle test against each candidate's
   triangle/fan patches; keep the nearest. For quads, use the bilinear patch implied by
   §3.3, including its boundary edges, rather than treating a split-triangle coordinate
   as bilinear UV. On a warped quad those are different surface points. The bounded
   bilinear solver must report failure instead of publishing an unconverged binding.
   This is a nearest-candidate search over eight centroids, not a proof of global
   nearest distance across every mesh face. (Convention resolved 2026-09-13; the
   portable binding helper and its numerical/performance gates are in progress.)
3. Store `(faceIndex, uv)` and, when `usdGen:rebind != "never"`, author them back onto the source
   prim on the tool's explicit "Bake bindings" action (never automatically — that would be a stage
   write from an evaluator, which R1 forbids).

Cost: the same kd-tree shape as `GuideInterpolate`'s capture, so it is bounded by gate **E-4**
(100 k / 1 M rest roots at 8 threads, ≤ 25 ms and **linear** in roots), which runs as M0 pre-work
item **PW-1** (`11-roadmap.md` §1) and binds at M3 (ADR §9 R40). UNMEASURED until E-4 runs.

### 3.5 Rebinding when the surface changes

The surface's topology hash is part of every capture epoch (`03-execution-engine.md` §3.4,
ADR §4.1 I4), so a topology edit re-captures the affected sub-graph. What happens to the *authored*
bindings on a C3 prim is a separate policy, because usdGen cannot re-derive them:

| `usdGen:rebind` | Face index out of range | Face index in range but topology changed |
|---|---|---|
| `never` | the curve is dropped, counted, and one warning names the prim | the binding is trusted |
| `onError` (default) | the curve is rebound by closest point (§3.4) | the binding is trusted |
| `always` | rebound | rebound |

Independently, a topology change bumps the frozen epoch of any freeze made against that surface
(§5.3), so the artist sees a **stale** badge even when `rebind` silently repaired the bindings. The
separation is deliberate: repairing geometry is cheap and reversible; telling the artist their frozen
look no longer matches the rig is what they actually need to know (A7 §9.3).

---

## 4. `UsdGenDeform` — transporting curves onto the deformed surface

### 4.1 The prim

`UsdGenDeform` derives from `UsdGenDeformer` (ADR §2.1): it is `deformedSpace` by class and it
re-runs per motion sample.

| Property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:input` | `rel` | — | one upstream node |
| `usdGen:mode` | `uniform token` | `"rigidFrame"` | `rigidFrame` (v1) \| `rbf` (v2) \| `pointDeform` (v2). An unimplemented mode is a compile error naming the prim and the mode; it is never silently downgraded |
| `usdGen:twistAware` | `bool` | `true` | see §4.3 |
| `usdGen:preserveShape` | `float` | `0.0` | 0 = off; strength of the Cosserat shape-preservation pass (A7 §3.5). **v2**; in v1 a non-zero value is a warning and is ignored |
| `usdGen:preserveShape:iterations` | `int` | `0` | Cosserat iteration count, v2 |
| `usdGen:rbfSamples` | `int` | `100` | `mode = "rbf"` only (v2); Unreal's ≤100-sample binding |
| `usdGen:readPhase` | `uniform token` | `"final"` | `base` \| `preceding` \| `final` \| `@<primPath>` (S26) |
| `usdGen:space` | `uniform token` | `"auto"` (inherited, not redeclared) | Tokens are `auto \| rest \| deformed` only — **there is no `world` token**, because post-flattening deformed space *is* world space (ADR §9 R9, S4). `auto` resolves to `deformed` for this type **by its declared class** (S25, `02-schema.md` §2.5 and §2.8); authoring `rest` on a `UsdGenDeformer` is a compile error naming the prim. The engine enum is `UsdGenSpace { Inherit, Rest, Deformed }`, `Inherit` ⇔ `auto` |

Mode switches are **value edits**, not prim delete/create resyncs (ADR §2.1) — that is why one prim
type carries three algorithms.

### 4.2 Reading the deformed surface

usdGen's hair scene index is a renderer-level `HdSceneIndexPlugin` at insertion phase 0,
`InsertionOrderAtEnd` (S1), so everything it reads is **post-flattening and post-deformation**: after
RigExec, after UsdSkel, after any third-party `UsdImagingSceneIndexPlugin`, after instancing
(MEASURED, `research/G-chain-order-probe.md` §2–3). The surface read has three cases:

1. **Plain or RigExec-deformed points.** `primvars/points` carries a retained `VtVec3fArray`; read it
   with `HdPrimvarsSchema::GetFromParent(ds).GetPrimvar(points).GetPrimvarValue()->GetValue(0)`
   (`research/A2-usdrig-imaging.md` §8).
2. **UsdSkel-skinned points.** `primvars/points` is **blocked** with an `HdBlockDataSource` and the
   values live behind `extComputationPrimvars/points`
   (`pxr/usdImaging/usdSkelImaging/dataSourceResolvedPointsBasedPrim.cpp:291,506`, MEASURED read).
   usdGen does not re-implement skinning: it wraps its own input in a **private**
   `HdSiExtComputationPrimvarPruningSceneIndex`
   (`pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h:35-41`), which "prunes computed
   primvars and presents them as authored primvars … the computation is executed when pulling on the
   primvar's value" (header comment, `:26-28`). That is S3, and the wrapper is **never spliced into
   the shared chain** — it is constructed around the hair index's own input, so no other consumer
   pays for it. It is a pass-through when nothing is computed.

   The same header notes at `:32-33` that the index "is in service of emulated ExtComputations
   (i.e., when `HD_ENABLE_SCENE_INDEX_EMULATION` is true)" — the one line that decides whether the
   wrapper resolves `skinnedPoints` in the native chain S3 promises. Nothing on this host has run it,
   so the wrapper is both the largest correctness risk in §4 and its only cost term on a skinned
   scalp. Both halves are gate **SI-9** (T1, **M2**): ADR §9 R40 binds SI-9 to "pruning-wrapper cost
   on a production-density skinned scalp", and this document reads it as that cost plus the
   assertion that the wrapper resolves `skinnedPoints` with `HD_ENABLE_SCENE_INDEX_EMULATION`
   **off** — the one thing the cost is worthless without.
   `09-performance-and-benchmarks.md` §5 is the single gate registry (ADR §9 R40) and carries the
   SI-9 row. The session-arbitration check — two attached indices agreeing on generation, prim set
   and frame, which `12-risks-decisions-open-questions.md` RK-09 asked for — is a separate gate,
   **SI-10** (T1, M2, `testUsdGenSessions`), registered in 09 §5.2; SI-9 in this document means the
   pruning-wrapper measurement and nothing else.

   `testUsdGenSkelInterop` (§9, T1) is therefore a **stop condition**, not just a test — **SC-13** in
   `11-roadmap.md` §5.2, the stop-condition register: SI-9 red ⇒ execute the aggregator computation
   ourselves from `extComputation/inputValues` (`restPoints`, `geomBindXform`, `influences`,
   `skinningXforms`, …; `pxr/usdImaging/usdSkelImaging/dataSourceResolvedExtComputationPrim.cpp:80-160`)
   through the public `UsdSkelSkinPoints` kernel (`pxr/usd/usdSkel/utils.h`), subscribing to
   `extComputation/inputValues/*` on the computation prims — `research/A2-usdrig-imaging.md` §8
   option (a), its own practical rule. It costs one CPU skinning pass per deforming frame and M2
   re-plans around it.
3. **`HD_ENABLE_DEFERRED_SKINNING=true`.** Skinning moves into the renderer and no CPU points exist
   anywhere (`pxr/imaging/hd/skinningSettings.cpp:15-19`, A2 §8). usdGen detects this at session start
   and emits one hard diagnostic per groom: unsupported in v1, remedy is one env var. ASSUMPTION:
   the configuration is rare enough not to warrant duplicating the vertex shader on the CPU.

`usdGen:readPhase` selects *which* surface state to read (S26): `base` = the rest surface at
`Default()` (S12), `final` = the surface as usdGen sees it post-flattening, `@<primPath>` = a named
prim, which is how a low-resolution proxy scalp is used to drive a dense groom. `preceding` is an
unconditional v1 alias for `final` (ADR §9 R9; `03-execution-engine.md` §1.6).

### 4.3 The transport

For each curve `c` with rest root frame `F_rest(c)` and animated root frame `F_anim(c)` (§3.2), mode
`rigidFrame` computes, per CV `i`:

```
M(c)      = R_anim(c) * transpose(R_rest(c))              # 3x3, both orthonormal -> transpose == inverse
P_out[i]  = P_anim_root(c) + M(c) * (P_rest[i] - P_rest_root(c))
```

with `usdGen:blend` applied as an exact-endpoint envelope:
`P_final[i] = lerp(P_rest[i], P_out[i], blend)`, `blend = 0` and `blend = 1` reproducing the inputs
bit-for-bit (the `RigExecBlendEnvelope` contract, A1 §5, and the rule every operator obeys in
`04-operators.md`).

**`usdGen:twistAware`.** With `twistAware = true`, `T` comes from the surface's `dP/du` (§3.2), so a
surface that twists about its normal twists the hair with it — correct on a wrist or a tail, wrong on
a sphere whose UV seam sweeps past. With `twistAware = false`, `T` is the projection of a fixed
reference axis into the tangent plane: the coordinate axis **of the surface's own space** least
parallel to the mean rest normal, chosen once per surface at capture. Surface-local, because usdGen
reads post-flattening data in which `points` are surface-local and `xform/matrix` carries the world
transform (S4, §4.6), so the choice is invariant under the surface's world matrix. Where
`|N × axis| < 1e-6` for an individual curve the second-least-parallel axis is used, and the choice is
recorded per curve at capture so it cannot flip between frames. The hair then follows the normal and
never the parameterisation, and both poses use the same rule, so there is no pop at `blend = 0`.

`rbf` (a displacement field from ≤100 surface samples, the Unreal binding shape) and `pointDeform`
(per-CV weights over surface points, Houdini's capture-and-deform) are **v2** (A7 §9.1 G7). They
exist in the token now so that a v2 upgrade is a value edit, not a prim swap.

### 4.4 Where `UsdGenDeform` sits, and what may follow it

The chain shape this document exists to support is:

```
UsdGenCurveSource  →  UsdGenDeform  →  (deformed-space stylers)  →  terminal
        rest              deformed              deformed
```

The topologically sorted node list is partitioned by `Space()` into a **rest head** `H` and a
**deformed tail** `T`. `T` begins at the **first** node in topological order whose resolved space is
`deformed`, and contains every node at or downstream of it — the **downstream-closure** rule. A
`restSpace` node downstream of a `deformedSpace` node is therefore legal: it keeps its rest-class
arithmetic (`UsdGenSpace::Inherit` resolves to the type's own class) but is scheduled in the tail,
because its input moves every frame (`03-execution-engine.md` §1.6, which owns the arithmetic;
S25, I4). In the canonical chain above `UsdGenDeform` is that first node, and §2.6's
sim-cache case is the same rule with a `deformedSpace` `UsdGenCurveSource` opening the tail.
Consequences, all measured or gated:

* A frame change dirties only `T`'s chunks. The frame ledger — `09-performance-and-benchmarks.md`
  §0.2, the only one (ADR §9 R41) — budgets the deformed tail at **0.4–0.6 ms** over 800 k CVs,
  **DERIVED** from a MEASURED 0.16–0.55 ms single styler pass
  (`research/G-data-plane-engine-prototype-benchmark.md` §6) and a MEASURED five-node chain of
  1.02 ms at 8 threads (1.72–1.91 ms at 20; same report §3.3, §4 — every budget quotes the 8-thread
  private-arena number, ADR §9 R27). Gate **E-1** bounds the whole chain at ≤ 1.5 ms and gate
  **E-3** bounds the analogous "last parameter only" case at ≤ 0.6 ms (baseline 0.18–0.41 ms
  MEASURED).
* Stylers placed *below* `UsdGenDeform` run in deformed space and are re-run per motion sample.
  Stylers placed *above* it run in rest space, are cached across frames, and are the reason a
  100 k-curve groom costs a tail and not a chain per frame. `04-operators.md` marks each operator's
  default class; `usdGen:space` overrides it per prim.
* A `UsdGenSculptLayer` may sit on either side. Above `UsdGenDeform` its deltas are baked into the
  rest pose and transported with the curve (the usual comb workflow); below it they are applied in
  the animated root frame, which is what a shot-specific fix wants.
* **Cross-chunk deformed-space operators are v3.** `Collide`, `Wind` and `Smooth(neighbours)` need a
  per-frame spatial structure; they get a two-pass gather/scatter node kind on a per-frame grid, and
  never a chunk fan-in (ADR §4.2.5).

### 4.5 Per-motion-sample re-run

Motion is a Description-level property set (ADR §2.3): `usdGen:motion:mode = single | velocities |
samples`, `usdGen:motion:sampleCount = 3` (2…16), `usdGen:motion:forwardSurfaceSamples = false`.

| Profile | `GetContributingSampleTimesForInterval` | Evaluations per frame | Cost model |
|---|---|---|---|
| **P0 `single`** (default; Storm; hdEmbree) | returns `false` | 1 × tail | `T` |
| **P1 `velocities`** (v1, lands M7 — ADR §9 R38) | returns `false`; the index publishes `primvars/velocities` and blocks any upstream ones | 1 × tail + 1 memory-bound pass | `T + m`, `m ≈ 1.0 ms` per 1.6 M points MEASURED |
| **P2 `samples`** (v1, lands M7 — ADR §9 R38) | returns `true` + the retained offsets (≥2) | `k` × **tail**, not `k` × chain | `k·T + (k−1)·m` |

Storm never asks for more than one sample (MEASURED,
`research/G-motion-blur-sampling-strategy.md` §1.2), so P2 costs the viewport nothing as long as the
offsets are filled lazily on the first `GetContributingSampleTimesForInterval` pull or eagerly by an
app preflight (§4.2 of that report). The per-offset cache is `UsdGenMotionCache`, keyed by
`(graphGeneration, surfaceGeneration, absoluteTime)` so whole-frame offsets reuse the previous
frame's `+1` while scrubbing forward, with lerp between the two bracketing retained offsets and clamp
outside — hdPrman re-distributes to `ri:object:geosamples` and *will* ask for non-retained times
(§4.5 of that report). 19.2 MB per sample at 100 k × 16 CV, MEASURED.

**`usdGen:motion:forwardSurfaceSamples`** decides where P2's offsets come from. `false` (default)
clamps the surface's contributing times to the shutter window; `true` forwards them raw, which
reproduces UsdImaging's `[-1, 0, +1]` and **triples the tail cost** relative to `{open, close}` while
evaluating at times far outside a ±0.25 shutter (offset arithmetic over the MEASURED per-pass costs,
`research/G-motion-blur-sampling-strategy.md` §5; the tail cost itself is UNMEASURED, gate R-1). The
option exists for exact parity with a surface's own blur and is off by default.

`velocities` and `accelerations` are blocked on every tile whenever usdGen owns `points`, in every
profile except P1 (S29, S32): a stale upstream `velocities` would otherwise be extrapolated from
usdGen's new points. Never bake motion samples into a freeze (§1.5).

### 4.6 Spaces, `xform` and `extent`

Everything usdGen reads is post-flattening, so `xform/matrix` on a surface is world (or
prototype-common) space with `resetXformStack = true`, and dirtiness is per prim, never hierarchical
(S4). Output tiles therefore carry `xform = the surface's world matrix, resetXformStack = true` and
**surface-local points**, and re-dirty their own `xform` whenever the surface's is dirtied.

`extent` is published per tile **every deforming frame** — without an authored extent the bounding
box is the default-constructed `GfRange3d`, `[FLT_MAX, -FLT_MAX]`, and the prim is never
frustum-culled (`pxr/imaging/hdSt/primUtils.cpp:887-891`, which sets `sharedData->bounds` and states
the consequence in comment; the inverted box then reaches the GPU test through `bboxLocalMin` /
`bboxLocalMax`, `pxr/imaging/hdSt/shaders/frustumCull.glslfx:162-166`). usdGen computes it during the
SoA→AoS interleave that already touches every point, and **pads it by half the tile's maximum
width** so a strand whose ribbon crosses the frustum edge is not culled with its centreline outside.
The ledger budgets 0.05 ms for the **49** tiles a 100 k-curve description publishes at the default
partition (`chunkSize = 512`, `tileTarget = 64` ⇒ 196 chunks, `chunksPerTile = 4`, `nTiles = 49`;
ADR §9 R21, `03-execution-engine.md` §1.4) as part of the interleave pass — **ASSUMPTION**, the tag
`09-performance-and-benchmarks.md` §0.2 gives that row (min/max fused into the interleave loop, never
measured separately); gate **S-2** (deform frame ≤ 2.0 ms delta over static) bounds the whole
publish.

**64 tiles is the 1 M-curve row of that arithmetic, not the 100 k one** (ADR §9 R21).
`09-performance-and-benchmarks.md` §0.2, gate S-2, `01-architecture.md` §0.3's extent and publish
rows and `03-execution-engine.md` §6.3 all carry 49 at the 100 k fixture.

### 4.7 Invalidation

| Incoming | Reaction |
|---|---|
| `primvars/points/primvarValue` on a bound surface | re-run the deformed tail for the chunks bound to that surface; chunk order is surface-major, so a surface dirty is a **range** (`03-execution-engine.md`) |
| `xform/matrix` on a bound surface | re-dirty the dependent tiles' `xform` only; no re-evaluation |
| `extComputationPrimvars` or `extComputation/inputValues/*` on `<mesh>/skinningPoints*Computation` | same as a points dirty — the pruning wrapper re-executes the computation on pull (A2 §8) |
| `primvars/points` on a **frozen** C3 prim | re-load the points plane of that source node and re-run its deformed tail — **not** a re-capture: ids and topology did not move (§4.7.1, §5.8) |
| `basisCurves/topology/*` on a C3 prim | `UsdGenDirtyTopology`: the id mapping may have changed, so every downstream sculpt layer re-matches by `curveId` (§6.3) |
| `UniversalSet()` on a surface | treat as a resync of that surface: re-read type, topology, rest and points |

Outgoing dirties follow ADR §5.2: the **bare** `primvars/points/primvarValue` plus `extent/*` per
dirty tile where usdGen owns the prim; `HdContainerDataSourceEditor::ComputeDirtyLocators`
**and the bare `primvars` locator** for **every** overlay the index makes on an upstream prim it does
not own — the `visibility` overlay on C3 source prims, the CV-display child of §7.3, and any primvar
the index adds (ADR §9 R29, §5.7). The bare-`primvars` half is S5, and it is a measured
usdRig/UsdSkel interop bug: without it the resolved prim freezes. `displayColor` is never
co-dirtied with `points` (S30): `DirtyPrimvar` is one bit for every primvar except
points/normals/widths (`HdChangeTracker::IsPrimvarDirty`,
`pxr/imaging/hd/changeTracker.cpp:957-979`), so a `displayColor` edit re-uploads every non-points
primvar. Gate **SI-2** asserts the exact locator set.

### 4.7.1 How a dirty on a foreign curve prim reaches usdGen

The fourth and fifth rows of the table above (`primvars/points` and `basisCurves/topology/*` on a C3
prim) are dirties on prims usdGen does **not** own. Nothing routes those to us by default, and the
freeze report names this as the hard half: learning that a target now holds different data "has
exactly two mechanisms, and neither is free — either the operator adapter republishes the resolved
target (and the hair SI keeps `frozenPath -> dependent operator prims` as a reverse map, re-emitting
`PrimsDirtied` on the dependents …), or the evaluator that owns the stage does the resolution and
pushes into a store" (`research/G-freeze-bake-undo-and-frozen-reentry.md` §2.3).

usdGen takes the first, and it costs nothing extra because the machinery exists.
`UsdGenDirtyRouter` (`03-execution-engine.md` §5.1) is keyed by `SdfPath` and is built at compile
"from every node's `TopologyParameters()`/`ValueParameters()` **and its relationship targets**".
Compile step 6 already resolves each node's `usdGen:curves` / `usdGen:frozen:curves` into
`UsdGenGraphDesc::curveSets` (elements `UsdGenCurveSetDesc`, ADR §9 R23; `03-execution-engine.md`
§2.2 declares `std::vector<UsdGenCurveSetDesc> curveSets` and its §3.1 step 6 binds into it), so the
same step adds one router entry
per resolved target path: the reverse map `SdfPath(C3 prim) -> {node ids}`. `_PrimsDirtied`
consults it with the same hash lookup it uses for operator prims, so a comb stroke on a frozen prim
costs one lookup, not a scan.

The router key is the **full locator path** below the prim, never its first two elements
(ADR §9 R25), and the dirty class differs per locator. The three classes cost very different amounts
(`03-execution-engine.md` §3.6 prices them), so this table is normative and §5.8 refers to it rather
than restating it:

| Locator on a C3 source prim | Dirty class | What re-runs |
|---|---|---|
| `primvars/points` | `UsdGenDirtyParameter` | re-load the points plane, re-run the deformed tail of the dependents. The capture is untouched: ids, counts and bindings did not move |
| `primvars/rest`, `primvars/skinprim`, `primvars/skinprimuv`, `primvars/usdGen:curveId`, `primvars/usdGen:rootFrame` | `UsdGenDirtyCapture` | re-capture the source node and everything downstream; the capture epoch bumps |
| `primvars/usdGen:frozenEpoch` | *(none)* | re-check staleness and nothing else: no dirty bit, no re-capture, no re-evaluation (§5.8) |
| `basisCurves/topology/*` | `UsdGenDirtyTopology` | recapture downstream plus one topology publish; every downstream `UsdGenSculptLayer` re-matches by `curveId` (§6.3) |
| relationship or custom attribute | *(never arrives)* | invisible to Hydra (§1.2) |

That taxonomy is the report's, verbatim in intent: "a `primvars/points` dirty on a frozen prim
invalidates only the deform-transport tail of its dependents; a `primvars/usdGen:frozenEpoch` dirty
means *re-check staleness*; a `basisCurves/topology/...` dirty means the id mapping may have changed"
(MEASURED locator taxonomy, `research/G-freeze-bake-undo-and-frozen-reentry.md` §2.4).

This satisfies S14 by the *pull-everything* route — capture reads every field of C3 once per topology
generation. Declaring `__dependencies` instead (ADR §5.3 authors one edge per **tile**) is rejected:
the dependency is per source prim, not per tile, so it would add `nTiles` edges for one source, and
`HdDependencyForwardingSceneIndex` fan-out costs 0.17 / 0.38 / 0.90 / 1.30 µs per prim at
N = 32 / 1 k / 10 k / 100 k (MEASURED, **EV-031**).
Tiles keep their surface `__dependencies` edge as C2 specifies; sources do not get one.

---

## 5. `UsdGenFreeze`

### 5.1 The prim

| Property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:input` | `rel` | — | the operator whose output was frozen |
| `usdGen:frozen:curves` | `rel` | — | exactly one target: the authored C3 `BasisCurves` holding the snapshot |
| `usdGen:frozen:mode` | `uniform token` | `"frozen"` | `frozen` \| `live` |
| `usdGen:frozen:epoch` | `uniform string` | `""` | must equal the target's `primvars:usdGen:frozenEpoch` |
| `usdGen:frozen:tier` | `uniform token` | `"session"` | `session` \| `sublayer` \| `payload` — a record of where the tool put the data. **The evaluator never reads it**; it exists so "Promote to sublayer" knows what it is promoting |

### 5.2 What a freeze caps, and what stays authored

A freeze **caps** the chain; it does not replace it (A7 §1.4, XGen *Groom Bake*: "deactivates all
modifiers below it", reversibly). With `mode = "frozen"`:

* Every node upstream of the freeze is excluded from the compiled DAG. Its prims stay on the stage,
  stay authored, and the stack editor greys them.
* The freeze compiles to a source node that loads `usdGen:frozen:curves` through the §2.2 path.
* Every node downstream is unchanged. This is the frozen-reentry loop (§5.7).

With `mode = "live"` the freeze compiles to a pass-through and the upstream chain runs normally. The
frozen prim stays on the stage, so re-freezing is a token edit plus a data rewrite, never a
re-authoring of the chain.

`frozen:mode` and `frozen:curves` are both terms of `d(n)` — `mode` and the sorted relationship
targets (ADR §4.2.1). A digest is a function of a node's **inputs**, so a `frozen:mode` flip changes
`d(freeze)` and the freeze plus every node **downstream** of it recompiles
(`03-execution-engine.md` §3.2). What changes upstream is *membership*, not a digest: `frozen` drops
the head from the compiled DAG and `live` re-admits it, which is the separate and much larger cost
accounted immediately below. **The two directions do not cost the same, and the gesture looks free in
the UI, so state it:**

* `live -> frozen` is cheap. The head is *removed* from the DAG and the frozen buffer is already
  loaded, so the next commit is a load (0.19–0.20 ms MEASURED, §2.7) plus the tail.
* `frozen -> live` **re-admits every node upstream of the freeze**, and each re-admitted node with
  no cached capture pays its `Capture()` in full — scatter, kd-trees, guide weights, map / Ptex /
  SeExpr sampling (ADR §4.1 I4; the capture terms are `03-execution-engine.md` §4.1). Gate **E-6**
  (≤ 0.2 ms, exactly 1 node rebuilt) bounds only the **compile**, never the capture, and E-6's
  fixture is "append one node to a 200-node groom" — a different gesture.

The first frame after an unfreeze is therefore UNMEASURED: bounded below by E-1's five-operator
chain — **≤ 1.5 ms** (ADR §9 R27; MEASURED 1.02 ms at 8 threads,
`research/G-data-plane-engine-prototype-benchmark.md` §3.3) — and above by whatever capture the head
contains; a head carrying a
`GuideInterpolate` pays E-4's kNN as well. The tool shows a progress state on unfreeze and reports
the re-captured node count in the stack profiler. Unfreeze is still one token edit with no prim
removal (§5.6); it is not a free one.

### 5.3 The epoch

```
usdGen:frozen:epoch  ==  primvars:usdGen:frozenEpoch  ==  "usdgen1:sha1:" + hex(SHA1(stream))
```

where `stream` is the canonical concatenation of

1. `usdGen:schemaVersion` (the `usdgen1:` prefix is the human-readable form of the same number,
   ADR §2.3 row "Versioning"; `02-schema.md` §2.19);
2. the frozen node's **capture epoch** (128-bit), which already covers the surface's topology hash and
   rest-points hash, the seed, every capture-time parameter, every map's resolved asset path plus its
   texture generation counter, and the upstream node's capture epoch
   (`03-execution-engine.md` §3.4, ADR §4.1 I4; the structural digest formula is ADR §4.2.1);
3. the **structural digest** of the sub-graph up to and including the frozen node;
4. the curve count and a hash of the `curveVertexCounts` vector.

That is exactly A7 §9.3's "hash of (scatter seed, surface topology, guide ids)", made precise by
reusing digests the engine already maintains.

**Mismatch means *stale*, not *wrong*, and nothing switches by itself.** ADR §9 **R18** rules this
directly: "warn and keep rendering the frozen data; never silently re-cook a chain the artist froze."
On a `UsdGenFreeze` the stack editor badges the node and one `TF_WARN` names both epochs; the
evaluator keeps rendering the frozen data (the rule `design/proposal-risk.md` §3.7 states and R18
ratifies). The artist unfreezes, or re-freezes, in one token edit (§5.6). R18 also settles two
adjacent questions: `usdGen:staleAction` exists **only** on `UsdGenCurveSource`, and
`usdGen:frozen:tier` is advisory — the evaluator never reads it.

Falling back to `live` automatically would be a cliff, and term 2 is why: the epoch carries every
map's resolved asset path **plus its texture generation counter**, so S13's explicit "reload maps"
action bumps the capture epoch of every map consumer (`03-execution-engine.md` §3.6, row "map asset
path or `ReloadMaps()`") and marks every freeze below a map stale at once. Under an automatic
fallback that one action silently re-runs every chain the freezes exist to cap (§5.2).
`02-schema.md` §2.9's `usdGen:frozen:epoch` row states the same rule in the same words — the
evaluator "keeps rendering the frozen data — a freeze is never silently re-cooked".

One case does still fall back to `live`, and it is a different one: an **unrecognised `usdgen<N>:`
prefix**, i.e. a freeze written by a newer major schema whose bytes this binary cannot be trusted to
read at all. That is `02-schema.md` §8.5's rule and it stands unchanged. Same prefix, different
digest = stale, keep rendering; unknown prefix = fall back to the authored chain.

On a `UsdGenCurveSource` there is no upstream to fall back to, so `usdGen:staleAction` decides
between warn / ignore / block (§2.3, row 11).

### 5.4 Landing tiers

| `usdGen:frozen:tier` | Where | Author + save | Bytes at 100 k × 8 CV | Reopen / compose | Undo | Survives |
|---|---|---:|---:|---:|---|---|
| `session` | usdview's session layer (the default edit target, `pxr/usdImaging/usdviewq/appController.py:1283`) | **0.4 ms**, no I/O | 0 | — | `SubtreeSnapshot` stash (**EV-040**) | dies with the session |
| `sublayer` | `<asset>_groomBake.usdc`, sublayered | 10.6 ms (**EV-046**) | 9 601 971 (**EV-046**) | 0.2 ms compose, 0.1 ms first `points.Get()` (**EV-049**) | drop or mute the sublayer, 0.13–0.21 ms, flat in curve count (**EV-043**) | yes |
| `payload` | the same `.usdc`, arced by payload on a session `over` | 10.6 ms (**EV-046**) | 9 601 971 (**EV-046**) | **21.5 ms** compose, 1.5 ms first `points.Get()` (**EV-049**) | `RemovePrim` on the over, **outside the interactive path** and subject to both landmines of §5.6 | yes, and it is **unloadable** |

All MEASURED; the `session` row's 0.4 ms is the author-only figure of
`research/G-freeze-bake-undo-and-frozen-reentry.md` §3 and has no ledger row of its own. The three
tokens are the only names for these places: S42's "T1/T2/T3 landing tiers" are retired from prose
(ADR §9 R1) because T0–T4 are the test tiers and `T-1…T-5` are the tool gates. The `payload` row's
`RemovePrim` must run in the layer the over was authored into, and raises a spurious
`Tf.ErrorException` when an OpenExec system is attached — contain it with a `Tf.Error.Mark` bracket
plus a post-condition assert (§5.6, S41 as corrected by
`12-risks-decisions-open-questions.md` §2 RK-08). Undo of an *interactive* freeze is always
`SetActive(false)`, never `RemovePrim`.
Never `.usda` (40 317 359 B, 368 ms to write, 532 ms to reopen — a 660× read penalty; **EV-046**,
**EV-047**). Never bake motion samples (24 samples = 230 402 598 bytes, **EV-046**); author
`velocities` instead, which is 2× and is what
`HdsiVelocityMotionResolvingSceneIndex` consumes.

`session` is the interactive comb/sculpt loop, `sublayer` is the artist's "commit results to the
stage via the tooling" (R9), `payload` is shot-level assembly of very heavy grooms.

### 5.5 The write path

Specified here because three separate measurements constrain it; `08-tools.md` owns the UI.

**Rule 0 — a `UsdGenFreeze` may only cap a `restSpace` node.** A freeze inside the deformed tail
(below a `UsdGenDeform`, which §4.4 permits stylers to occupy) is a **compile error** naming the
freeze and the first `deformedSpace` node above it.

**Rule 0 is a decision this document makes; the ADR is silent.** ADR §2.3's freeze row says only that
the freeze caps the chain. The rule is folded into `02-schema.md` §2.9 as a compile-time constraint
on the freeze's `usdGen:input`, and into `08-tools.md` §4.1 as "the freeze bar greys the action on
nodes below the rest/tail split", so C1 freezes it at the end of M1 (ADR §3). The **correctness**
reason is one:

* Re-entering a deformed snapshot above a `UsdGenDeform` would deform it twice — the error
  `UsdGenCurveSource` already guards with `usdGen:useRest = false` (§2.6, item 2).

There is also a cost, which is a consequence and not a justification: a deformed freeze pays a second
9.6 MB per 800 k CVs, because `primvars:rest` can no longer share the `points` buffer (MEASURED,
+26 bytes shared vs +9 600 046 bytes distinct, **EV-048**). A freeze of a deformed node could simply
not author `primvars:rest`; that is why the size is not the rule's reason.

A shot-specific frozen-**deformed** cache is a legitimate thing to want. It is an **export**, not a
freeze, and it re-enters through `UsdGenCurveSource` with `usdGen:useRest = false`, where that guard
and the `deformedSpace` classification of §2.6 apply. The freeze bar greys the freeze action on
nodes below the rest/tail split and says why.

1. Evaluate the frozen node to completion at the session's current frame. Because of Rule 0 that
   node is `restSpace`, so `points` and `primvars:rest` are the same `VtArray` object and `rest`
   costs 26 bytes (MEASURED, **EV-048**).
2. `UsdGeom.BasisCurves.Define()` **outside** any `Sdf.ChangeBlock` — `Usd*.Define()` inside a change
   block fails at `UsdStage::_DefinePrim` (`pxr/usd/usd/stage.cpp:3893`, MEASURED, **EV-066**).
3. Author every C3 property **inside one** `Sdf.ChangeBlock`: 60–86 µs for a 100 k-CV `BasisCurves`
   (MEASURED, **EV-066**). Values cross as `Vt` arrays — `attr.Set(Vt.Vec3fArray)` is 1.63–1.82 µs at
   any size (**EV-058**), while `Vt.Vec3fArray.FromBuffer` costs 420 µs per 1.2 MB and
   `attr.Set(numpy)` 480 µs (**EV-057**). Never use the buffer route on groom-sized arrays (S39).
4. Author `usdGen:frozen:curves`, `:epoch`, `:mode = "frozen"` and `:tier` on the `UsdGenFreeze` prim
   in the same change block.
5. Push one `SubtreeSnapshot` undo entry that records **the layer it was authored into** (§5.6).
6. Freezes are **siblings under `<Description>/Frozen/`** (ADR §2.2) and the tool **never re-authors
   that scope's spec**: touching the parent's `typeName` produces 203 `PrimsAdded` over 200 siblings
   and 1.3–1.5 ms of scene-index work, i.e. 203 delegate destroy/create cycles (MEASURED,
   **EV-052**).
7. Batch N freezes into one `Sdf.ChangeBlock` so usdview pays **one** `_resetGUI` and one camera
   traversal. Both are O(stage prims), not O(curves): `_resetPrimView` is 2.64 / 23.0 / 102.2 ms at
   115 / 2 205 / 11 005 prims, identical at 10 k and 100 k curves, and `_updateForStageChanges` runs
   a full `Utils._GetAllPrimsOfType(stage, UsdGeom.Camera)` traversal per freeze
   (`appController.py:2504-2521`, `:3048-3050`; MEASURED, **EV-051**).

### 5.6 Unfreeze, undo and the two USD landmines

**Unfreeze is one token edit**: `usdGen:frozen:mode = "live"`. No prim removal, no resync, no data
loss; re-freezing is the inverse edit. That is the whole point of `mode` being a token rather than
the freeze being a structural presence/absence. It is cheap **on the stage** and it is not cheap in
the evaluator: as §5.2 states, `frozen -> live` re-admits the whole upstream head and pays its
captures. The two statements are about different layers and both are true.

**Undo of a live freeze is `SetActive(false)`**, measured at 0.02 ms at 10 k, 100 k *and* 1 M curves,
against `RemovePrim` at 0.06 / 1.47 / 13.47 ms — the difference is the `free()` (MEASURED,
**EV-044**). Real removal happens only when the undo entry falls off the 200-deep stack, where the
deallocation is off the interactive path. `SubtreeSnapshot` (`Sdf.CopySpec` into an anonymous stash)
is the snapshot unit: 0.10 ms capture, 0.23–0.24 ms undo, 0.08–0.09 ms redo, **zero RSS growth**
(MEASURED, **EV-040**), and 11/11 fidelity assertions including time samples, `Ts.Spline`, nested
children, relationships, `kind` and uniform variability (freeze report §1.2).

Landmine 1 — **`UsdStage::RemovePrim` is a silent no-op when the spec lives outside the current edit
target.** With usdview's session-layer default (`appController.py:1283`), a root-layer prim cannot be
removed at all: `still on stage: 1`, zero notices, 0.00 ms (MEASURED, qualitative, freeze report
§1.5). **Rule: a
freeze may only be undone in the layer it was authored into.** The undo entry records the layer and
`Restore()` is a no-op when `entry.layer.expired`.

Landmine 2 — **`RemovePrim` with an OpenExec system attached raises a spurious
`Tf.ErrorException`.** Root cause is stock OpenUSD 26.08: `pxr/exec/esfUsd/stageData.cpp:361` applies
`UsdPrimDefaultPredicate` to the prim fetched at `:351` without checking validity, and
`pxr/usd/usd/primFlags.cpp:21-27` posts `TF_CODING_ERROR("Applying predicate to invalid prim.")`
before returning `false`. It fires exactly when the resynced path has no prim at the end of the
change; `SetActive(false)`, `CopySpec`-over-live and remove+re-`CopySpec` in one `Sdf.ChangeBlock` do
not trip it (9-case matrix, MEASURED, freeze report §1.4; reproduced with zero usdRig code,
**EV-079**). It is harmless — the removal completes and N removals in one change block coalesce into
one exception — but it must be contained: `TfErrorMark` + `Clear()` in C++ and, in Python,
**`Tf.Error.Mark`** — the class is bound at `pxr/base/tf/wrapError.cpp:212-221`, inside the
`Tf.Error` scope opened at `:200`; the name `Tf.ErrorMark` does not exist
(`12-risks-decisions-open-questions.md` §2 RK-08, which corrects the freeze report's §1.4 claim and
S41/S46 on this point). The bracket is
`m = Tf.Error.Mark(); m.SetMark(); …; errs = m.GetErrors(); m.Clear()`, filtering the
`stageData.cpp:361` predicate error by message, re-raising anything else, and asserting the
post-condition that the prim is gone. File upstream against `stageData.cpp:351,:361` (S46). S41,
S46 and `research/G-freeze-bake-undo-and-frozen-reentry.md` §1.4 all quote `:360`, the last line of
the comment, not the call; `02-schema.md` and `appendix-A-evidence-ledger.md` §3 already carry the
corrected `:351, :361`.

### 5.7 The frozen-reentry loop, and what the description publishes

Frozen curves go **back through** `UsdGenDeform` and every styler below the freeze — the chain shape
§4.4 draws, with a `UsdGenFreeze` in the place of the `UsdGenCurveSource`.

**The description always publishes tiles.** The terminal publishes 32–256 `basisCurves` tiles under
`<Description>/__usdGenRender/` (S27, ADR §2.2, contract C2). There is no second publication shape
and no property that selects one: `usdGen:output (tiles|inPlace)` is **dropped everywhere**
(ADR §9 R8), and the in-place overlay path **does not exist in v1** (ADR §9 R29).

The hair scene index prunes every prim under `<Description>/` except `__usdGenRender`, so `Ops/`,
`Guides/`, `Maps/`, `Prototypes/` and `Frozen/` never draw. A C3 source prim that lives **outside**
the groom cannot be pruned without vanishing from the outliner, so the index instead overlays
`visibility/visibility = false` on it (`pxr/imaging/hd/visibilitySchema.h:35-39`, `GetVisibility()`
at `:73`) — one reversible locator that shows in the property panel. Guides, freezes and imports
therefore never draw twice (ADR §9 R29).

**S5's bare-`primvars` rule applies to every overlay the index makes on an upstream prim**, not only
to `points`: the `visibility` overlay above, the CV-display child of §7.3, and any primvar the index
adds all emit `HdContainerDataSourceEditor::ComputeDirtyLocators` **and** the bare `primvars`
locator (ADR §5.2, ADR §9 R29, §4.7). Without the bare locator the resolved upstream prim freezes —
the measured usdRig/UsdSkel interop bug S5 records.

An **in-place `points` overlay on a frozen prim** — publishing the deformed result onto the source
prim instead of onto tiles, so the topology never re-uploads and Storm keeps its VBO — is a **v2**
optimisation behind gate **S-10** (T2, M8: PrimsRemoved/Added versus an in-place points dirty on a
frozen prim; ADR §9 R29, R38, R40). It is out of scope here (§10).

One consequence of the always-tiles rule belongs to the look, and it is why §1.2's "no `normals`"
bullet points here. ADR §5.4 variant A (tangent from Storm's own `inData.Neye`, no `hairTangent`
primvar) is licensed "on usdGen tiles because the SI pins refineLevel 2, always publishes `widths`,
and binds the shader only to its own tiles". None of those three preconditions holds on a curve prim
usdGen does **not** own — a guide set drawn in a wire or mesh context today, an in-place overlay in
v2 — so any such prim takes **variant B** (`UsdGenHairPreviewPrimvar`, ADR §9 R4) with a published
`hairTangent`, at a cost of ≈ +1.5–2.5 ms per deforming frame at 100 k × 8 CV — **DERIVED**, the
figure `09-performance-and-benchmarks.md` §0.2's variant-A precondition carries, produced by gate
**S-8**.
`07-look-maps-expressions.md` §2.1 lists the three shipped glslfx files and its §2.5 owns the tangent
policy (ADR §5.4, ADR §9 R4, gate S-8).

### 5.8 Staleness in the loop

Staleness is checked in one place, on a string compare of a **constant primvar** — the only channel
that both reaches Hydra and dirties precisely (MEASURED, freeze report §2.3). The dirty classes are
the per-locator table of §4.7.1, which is normative and is not restated here. The three rows that
matter to the loop: a `primvars/usdGen:frozenEpoch` dirty means *re-check staleness* and nothing
else — no dirty bit, no re-capture, no re-evaluation; a `basisCurves/topology/*` dirty means the id
mapping may have changed and every downstream `UsdGenSculptLayer` must re-match by `curveId` (§6.3);
and a `primvars/points` dirty on the frozen prim — what the comb brush produces, 0.02–0.13 ms with no
`_resetPrimView` (MEASURED, freeze report §4.1, §4.3) — is `UsdGenDirtyParameter`, invalidating only
the deform-transport tail of its dependents.

### 5.9 Measured cost of one freeze

| Step | 10 k | 100 k | 1 M | Source |
|---|---:|---:|---:|---|
| author into the session layer | 0.25 ms | 0.27 ms | 0.27 ms | MEASURED, **EV-041** |
| `UsdImagingStageSceneIndex::ApplyPendingUpdates` | 0.11 ms | 0.13 ms | 0.13 ms | MEASURED, component of **EV-045** |
| terminal `GetPrim` | 0.001 ms | 0.001 ms | 0.001 ms | same |
| first full data pull | 0.19 ms | 0.19 ms | 0.20 ms | same |
| **edit → pulled, total** | **0.55 ms** | **0.59 ms** | **0.59 ms** | MEASURED, **EV-045** |
| points-only edit → apply | 0.06 ms | 0.08 ms | 0.13 ms | MEASURED, freeze report §4.1 (no ledger row) |

`_ApplyPendingResyncs` coalesces resync paths by prefix and re-walks only that subtree with
`_PopulateSubtree` (`pxr/usdImaging/usdImaging/stageSceneIndex.cpp:635,686`), which is why the cost is
flat in curve count: a data source hands back the retained `VtArray`.

**What is still UNMEASURED**: the GPU cost of the `PrimsRemoved` + `PrimsAdded` pair the resync emits
— Storm destroys the rprim and reallocates every VBO. The freeze report's §5 is the protocol; it runs
at tier T2 on the EGL harness in M2 and at tier T4 on a workstation. Until it runs, "a freeze is
sub-millisecond" is true only up to the delegate boundary. Gate **T-4** covers the app half
(one freeze on a 2 205-prim stage ≤ 5 ms author + exactly one `_resetGUI`; the MEASURED baseline is
**EV-050**'s 3.45 ms at 2 205 prims, and `_resetGUI` itself is excluded from the threshold,
**EV-051**).

---

## 6. `UsdGenSculptLayer`

### 6.1 The prim

| Property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:input` | `rel` | — | one upstream node |
| `usdGen:sculpt:weight` | `float` | `1.0` | layer weight `[0,1]`, XGen sculpt-layer semantics |
| `usdGen:sculpt:space` | `uniform token` | `"rootFrame"` | `rootFrame` \| `object`. `rootFrame` is what makes a delta survive surface deformation; `object` is the escape hatch for a fix that must not follow the surface |
| `usdGen:sculpt:curveIds` | `uint64[]` | `[]` | which curves have deltas, **sorted ascending**; 64-bit, matching `primvars:usdGen:curveId` (ADR §9 R12) |
| `usdGen:sculpt:cvOffsets` | `int[]` | `[]` | prefix offsets into `deltas`; size `curveIds.size() + 1` |
| `usdGen:sculpt:deltas` | `vector3f[]` | `[]` | per-CV delta **in that curve's root frame** |
| `usdGen:sculpt:epoch` | `uniform string` | `""` | the epoch the deltas were authored against |
| `usdGen:sculpt:lockedCurves` | `uint64[]` | `[]` | Freeze-brush ids: downstream stylers are zeroed for these curves |
| `usdGen:sculpt:rootPrims` | `int[]` | `[]` | **optional**, parallel to `curveIds`: the face each delta's curve was rooted on |
| `usdGen:sculpt:rootUVs` | `texCoord2f[]` | `[]` | **optional**, parallel to `curveIds`: the root UV |

`usdGen:sculpt:rootPrims (int[])` and `usdGen:sculpt:rootUVs (texCoord2f[])` are **ratified by
ADR §9 R8**, and both `02-schema.md` §2.10 (the normative registry, ADR §9 R7) and
`04-operators.md` §2.7 now declare them.
They are required by a feature the ADR itself mandates — "Rebase sculpt (re-match by
nearest root UV)" — because without a record of where each delta's curve *was* rooted a rebase has
nothing to match against once the old topology is gone. The values are the ones the engine already
holds per curve (`UsdGenCurveBuffer::rootPrim`, `rootUV`, `03-execution-engine.md` §1.2); the tool
writes them at commit. When they are absent the "Rebase sculpt" action is greyed out with a tooltip
naming the reason.

### 6.2 Evaluation

For each curve `c` whose id is at index `j` of `curveIds`, with `n = cvOffsets[j+1] - cvOffsets[j]`
deltas:

```
w      = usdGen:blend * usdGen:sculpt:weight
F(c)   = the curve's current root frame (rest frame above UsdGenDeform, animated frame below it)
P[i] += w * R(F(c)) * deltas[cvOffsets[j] + i]        for i in [0, min(n, cvCount(c)))
```

`usdGen:blend` is in `w` because every `UsdGenOperator` carries the envelope and
`04-operators.md` §5.2's mask arithmetic already multiplies it in
(`w(c, i) = usdGen:blend * curveMask[c] * rampLUT[…]`). `04-operators.md` §2.7 writes the same
evaluate step with `weight = usdGen:blend * usdGen:sculpt:weight`.

With `usdGen:sculpt:space = "object"`, `R(F(c))` is the identity and the deltas are applied in the
prim's own space. Only the rotation part of `F` is applied in either space: the deltas are offsets,
not positions. **`object` is placement-dependent, and it is easy to get wrong.** *Below*
`UsdGenDeform` an object-space delta genuinely does not follow the surface, which is what a
shot-specific fix wants. *Above* it the delta is baked into the rest pose and is then transported by
§4.3's `M(c) = R_anim · transpose(R_rest)`, so it follows the surface after all. An `object`-space
layer that must not follow the surface therefore has to sit in the deformed tail, and the compiler
warns when an `object`-space `UsdGenSculptLayer` is upstream of a `UsdGenDeform`.

With the default `rootFrame` space the deltas live in the curve's own root frame, so a delta set
authored on a T-pose scalp follows the head through any deformation without a rebase — that is the
entire reason for the root-frame convention (A7 §9.3).

If `n != cvCount(c)` (a `UsdGenResample` or a CV-count edit ran between the authoring and now), the
deltas for that curve are **retained and ignored**, the layer is marked stale, and the node reports
the count in `UsdGenNodeStats`. Deltas are never silently resampled: the artist decides.

Multiple `UsdGenSculptLayer` prims chained in sequence blend additively in chain order, each with its
own weight, exactly as XGen's sculpt layers do.

### 6.3 Sparse addressing

`curveIds` is sorted, so the layer's capture builds a `UsdGenSculptIndex`: a binary search from the
buffer's `curveId` array into `curveIds`, materialised once per capture epoch as a dense
`int32` per-curve slot array (`-1` = no delta). Evaluation is then one branch per curve with no
search. Cost is `O(totalCurves · log |curveIds|)` once per capture, and `O(affected CVs)` per frame.

Ids that no longer exist in the buffer are kept in the authored arrays and ignored (A7 §9.3): an
artist who scrubs density down and back up must not lose comb work.

### 6.4 Locked curves

`usdGen:sculpt:lockedCurves` is the Freeze brush: the named curves are excluded from every styler
*below* this layer. **No new buffer field implements it.** The sculpt layer's `Capture()`
materialises `lockedCurveSuppression(c)` (0 or 1) and every downstream operator's mask resolution
multiplies it in, in the shape `04-operators.md` §5.2 specifies:

```
curveMask[c] = clamp(base(c) * rnd(c) * nse(c) * lockedCurveSuppression(c), 0, 1)
```

The suppression term is **inside** the clamp. ADR §9 R16 makes 04 §5.2's pseudocode the executable
form and requires it to be arithmetically identical to `02-schema.md` §2.13's block, so no document
may move the term outside. The
existing per-curve `VtFloatArray curveMask` in `UsdGenCurveBuffer` (`03-execution-engine.md`
§1.2) carries it; there is no `lockMask` field and no extra pass, because the mask is already
resolved once per node per capture. One mechanism implements XGen's Freeze brush for every
downstream styler at once.

### 6.5 Epoch, staleness and rebase

`usdGen:sculpt:epoch` is compared against the epoch of the node the layer reads. Mismatch means the
upstream topology or capture changed since the deltas were authored:

* The stack editor badges the layer and the status line names the first mismatching term.
* Evaluation continues — deltas whose ids still exist still apply. Silence is worse than a stale
  badge, and dropping the artist's work is worse than both.
* **"Rebase sculpt"** (one undoable action in the tool) re-keys the deltas: for each entry it looks up
  `(rootPrims[j], rootUVs[j])`, takes the nearest current curve root on that face — or, when the face
  is gone, the nearest root in rest 3-space through the same `UsdGenBindingCache` kd-tree as §3.4 —
  and rewrites `curveIds`, `rootPrims`, `rootUVs` and `epoch` in one `Sdf.ChangeBlock` inside a
  `SubtreeSnapshot` bracket. Entries with no match inside a tool-set radius are dropped and reported
  by count. Rebase is **v2** (ADR §6); v1 ships the badge and the retain-and-ignore behaviour.

### 6.6 Interaction with density decimation

`usdGen:densityScale` and `usdGen:renderDensityScale` decimate **by stable id**, and the two session
contexts are **exclusive** (ADR §9 R13): `scale` is `usdGen:densityScale` in the **interactive**
context and `usdGen:renderDensityScale` in the **render** context, neither applies in the other, and
both default 1. The predicate is

```
keepFraction = clamp(scale_groom * scale_description, 0, 1)      # values above 1 clamp with one TF_WARN
keep(c)      iff  UsdGenHash32(curveId(c), kSaltDensity) < keepFraction * 2^32
```

with `UsdGenHash32` and `kSaltDensity ≠ 0` exactly as §2.4 pins them (ADR §9 R12, R13). Scales never
re-scatter and never re-chunk: the chunk partition is computed once per capture over the **full** id
set and `UsdGenChunkDesc::liveCount` tracks the survivors (`03-execution-engine.md` §1.2). Three
properties follow, and they are the reason decimation is id-based rather than "drop the tail of each
face's list":

1. A curve that survives at `keepFraction = 0.25` also survives at every larger fraction, so
   scrubbing density never re-shuffles which hairs are present.
2. Sculpt deltas, clump ids and locked-curve ids are keyed by the same ids, so they survive every
   density change untouched.
3. During an interactive density **drag** the element counts do not change at all: curves above the
   live count are parked (CVs collapsed to the root, `widths` set to 0) and only `points` and
   `widths` are dirtied, because any element-count change reallocates the whole aggregated VBO and
   bumps its version (the live `#else` branch, `pxr/imaging/hdSt/vboMemoryManager.cpp:604-618`;
   MEASURED, `research/G-storm-throughput-and-prim-granularity.md` §1.6). Real counts commit on release; gate
   **S-7** bounds a parked frame at ≤ 1.5× a points-only frame.

Parked curves keep their ids, so a stroke made mid-drag lands on the curve the artist aimed at.

### 6.7 Where deltas come from

The brush loop is `08-tools.md`'s subject; two of its properties belong here because they constrain
this schema:

* During a drag there is **no stage traffic**: the tool sets a live override in the C++ registry and
  the session publishes a generation with only the touched leaves dirtied (S40); one stage write at
  release, inside an undo bracket. Python cost per move is **21.3 µs** (0.34 µs zero-copy read +
  16.6 µs numpy kernel over 2 000 CVs + 4.1 µs sparse push) against 309 µs in pure Python (MEASURED,
  **EV-061**, `research/G-tool-loop-array-transport-and-cv-picking.md` §1.4). Gate **T-1** bounds a
  move at ≤ 1 ms of Python plus engine at 100 k curves.
* The release writes **deltas in the root frame**, not points, so the same stroke survives a later
  surface animation. The conversion is

  ```
  delta[i] = transpose(R(F_anim(c))) * (P_new[i] - P_old[i])
  ```

  — **always the animated root frame**, whichever side of `UsdGenDeform` the layer sits on, because
  `P_new - P_old` is always measured in the deformed pose the artist is looking at. Above the deform
  this is still correct: §4.3 transports a rest-frame delta by `M(c) = R_anim · transpose(R_rest)`,
  so `transpose(R_anim) · d` reappears on screen as `R_anim · transpose(R_anim) · d = d`. Below it
  `F_anim == F(c)` and the two forms coincide. Writing `transpose(R_rest)` instead lands the stroke
  rotated by `transpose(R_rest) · R_anim` — invisible at the rest pose, which is where a developer
  would test it, and visibly wrong on every animated frame. `testUsdGenSculptRoundTrip` (§9, T0)
  asserts the round trip **at a non-rest frame**. The conversion runs in C++, where both frames
  already exist.

CV picking for the brush is CPU in C++: 166 µs at 100 k CVs and 1.66 ms at 1 M, ≈8× cheaper than a
Hydra pick (MEASURED, **EV-064**), with BLAS threads pinned to 1 — unpinned, the numpy equivalent
inflates 10–20× on a loaded host (**EV-065**). Gate **T-2** bounds it at 0.25 / 2.5 ms.

---

## 7. Guides as curves

### 7.1 `UsdGenGuideSet`

A `UsdGenGuideSet` (a `UsdGeomImageable`, ADR §2.1) is a named set whose children are `BasisCurves`
under contract C3 with `primvars:usdGen:role = "guide"`. It lives at `<Description>/Guides/<setName>`
(ADR §2.2). Guides are, mechanically, a freeze of a sparse subset — XGen's "create guides at 10 %
density" — and they load through exactly the §2 path with `usdGen:lane = "reference"`.

| Property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:blend` | `float[]` | `[]` | **per guide**, XGen's range-of-influence blend (ADR §2.3), indexed by the child curves' `primvars:usdGen:curveId` in ascending order. Empty = 1.0 for every guide. Array-typed, and a different property from the scalar `usdGen:blend` envelope on `UsdGenOperator`: a `UsdGenGuideSet` is a `UsdGeomImageable`, not an operator, so the two never appear on one prim |
| `usdGen:surface` | `rel` | — | overrides the Description's surface for this set's rest binding |

Per-guide `usdGen:blend` scales that guide's weight after normalisation in `GuideInterpolate`, which
is how an artist damps one guide's influence without moving it.

### 7.2 Guides in the reference lane

Guides are the canonical **reference lane** input (I3, ADR §4.1): hair chunks never read other hair
chunks; guides, clump centres and card roots are **un-chunked reference buffers evaluated to
completion first**, and consumers store `guideIdx[3]` / `guideW[3]` at capture. Consequences for this
document:

* A guide set is evaluated in full before any dependent chunk runs. Its own chain (guides may
  themselves be styled) is a small graph evaluated in the same commit.
* `UsdGenGuideInterpolate` emits `guideIndex` (`int[]`, `uniform`, `elementSize = 3`) and
  `guideWeight` (`float[]`, `uniform`, `elementSize = 3`), matching Unreal's
  `groom_closest_guides` / `groom_guide_weights` arity so a bake round-trips (ADR §2.3, ADR §9 R24,
  A7 §2.1). They land on the tiles under contract C2 as `primvars/guideIndex` and
  `primvars/guideWeight` — no `usdGen:` prefix (`06-imaging.md` §4.1). `int[3]` / `float[3]` are not
  USD type names.
* Guide capture is a kd-tree over guide roots in rest space, the same structure as §3.4. Gate **E-4**
  is the measurement (100 k roots, 4 k guides, 8 threads, ≤ 25 ms and linear in roots), run as M0
  pre-work item **PW-1** (`11-roadmap.md` §1) and binding at M3.

### 7.3 Guide display prims

Guides are drawn, so the hair scene index synthesizes two prims per set under
`<Description>/__usdGenRender/guides/` (ADR §2.2):

| Path | Type | Purpose |
|---|---|---|
| `guides/<setName>` | `basisCurves` | the guide curves themselves, `purpose = "guide"` so they are hidden in a render view but visible in the viewport |
| `guides/<setName>/cvs` | `points` | CV display for the brushes: `points`, `widths` and `displayColor`, published only while a brush that needs CVs is active |

Both carry `primOrigin{scenePath}` — a picked prim without it yields an **empty** `hitPrimPath`
(`pxr/imaging/hdx/pickTask.cpp:1277-1317`, MEASURED in the tool-loop report), so the artist could not
select a guide at all. CV *highlighting* through Hydra selection is unreachable from usdview
(`HdSelectionSchema` carries only `fullySelected` and `nestedInstanceIndices`), which is why CV
display is a synthesized `points` child prim and not a selection state (S40).

### 7.4 Producing guides

Three routes, all landing in the same C3 shape:

1. **Freeze a sparse groom** into `<Description>/Guides/<setName>/` with
   `primvars:usdGen:role = "guide"` — the §5 write path with one changed primvar.
2. **Import** any C3 prim carrying `primvars:usdGen:role = "guide"` under a `UsdGenGuideSet`.
3. **Plant by hand.** The place-guide brush authors one curve per click, `skinprim`/`skinprimuv` from
   `UsdGenImaging_ClosestSurfacePoint`, `rest` equal to `points`; one gesture is one
   `Sdf.ChangeBlock` and therefore one `_resetGUI` (§5.5, item 7).

---

## 8. Performance expectations, by path

### 8.1 The paths

| Path | Expectation | Status | Gate |
|---|---|---|---|
| USD → Hydra half of a freeze (author → pulled) | 0.55–0.59 ms, **flat in curve count** to 1 M | MEASURED, **EV-045** | T-4 (app half) |
| Reopening a `sublayer` sidecar and reading `points` | 0.4–0.8 ms at 800 k CVs — **a cold reopen of the file, not the arc**; composing the sublayer arc itself is 0.2 ms plus a 0.1 ms first `points.Get()` (§5.4) | MEASURED, **EV-047** (reopen), **EV-049** (compose) | — |
| `payload` composition | 21.5 ms | MEASURED, **EV-049** | — |
| Undo of a live freeze (`SetActive(false)`) | 0.02 ms at any curve count | MEASURED, **EV-044** | T-4 |
| `SubtreeSnapshot` capture / undo / redo | 0.10 / 0.23–0.24 / 0.08–0.09 ms, zero RSS growth | MEASURED, **EV-040** | T-4 |
| usdview GUI cost per freeze | O(stage prims): 4.28 / 35.4 / 105.5 ms of `_resetGUI` at 115 / 2 205 / 11 005 prims, of which `_resetPrimView` is 2.64 / 23.0 / 102.2 ms | MEASURED, **EV-051** | T-4 (exactly one `_resetGUI`; the walk itself is excluded from the threshold) |
| Loading a C3 source into the arena, 800 k CVs | scene-index pull 0.19 ms + one SoA transpose pass ≈ 0.25 ms (9.6 MB in, 9.6 MB out) | pull MEASURED (**EV-090**); transpose **DERIVED from EV-074** (0.49 ms per 19.2 MB `VtArray` pass) | E-1r |
| **Deform tail per frame at 100 k × 8 CV** | 0.4–0.6 ms | **DERIVED from EV-015/EV-016** (0.158–0.545 ms styler pass) **and EV-008** (1.02 ms 5-node chain at 8 threads); the row is `09-performance-and-benchmarks.md` §0.2's | E-1, E-3 |
| SoA → AoS interleave of all dirty tiles, 800 k CVs | 0.25 ms (9.6 MB in, 9.6 MB out), the same figure `09-performance-and-benchmarks.md` §0.2 carries | **DERIVED from EV-074** (0.49 ms per 19.2 MB pass); a strided three-stream gather is not a memcpy, so it is a floor | S-2 |
| Per-tile extent, **49** tiles at 100 k (ADR §9 R21, `03-execution-engine.md` §1.4) | 0.05 ms, folded into the interleave | **ASSUMPTION** (min/max fused into the interleave loop), as `09-performance-and-benchmarks.md` §0.2 tags it | S-2 |
| Publish + notice emission, **49** tiles | 0.02 ms | **DERIVED**; `09-performance-and-benchmarks.md` §0.2 owns this row and its derivation (ADR §9 R41) | SI-2 |
| Pruning-wrapper surface read per deforming frame, production-density skinned scalp | — | **UNMEASURED** | **SI-9** |
| **usdGen total on a deform frame** | 0.8–1.0 ms | **DERIVED** aggregate of the five rows above, quoted from `09-performance-and-benchmarks.md` §0.2, the only frame ledger (ADR §9 R41) | S-2 (≤ 2.0 ms delta over static) |
| **Ragged chunks vs uniform** | ≤ 2× | UNMEASURED | **E-1r** |
| First frame after `frozen -> live` (unfreeze) | one sub-graph recompile **plus a full capture of every re-admitted node** (§5.2) | UNMEASURED — bounded below by E-1's **≤ 1.5 ms** chain (ADR §9 R27; MEASURED 1.02 ms at 8 threads, **EV-008**) and above by the head's capture terms (`03-execution-engine.md` §4.1) | E-1, E-4, E-6 |
| First frame after `live -> frozen` (re-freeze) | one load, 0.19–0.20 ms, plus the tail | MEASURED (load), **EV-090** — the pull term of **EV-045** | — |
| P2 motion at k offsets | `k·T + (k−1)·m`, `m ≈ 1.0 ms` per 1.6 M points | `m` MEASURED (**EV-071**…**EV-073**), `T` UNMEASURED | R-1 |
| Binding computation, 100 k roots | kd-tree + kNN + patch solve | C3 cold 8.599 ms triangles / 15.708 ms bilinear quads over 4096 faces, 8-way arena; detailed workload/limits in plan 15 | E-4 (GuideInterpolate workload remains separate) |
| Brush move, Python + engine | 21.3 µs Python (**EV-061**) + 0.035–0.044 ms engine (**EV-002**, the 1 %-sparse chunk re-run at 20 threads) | MEASURED | T-1 |
| CV pick, 100 k / 1 M CVs | 166 µs / 1.66 ms | MEASURED, **EV-064** | T-2, T-3 |
| GPU cost of a freeze's `PrimsRemoved`+`PrimsAdded` | — | UNMEASURED; the protocol is freeze report §5 | tier T2 in M2, tier T4 at release |

### 8.2 The reading

usdGen owns **5–6 % of a 100 k-curve frame**. The share is read straight off
`09-performance-and-benchmarks.md` §0.2, the only frame ledger (ADR §9 R41): usdGen's total on a
deform frame is **0.8–1.0 ms** of a **15.8–16.0 ms** frame total. It is **DERIVED**, and worth
spelling out because both terms are derived: usdGen's own figure is an aggregate of five derived
rows, and the ledger's largest term — the ~12.2 ms Storm draw — is itself interpolated between a
MEASURED 5.01 ms at 40 k (**EV-020**) and 23.93 ms at 200 k (**EV-021**), which gate **S-1**
re-measures at 100 k. Never publish a share computed against the draw row alone; §0.2's rows are the
only source (ADR §9 R41).

The deform path is memory-bound: at 800 k CVs the transport kernel reads 9.6 MB of rest points,
writes 9.6 MB, and the interleave touches both again. Hence planar SoA per CV (25.3 → 80.9 GFLOP/s
for the same kernel, MEASURED, **EV-014**/**EV-016**, S22), AoS per curve for the root frames
(36 B/curve, read once), and a
private `tbb::task_arena` at the measured 8-thread knee: 3.90 ms at 1 thread → **1.02 ms at 8** →
1.79 ms at 20 (MEASURED, **EV-008**). That 1.02 ms is E-1's baseline and E-1's threshold is
**≤ 1.5 ms** (ADR §9 R27), which is what `09-performance-and-benchmarks.md` §5.1 registers.

The freeze path is not memory-bound at all: it is flat in curve count to 1 M (**EV-045**) because a
data source hands back a retained `VtArray`; the only scaling term is `RemovePrim`'s `free()`
(**EV-044**), which §5.6 moves off the interactive path.

---

## 9. Testing

Tiers are T0–T4 (§0.3). Every test named here is a CI test. Gate ids, their tiers and their milestone
exits come from `09-performance-and-benchmarks.md` §5, the single gate registry, as reconciled by
ADR §9 R40 — the **Exits** column below is that assignment, not a claim this document makes. The
gates in these tables that **exit M2** are **E-1r**, **E-3**, **SI-9**, **S-2** and **S-3**; the rest
exit other milestones and are listed here only because this document's code is what they exercise.
No T4 gate is a milestone exit; T4 gates are release criteria (ADR §7).

`10-build-dependencies-testing.md` §5.6 is the CTest name registry and
`09-performance-and-benchmarks.md` §5 names the driver for every gate. Rows below that carry a gate
id use that driver's name; rows with no gate are tests this document mints, registered in
`10-build-dependencies-testing.md` §5.6.

**T0 — pure engine, no stage, no Hydra** (possible because the engine input is `UsdGenGraphDesc`):

| Test | Asserts | Gate | Exits |
|---|---|---|---|
| `testUsdGenCurveLoader` | the §2.3 ladder, all twelve rows, on synthetic `UsdGenSourceArrays` | — | M2 |
| `benchUsdGenChain --ragged` | ragged vs uniform produce identical points for the same curves; ragged run ≤ 2× uniform at 100 k × 8 | **E-1r** | M2 |
| `testUsdGenRootFrame` | frame orthonormality; n-gon `(f, uv)` round-trip to 1e-5; degenerate-tangent fallback | — | M2 |
| `testUsdGenTransport` | `blend = 0` and `blend = 1` are bitwise exact; rigid transport of a rigidly moved surface reproduces the input exactly | — (the `usdGen:blend` exactness rule, `04-operators.md` §0.5) | M2 |
| `testUsdGenSculptRoundTrip` | a stroke displacement `d` converted by §6.7 and re-applied by §6.2 reproduces `d` **at a non-rest frame** as well as at rest, for a layer on either side of `UsdGenDeform` | — | M2 |
| `testUsdGenSculptIndex` | sparse match by `uint64` id; retain-and-ignore for missing ids; `n != cvCount` marks stale without applying | — | M2 |
| `testUsdGenFreezeSpaceRule` | §5.5 Rule 0: a `UsdGenFreeze` whose input resolves to a `deformedSpace` node is a compile error naming the freeze and the first `deformedSpace` node above it; a freeze on the last `restSpace` node compiles | — | M2 |
| `testUsdGenHash` | `UsdGenHash32(key, salt)` matches the pinned SplitMix64 form (ADR §9 R12) bit for bit at fixed vectors; `hairId` (salt 0) stays uniform within 2 % after decimation at `keepFraction = 0.25` with `kSaltDensity ≠ 0` (§2.4, §6.6) | — | M2 |
| `testUsdGenEpoch` | the epoch changes for each of the four terms in §5.3 and for nothing else | — | M2 |
| `testUsdGenKernelDeterminism` | 1 vs 8 threads, `-ffp-contract=off`, bitwise identical | **E-8** | M1, re-run M4 |
| `benchUsdGenChain`, `benchUsdGenSparse --terminal`, `benchUsdGenMemory` | 5-op chain at 100 k × 8 **≤ 1.5 ms** at 8 threads (ADR §9 R27; MEASURED 1.02 ms, **EV-008**); last-param edit ≤ 0.6 ms; RSS at 1 M ≤ 800 MB | **E-1, E-3, E-5** | M1 / M2 / M3 |
| `benchUsdGenKnn` | kNN over 100 k / 1 M rest roots at 8 threads, ≤ 25 ms and linear | **E-4** | M0 pre-work (PW-1), binding at M3 |

**T1 — headless scene index over the real `UsdImagingCreateSceneIndices` chain** (sub-100 ms, no GL):

| Test | Asserts | Gate | Exits |
|---|---|---|---|
| `testUsdGenFrozenReentry` | a C3 prim read from the hair index's **input** yields `points`, `rest`, `skinprim`, `skinprimuv`, `usdGen:curveId` with correct interpolations, and no `UsdStage` is touched | — | M2 |
| `testUsdGenC3Contract` | every C3 primvar survives to the terminal index with its interpolation and **width** — `usdGen:curveId` arrives as `uint64[]`, not `int[]` (ADR §9 R12); declared-but-unauthored primvars are skipped by value, not by presence | — | M2 |
| `testUsdGenTileContract` | `points.size() == Σ curveVertexCounts` on every published tile | **SI-1** | M1 |
| `testUsdGenInvalidation` | a surface `primvars/points` dirty produces exactly `primvars/points/primvarValue` + `extent/*` on the dependent tiles and nothing else; an **overlaid upstream prim** (the `visibility` overlay of §5.7, the CV child of §7.3) additionally emits the bare `primvars` locator | **SI-2**, S5 | M1, re-run on overlaid prims at M2 |
| `testUsdGenSkelInterop` (`10-build-dependencies-testing.md` §5.6) | a UsdSkel-skinned scalp drives the groom through the private pruning wrapper **with `HD_ENABLE_SCENE_INDEX_EMULATION` off**; a `skinningXforms` dirty re-runs the tail; the wrapper's per-frame cost on a production-density skinned scalp. **Stop condition SC-13 as requested** (§4.2 case 2): red ⇒ fall back to A2 §8 option (a) and re-plan M2 | **SI-9** | M2 |
| `testUsdGenAdapter` | every `usdGen:*` property of `UsdGenCurveSource`, `UsdGenDeform`, `UsdGenFreeze`, `UsdGenSculptLayer`, `UsdGenGuideSet` and `UsdGenRestAPI` appears in Hydra and dirties | **SI-7** | M1 |
| `testUsdGenFreezeEpoch` | epoch mismatch on a `UsdGenFreeze` badges and **keeps rendering the frozen data** (§5.3, ADR §9 R18); each `staleAction` on a `UsdGenCurveSource` behaves per §2.3 row 11; a `primvars/usdGen:frozenEpoch` dirty re-checks staleness and sets no dirty bit | — | M2 |
| `testUsdGenSourceDirtyRouting` | the full §4.7.1 locator table: `primvars/points` routes as `UsdGenDirtyParameter`, `primvars/rest` and the binding primvars as `UsdGenDirtyCapture`, `basisCurves/topology/*` as `UsdGenDirtyTopology`, all through the router's reverse map with no traversal and no `GetPrim` cook | — (supports SI-2, SI-3) | M1 |
| `testUsdGenScheduling` | 10 interactive edits produce exactly 10 commits, **all on the thread that committed**, with zero cooks inside `GetPrim` (the full SI-3 assertion) | **SI-3** | M1 |
| `testUsdGenMotionSamples` | P2 evaluates `k · tail`, not `k · chain`; `forwardSurfaceSamples` toggles the offset list; `velocities` are blocked outside P1 | — (the T1 half of **R-1**, whose T4 counterpart is the gate) | release |

**T2 — Storm through the EGL harness** (`usdGenTestUtils`/`eglctx.h`, on the GB10):

| Test | Asserts | Gate | Exits |
|---|---|---|---|
| `benchUsdGenStorm --deform` | deform frame ≤ 2.0 ms delta over static at 100 k × 8, **49** tiles (ADR §9 R21; `09-performance-and-benchmarks.md` §0.2 and gate S-2 already carry 49) | **S-2** | M2 |
| `benchUsdGenStorm --onetile` | one dirty tile of 49 ≤ 1.2× static | **S-3** | M2 |
| `benchUsdGenStorm --deform` (counters) | `vboRelocated == 0` across 100 deform frames | **S-6** | M1 |
| `benchUsdGenStorm --scrub` | a parked-CV drag ≤ 1.5× a points-only frame | **S-7** | M5 |
| `benchUsdGenStorm --freezegpu` | the freeze report §5 protocol: ms from `SetActive(True)` to first presented frame, per curve count; whether a points-only edit avoids the topology re-upload. A new mode of the one Storm driver `10-build-dependencies-testing.md` §5.6 registers, not a new binary | — (fills an open question) | M2 |

**T3 — `testusdview`** (under the scratchpad Xvfb; CPU numbers only, never quoted as GPU numbers):

| Test | Asserts | Gate | Exits |
|---|---|---|---|
| `testUsdviewUsdGenFreeze.py` | one freeze on a 2 205-prim stage: ≤ 5 ms author, exactly one `_resetGUI`; undo by `SetActive(false)`; the edit-target rule of §5.6 | **T-4** | M5 |
| `testUsdviewUsdGenComb.py` | press / move / release / escape; ≤ 1 ms per move; one stage write at release; the stroke commits deltas in the root frame | **T-1** | M5 |
| `testUsdviewUsdGenPick.py` | CPU CV pick agrees with `view.pick()` except on occlusion | **T-3** | M5 |

**T4 — workstation protocol** (release criteria, never a milestone exit): the freeze report §5 GPU
protocol on non-NVIDIA drivers; hdPrman motion parity with 3 samples (**R-1**); MSAA/OIT quality at
1-px strand widths (**R-2**).

---

## 10. Out of scope

* **Simulation itself.** usdGen consumes sim caches (§2.6); it does not solve. `UsdGenSimSource` (a
  live solver node) is **v3** (ADR §6).
* **Collision and shrinkwrap.** `UsdGenCollide` is v3 (ADR §2.1, §6): it needs a per-frame spatial
  structure over deformed geometry and a two-pass gather/scatter node kind (ADR §4.2.5).
* **Wind and forces.** `UsdGenWind` is v3.
* **`Smooth(neighbours)`** — the cross-curve variant — is v3 for the same reason; `Smooth`
  along-curve ships in v1 (ADR §6).
* **Cosserat shape preservation** (`usdGen:preserveShape` non-zero) is v2.
* **`rbf` and `pointDeform` deform modes** are v2; only `rigidFrame` is implemented in M2.
* **Sculpt rebase** is v2 (ADR §6); v1 ships the badge, the retained deltas and the two optional
  root-record arrays that make rebase possible later.
* **In-place publication.** `usdGen:output (tiles|inPlace)` is dropped everywhere (ADR §9 R8) and the
  in-place overlay path does not exist in v1. Publishing the deformed result onto a frozen source
  prim instead of onto tiles is a **v2** optimisation behind gate **S-10** (T2, M8; ADR §9 R29, R38,
  R40). The description always publishes tiles (§5.7).
* **Freezing inside the deformed tail.** A frozen-deformed cache is an export re-entering through
  `UsdGenCurveSource`, never a `UsdGenFreeze` (§5.5, Rule 0 — a decision this document makes, folded
  into `02-schema.md` §2.9 and `08-tools.md` §4.1).
* **Deferred skinning** (`HD_ENABLE_DEFERRED_SKINNING=true`) is unsupported in v1 (§4.2, case 3).
* **Caches whose curve count animates** are rejected (§2.6, item 3).
* **Baking motion samples into a freeze** is permanently out (§1.5): 230 402 598 B at 100 k × 8 for
  24 samples (MEASURED, **EV-046**).
* **Third-party operator ABI.** `UsdGenOpRegistry` is internal in v1/v2 (ADR §3).

---

## 11. Sources

**Design.** `design/adr-v1.md` §1 (S23/S27 chunk≠tile, S18(c), S24, S29 `hairId` float, S36), §2.1–2.3
(type hierarchy, reserved layout, freeze/sculpt property tables, adapter coverage), §3 (contracts
C1–C5), §4.1–4.3 (I1–I8, the digest fix, ragged-in-v1, commit triggers), §5.2–5.4 (invalidation
discipline, tile contract, `hairTangent`), §6 (catalogue split), §7 (milestones, gates).
**ADR §9 (the addendum) is binding over every earlier section** and is cited throughout: R1
(I1–I8, retired landing-tier labels), R2 (gate families), R3 (`UsdGenRestAPIAdapter`), R4 (three
glslfx files), R7–R8 (02 is the property registry; the `UsdGenCurveSource` and `UsdGenSculptLayer`
fold-ins, and the drop of `usdGen:output`), R9 (no `world` token), R12–R13 (64-bit `curveId`, the
pinned hash and salts, the density predicate), R15 (`UsdGenRestAPI` on the parent `Mesh` only;
subset face indices), R16 (04 §5.2's mask arithmetic is the executable form), R18 (stale freeze =
warn and keep rendering), R21
(tile arithmetic: 49 tiles at 100 k), R23 (`UsdGenGraphDesc::curveSets`), R24 (`guideIndex` /
`guideWeight` with `elementSize = 3`), R25 (full-locator router
keys), R27 (E-1 ≤ 1.5 ms), R29 (always tiles; the visibility overlay; S-10 in v2), R38 (P0/P1/P2 in
v1 at M7), R40 (the gate registry and milestone exits), R41 (09 §0.2 is the only frame ledger),
R42–R43 (number tags; `EV-nnn` is the citation handle for every measured number), R45 (every
cross-reference points at a section that exists today). One decision here is **not** in the ADR and
is flagged where it is made: §5.5's Rule 0 (a freeze may only cap a `restSpace` node), folded into
`02-schema.md` §2.9 and `08-tools.md` §4.1. The registries settle the two ids this document leans
on: `09-performance-and-benchmarks.md` §5.2 registers SI-9 (the pruning-wrapper cost) and SI-10
(session arbitration, RK-09's request) as separate gates, and `11-roadmap.md` §5.2 mints SC-13 for
§4.2's UsdSkel-pickup stop condition.
`design/brief-v1.md` §1 (R1–R9), S1, S3–S5, S8–S14, S17–S18, S22–S32, S37–S46.

**Siblings this document defers to.** `02-schema.md` §2.6, §2.9, §2.10, §2.13, §2.15, §2.19, §5 and
§7.3 (the normative property registry and contract C3, ADR §9 R7); `03-execution-engine.md` §1.2,
§1.4, §1.6, §2.2, §3.1–§3.6, §4.1, §4.5, §5.1 and §6.3 (buffers, tiles, digests, the router, tile
assembly); `04-operators.md` §0.5, §0.6, §2.4, §2.7 and §5.2 (the envelope, the pinned hash, the
mask arithmetic); `06-imaging.md` §2.6, §3.5 and §4.1 (the rest adapter, the pruning wrapper, the
C2 tile contract); `07-look-maps-expressions.md` §2.1 and §2.5 (the three glslfx files and the
tangent policy); `08-tools.md` §1.4, §4.1 and §7.4 (the C ABI, the freeze bar, the landmine
containment); `09-performance-and-benchmarks.md` §0.2 (the only frame ledger) and §5 (the only gate
registry); `10-build-dependencies-testing.md` §3.5 (the single env-var registry, ADR §9 R35) and §5.6
(CTest names);
`11-roadmap.md` §2.3 and §5 (M2's exit criteria and the stop-condition register);
`12-risks-decisions-open-questions.md` §2 (RK-08's `Tf.Error.Mark` correction, RK-09's SI-10
request) and §2.1 (SC-11, SC-12); `appendix-A-evidence-ledger.md` §2 (every `EV-nnn` row quoted
here) and §3 (the file:line index).

**Proposals.** `design/proposal-performance.md` §8 (static curves, freezing and re-entry — the
primary source for §5), §4.2 (`UsdGenRestAPI`, digests), §4.6 (example (b)), §5.1 (chunk arena and
the ragged descriptor), §5.7 (density scrub), §5.8 (the motion tail), §6.3 (what a synthesized prim
carries), §11.2 (gates). `design/proposal-risk.md` §3.7 (freeze and sculpt), §3.8 (contract C3),
§3.11 (canonical example (b) and its load costs). `design/proposal-artist.md` §3.8 (the artist
reading of example (b): the freeze caps rather than replaces, deltas keyed by `curveId`, undo by
`SetActive(false)`).

**Judges.** `design/judge-evidence.md`, `design/judge-artist.md`, `design/judge-delivery.md` —
read as defect lists (the `GetPrim` backstop withdrawal, `hairId` type, chunk≠tile, the `VtArray`
handoff rule); their rejected ideas are not restated here.

**Research.** `research/G-freeze-bake-undo-and-frozen-reentry.md` §1.2–1.5 (SubtreeSnapshot fidelity
and cost, the OpenExec resync bug, the edit-target trap), §2.1–2.4 (the C3 dump — whose `int[]`
`curveId` predates ADR §9 R12 — re-entry without a stage, what usdImaging will not carry, and §2.4's
dirty-locator taxonomy, which §4.7.1 turns into the normative router table), §3 (landing tiers and crate
sizes), §4.1–4.3 (scene-index, blast-radius and usdview costs), §5 (the GPU protocol that is still
open). `research/G-motion-blur-sampling-strategy.md` §1.2, §4.1–4.5, §5 (profiles, offsets, cache
shape, the 19.2 MB/1.0 ms floors). `research/A2-usdrig-imaging.md` §8 (UsdSkel pickup, and option (a) — the
CPU fallback if the pruning wrapper does not resolve without emulation) and §9 (the
invalidation contract). `research/A7-prior-art-grooming.md` §3.5–3.7, §9.1 G7, §9.3 (Guide Deform,
freeze/sculpt semantics). `research/G-data-plane-engine-prototype-benchmark.md` §3.3 (thread scaling),
§4 (the 5-node TBB chain and the 1 %-sparse re-run), §5 (RSS at 1 M), §6 (the single-thread styler
pass and the SoA/AoS kernel table). `research/G-tool-loop-array-transport-and-cv-picking.md`
§1.4 and Key facts (transport costs, `Define` inside a change block, CV pick, `primOrigin`).
`research/G-storm-throughput-and-prim-granularity.md` §1.3 (upload granularity = the prim), §1.8
(per-prim per-frame costs and the dependency fan-out), §2 "Topology strategy" and Key facts
(exact-size arrays, the 6.48 ms varying-primvar expansion, element-count reallocation,
`DirtyPrimvar`, extent-driven culling). `research/G-stage-free-parameter-and-time-transport.md`
§3 (the rest data source at `Default()`). `research/G-chain-order-probe.md` §2–3 (placement).
`research/ENVIRONMENT.md` including the CORRECTIONS block (EGL harness, Xvfb, installed third party).

**Prototypes carried into M2.** `prototypes/freeze-bake/` (probe1 `SubtreeSnapshot`, probe4 frozen
re-entry through `UsdImagingCreateSceneIndices`, probe7 locator taxonomy, probe8 fidelity, probe10
resync blast, `uv/testUsdviewFreezeCost.py`), `prototypes/data-plane-benchmark/` (`tbbBench.cpp` →
the chunk arena and E-1/E-1r harness), `prototypes/tool-loop/` (array transport, CV pick),
`prototypes/storm-throughput/` and `prototypes/storm-hair-look/` (the EGL harness and `eglctx.h` for
tier T2).


**OpenUSD 26.08 sources** are cited inline by `file:line` throughout and every one was re-verified
by grep in `/home/burkard/work/OpenUSD/pxr` for this revision; `appendix-A-evidence-ledger.md`
carries the consolidated index. Three citations were corrected in the process and the corrections
matter to anyone chasing them: `basisCurvesAdapter.cpp` **:367-368** for the `pinned` branch (§1.1,
and it is the Hydra 1.0 adapter), `esfUsd/stageData.cpp` **:351 and :361** for the OpenExec resync
bug that S41/S46 and the freeze report quote as `:360` (§5.6), and
`hd/changeTracker.cpp` **:957-979** for `IsPrimvarDirty` (§4.7).
