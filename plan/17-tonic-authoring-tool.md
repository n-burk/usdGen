# 17 — hierarchical groom authoring tool for usdview (`usdGenTonic`)

Date: 2026-09-18. Status: **proposed overlay** (plan v2). The tool is specified
from the repository itself (`13-codebase-alignment.md`, `08-tools.md`,
`gpu/tools.h`, `gpu/picking.cu`, `ops/sculptLayer.cpp`, `plugin/usdGenTools`).
`usdGenTonic` is the in-tree module name.

This overlay supersedes `08-tools.md` wherever the two disagree about **when the stage is written**
(§3) and **what the tool authors** (§2). It leaves every other `08-tools.md` decision in force,
including the module split, the Qt-free rule, `UsdGenToolState`, the undo module shape, the test tiers
and the workstation protocol.

---

## 0. The request, and the three decisions it forces

> Build a usdview plugin that builds, models, and sculpts hair in the viewport.
> CUDA powered, and easy for artists to use. It creates data for usdGen.
> **Viewport-only authoring that does not commit data to the stage
> synchronously; commit to the UsdStage asynchronously and non-blocking; prioritise interactivity.**

### 0.1 What the authoring model is

The authoring model is a **hierarchical volume/tube groomer**, not a brush groomer. Its geometric contract:

1. A 2D graph authored on the 3D scalp; nodes are added, removed and dragged during grooming.
2. Every closed region of that graph is the root set of one **clump**.
3. A clump volume ("tube") is a **center curve plus a series of orthogonal planar cross-sections**.
4. Artists sculpt control vertices of the center curve and of the cross-sections.
5. Guide curves are generated at a **prescribed density**, filling the volume so they reflect the
   center profile and the tube extent.
6. Volumes must give complete scalp coverage, no root-level intersections, and sufficient smoothness.
7. Coarse-to-fine: clumps are subdivided. The hierarchy is **persistent** (children of
   coarse parents). Levels are named L1, L2, and L3.
8. A **hierarchical, length-preserving sculpt** deforms children when a parent control curve moves;
   parents are built **bottom-up by recursive averaging**; on-the-fly parents can be made from any
   curve subset, transiently or persistently.
9. Outputs: tubes, center curves, cross-sections, guides, region/clump maps, hierarchy annotations —
   consumed by sim (a guide subset) and by the procedural amplifier (a host groomer: noise, curl, clumping,
   render-time interpolation).
10. Reference scale: > 500 K tube vertices with interactive rendering and selection (~60× optimisation).

Explicitly **not** this tool (and therefore downstream, in usdGen operators): noise, curl,
clumping, dense interpolation, and brush-painted maps, plus SeExpr. The fill kernel, region-map
rasterisation, UI chrome, undo model and file formats are unpublished; everything below in those
areas is our own design and is labelled as such.

### 0.2 The three decisions

**D1 — The tool owns a live model; the stage is a write-behind target.** All authoring state (scalp
graph, tubes, hierarchy, sculpt deltas, fill parameters, selection) lives in `TonicModel`, a C++
object with GPU-resident buffers inside the plugin process. Interaction never authors USD. A
`TonicCommitter` serialises snapshots of the model to USD **on a worker thread** and swaps the result
into a tool-owned sublayer on the main thread in one `Sdf.ChangeBlock`, debounced and coalesced, and
only when the main thread is idle. The viewport never waits on `Sdf`, on `UsdImagingStageSceneIndex`,
or on a usdGen cook.

This inverts `08-tools.md` §2.1/§2.4 ("one write at release; the cook runs inside the repaint"). That
design is still the right one for **brush edits on usdGen-generated curves** (the M5 shelf). It is the
wrong one for a Tonic-style tool, where a single drag on a parent center curve can rewrite tens of
thousands of tube vertices and re-fill thousands of guides; a synchronous write plus a cook inside
the repaint would put the reference-scale groom on the UI thread.

**D2 — The tool renders itself through its own scene index, not through the groom cook.** A
`UsdGenTonicSceneIndex` (a second `HdSceneIndexPlugin`, `plugin/usdGenTonic`) publishes synthetic
prims under `/__usdGenTonic/` straight from `TonicModel`: scalp graph (points + curves), tube
surfaces (meshes), center curves, cross-section rings, guide previews (basisCurves), selection and
gizmo geometry. The published data is produced by CUDA kernels and reaches Storm through one of two
transports (§4.3): **staged host copy** now (device → pinned host → `VtArray`), **CUDA-GL interop**
later (`cudaGlComputation.h`, already behind `USDGEN_HAS_CUDA_GL_INTEROP`, needs the private
OpenUSD patches). The usdGen cook of the *committed* guides is a separate, lower-priority,
cancellable pass that the artist can toggle ("show amplified hair").

**D3 — The stage contract is the existing usdGen guide contract, plus a small tube schema.** The
tool's job "instead of a host groomer" is to emit exactly what usdGen's amplifier already consumes:
`BasisCurves` guides with `UsdGenCurveAPI` (`role = guide`) targeted by `UsdGenGuideInterpolate`'s
`usdGen:guides`, a region map targeted by its `usdGen:region`, and a `UsdGenDeform` driver set for
sim/anim. Tubes, scalp graph and hierarchy are stored alongside as new codeless prims
(`UsdGenTube`, `UsdGenScalpGraph`, §2) so the groom round-trips, but **no usdGen kernel reads them**;
they are authoring-side only, as Tonic's tubes are authoring-side to a host groomer.

---

## 1. Architecture

### 1.1 Components and where they live

| Component | Location | Language | Role |
|---|---|---|---|
| `usdGenTonic` (SHARED) | `libs/usdGenTonic/usdGenTonic/` | C++17 + CUDA | `TonicModel`, `TonicKernels` (CUDA), `TonicCommitter`, `TonicUndo`, C ABI `tonicApi.h`. Links `usdGen` (for `gpu/` primitives, `DeviceBuffer`, picking) and `sdf`/`usd` **only in the committer TU** (gate B-1 stays true for `usdGen` itself). |
| `usdGenTonicImaging` | `libs/usdGenTonic/usdGenTonic/imaging/` | C++ | `UsdGenTonicSceneIndexPlugin` + `UsdGenTonicSceneIndex`, tile publisher for tool geometry, transport (host-staged / GL interop). |
| `plugin/usdGenTonic/` | `plugin/usdGenTonic/{plugInfo.json.in, resources/}` | — | Scene-index registration (`loadWithRenderer ""`, phase 0, after the groom index). |
| `usdGenTonicTools` | `plugin/usdGenTonicTools/usdGenTonicTools/*.py` | Python (Qt lazy) | The usdview plugin: modes, shelf, panels, HUD, hotkeys, gizmos. Qt-free kernels in `tonicMath.py`; ctypes binding `tonicLib.py`; array transport through `_usdGen` (`08-tools.md` §1.5) once it exists, `ctypes` + numpy until then. |
| schema additions | `libs/usdGenSchema/schema.usda` | usda | `UsdGenScalpGraph`, `UsdGenTube`, `UsdGenTubeHierarchyAPI`, `UsdGenTonicGroom` (§2). Regenerate with `bin/gen_schema.ps1`; `restore_generated_metadata.py` runs after. |
| tests | `tests/testUsdGenTonic*.{cpp,cu,py}` | — | Tiers T0/T1/T3 (§8). |
| examples | `examples/tonic-*.usda` | usda | Round-trip scenes (§8.4). |

The existing `plugin/usdGenTools` (SeExpr editor, value preview) stays as is. The new plugin
registers a sibling **usdGen → Tonic** menu and a dockable workspace; both plugins share
`UsdGenToolState` conventions from `08-tools.md` §1.3.

### 1.2 Threads

| Thread | Owns | Never does |
|---|---|---|
| UI / main (Qt + usdview) | input, `TonicModel` mutation requests, `UpdateViewport()`, the **swap** step of a commit (one `Sdf.ChangeBlock`, bounded, idle-scheduled) | serialisation, cooks, waits on CUDA |
| Tonic compute (one CUDA stream, `tonicStream`) | every kernel in §4; produces the next display snapshot | stage access |
| Tonic commit worker | model snapshot → anonymous `SdfLayer` | Qt, `UsdStage`, waiting on the bake |
| Tonic bake worker (+ `bakeStream`, lowest CUDA priority) | dirty-face texel rasterisation, D2H, Ptex file write, old-version cleanup | Qt, `UsdStage`, `tonicStream`, gating any swap |
| usdGen commit thread (existing, `sessionCooker`) | cooks of committed guides when "show amplified hair" is on | blocks the above |

The compute stream is **caller-serialised per gesture** like `CudaToolSession`, but it is a different
stream and a different engine object: Tonic geometry is not a usdGen generation. The two coexist
because `CudaToolSession` edits usdGen generations and `TonicModel` edits tubes; a Tonic guide edit
becomes a usdGen edit only through the committer.

### 1.3 The interaction contract (unchanged in spirit from `08-tools.md` §2.1)

* **press** — resolve camera once; pick (§4.6); snapshot the press-time base of the affected model
  subset on device; open an undo bracket.
* **move** — recompute the affected subset **from the press-time base** (idempotent, abortable);
  run the dependent kernels (§4.2 order); publish a display snapshot to the Tonic scene index;
  `UpdateViewport()`. **No stage traffic.**
* **release** — seal the undo entry in `TonicUndo`; bump `model.version`; **enqueue** a commit
  request (§3). Nothing else. The frame after release is identical to the last move frame.
* **escape** — restore the press-time base; publish; drop the bracket.

The budget for a move is the tool's own end-to-end **≤ 8 ms** at the reference scene (§7),
with a fallback ladder that degrades what is *drawn*, never what is *edited*.

---

## 2. Data model

### 2.1 `TonicModel` (in-memory, device-backed)

```
TonicModel
  scalp        : ScalpBinding        // mesh prim path, rest points/normals (device), face adjacency, BVH
  graph        : ScalpGraph          // nodes (id, faceId, uv, P, N, valence), edges (node pairs, on-surface
                                     // polyline, the two regions it separates), regions (closed faces of the
                                     // planar graph, node loop, colour). Nodes are SHARED: one node may bound
                                     // any number of regions; an edge always bounds two (or one at the graph
                                     // border). Welding two nodes merges their ids and re-links every edge.
  tubes[]      : Tube                // id, regionId, parent, children[], level, childIndex,
                                     //   center : CenterCurve {cv[] (P), knots, frames[] (RMF)}
                                     //   sections[] : CrossSection {t, cv[8..32] in section plane, scale, twist}
                                     //   derived : DerivedShape {center, sections}   // from parent via K14, empty at L1
                                     //   deltas  : ShapeDeltas  {centerCvDeltas in parent-local frames, sectionCvDeltas}
                                     //   subdivide : {count (2..8), seed, splitMode (kmeans|edge)}
                                     //   fill : FillParams {density, cvCount, lengthProfile, edgeBias, seed}
                                     //   flags: locked, hidden, transientParent, lockParents, lockChildren
                                     // authored shape == derived + deltas (L1: derived is empty, deltas absolute)
  guides       : GuideSet            // per tube: roots (faceId, uv), CVs (device), tubeId, hierarchy path
  sculpt       : SculptDeltas        // per (tube, level): center CV deltas in parent-relative frames
  selection    : Selection           // tubes, center CVs, section CVs, graph nodes, guides
  version      : uint64              // bumped per sealed gesture; the committer keys on it
```

Device buffers are planar SoA (`DeviceBuffer<T>`, `libs/usdGen/usdGen/gpu/`), sized per tube with a
free-list so a drag reallocates nothing. Host mirrors exist only for the fields the committer
serialises, and are refreshed lazily by version.

### 2.2 Stage schema (authored by the committer only)

Codeless, `usdGen:` namespaced, added to `schema.usda` with `doc` + `customData.usdGen.evaluation`
per the C1 rules (`13-codebase-alignment.md`; `usdgen-schema-cleanup-2026-09` memory). No dead
knobs: every attribute below is read by the committer's hydrate path.

```
UsdGenTonicGroom (Xformable container)
  rel   usdGen:tonic:scalp                        # the growth mesh
  rel   usdGen:tonic:description                  # the UsdGenDescription this groom feeds
  token usdGen:tonic:version                      # committer format version

UsdGenScalpGraph (Boundable, one per groom)
  int[]    usdGen:tonic:nodeFaceIds
  float2[] usdGen:tonic:nodeUVs
  int2[]   usdGen:tonic:edges
  int[]    usdGen:tonic:regionNodeCounts         # polygon-style region encoding; nodes are shared,
  int[]    usdGen:tonic:regionNodeIndices        #   so a welded boundary appears in both regions' loops
  color3f[] usdGen:tonic:regionColors
  int2[]   usdGen:tonic:linkedRegions             # pairs that share one interpolation id (§5.1 Link regions)
  float    usdGen:tonic:snapRadius                # on-surface weld/snap distance (rest units)

UsdGenTube (Xformable; hierarchy = prim hierarchy)
  int      usdGen:tonic:regionId
  int      usdGen:tonic:level                     # L1/L2/L3… (derived from depth, stored for tools)
  point3f[] usdGen:tonic:centerPoints             # center curve CVs (world space of the groom)
  float[]  usdGen:tonic:sectionT                  # parametric position of each cross-section
  int      usdGen:tonic:sectionCvCount
  point2f[] usdGen:tonic:sectionCvs               # flattened, in the section plane
  int      usdGen:tonic:childIndex                # index within the parent's subdivision (−1 at L1)
  point3f[] usdGen:tonic:centerDeltas             # child edits relative to the derived shape (§2.3)
  point2f[] usdGen:tonic:sectionDeltas
  int      usdGen:tonic:subdivide:count           # 2..8, default 4
  int      usdGen:tonic:subdivide:seed
  token    usdGen:tonic:subdivide:splitMode       # kmeans | edge
  float    usdGen:tonic:fill:density
  int      usdGen:tonic:fill:cvCount
  float[]  usdGen:tonic:fill:lengthProfile        # ramp knots (usdGenMath ramp encoding)
  int      usdGen:tonic:fill:seed
  bool     usdGen:tonic:locked
  bool     usdGen:tonic:lockParents
  bool     usdGen:tonic:lockChildren

UsdGenTubeHierarchyAPI (applied to a tube prim that is a transient/on-the-fly parent)
  rel   usdGen:tonic:members
  bool  usdGen:tonic:persistent
```

> **2026-09-19 (V0b, plan/18 §7 G1).** What the committer actually writes,
> where this text and the code had to settle:
> * The section arrays are `float2[]`, not `point2f[]`: that is what
>   `schema.usda` declares and what the committer/hydrate pair round-trips.
>   The schema wins; this paragraph is the one that was wrong.
> * Prim names carry the model's stable tube id — `tube<n>`, and `group<n>`
>   for the negative ids on-the-fly parents mint (a `-` is not a legal prim
>   name). Child ids are `parent * 16 + 1 + childIndex`, so the nesting plus
>   `childIndex` is enough for hydrate to re-mint every id.
> * `usdGen:tonic:locked` on a CHILD tube records a bridge import (§5.7): the
>   model has no separate per-child lock bit, and an import is the frozen
>   kind. On the L1 tube it is the model's own lock flag.
> * A transient on-the-fly parent is never committed; a persistent one is,
>   with `UsdGenTubeHierarchyAPI` (`members`, `persistent`) applied.
> * Every tube carries both its authored shape (`centerPoints`,
>   `sectionCvs`) and its `centerDeltas`/`sectionDeltas`. Hydrate re-derives
>   the child from the parent's subdivision and then installs the stored
>   shape and deltas verbatim, so the round trip is bit-exact rather than
>   "close enough after re-applying the deltas".

Outputs into the **existing** contract:

* `<groom>/Guides` — `BasisCurves` with `UsdGenCurveAPI` (`primvars:usdGen:role = "guide"`,
  `usdGen:curveId`, `usdGen:rootFrame`), primvars `tubeId` (uniform int), `hierarchyLevel`, `regionId`.
* `<groom>/RegionMap` — a `UsdGenPtexMap` prim (`usdGen:map:file`, `usdGen:map:filter = "nearest"`,
  `usdGen:map:firstChannel`, `channelCount = 1`) whose `.ptx` is **baked by its own asynchronous
  pipeline** (§3.1a: bake worker, low-priority CUDA stream, versioned files, one-attribute swap;
  §4.5), never on the UI thread and never gating a stage swap. The file is what defines interpolation for usdGen curves: it is a
  multi-channel float Ptex over the scalp mesh's faces with
  - channel 0 = L1 region id (the scalp-graph region that roots the strand),
  - channel *k* = the id of the level-*k+1* tube that contains the texel (0 where that level has no
    tube), so `firstChannel` selects the hierarchy level a downstream operator parts or clumps by.
  Ids are integers stored as floats, which matches GuideInterpolate's "values within 1/1024 are one
  region" rule, and `nearest` filtering keeps borders hard.
* `<groom>/RegionExpr` — a `UsdGenExpression` with `input:regionMap → <groom>/RegionMap` and source
  `ptex("regionMap")`, output `outputs:result`. This is the only way an operator can read a map
  (`usdgen-samplers-clump-guides`: maps enter through `input:<name>` relationships on an expression).
  Additional expressions per level (`RegionExprL2`, …) are authored only when a level exists.
* The linked `UsdGenDescription` gets, if absent, a `UsdGenGuideInterpolate` operator with
  `usdGen:guides → <groom>/Guides` and `usdGen:region → <groom>/RegionExpr` (a prim-path
  connection, resolved to `outputs:result`). If a `UsdGenClump` operator exists and its
  `usdGen:clump:map` is unconnected, the tool offers (does not force) `RegionExprL<n>` for the deepest
  level, which makes usdGen clumps follow Tonic's finest tubes. The tool never rewrites an operator
  stack an artist has hand-authored; it only fills in connections that are empty.
* A per-face `int primvars:usdGen:tonicRegion` on the scalp mesh is also written; it is the
  **live** map (no file I/O, updated every swap) used by the HUD, by the Ptex bake as its source, and
  by anyone who prefers a primvar. The Ptex is the contract; the primvar is the preview.

Everything the committer writes lands in a tool-owned anonymous sublayer of the session layer
(`usdGenTonic-live.usda`, in memory). "Save groom" copies that layer's content into a file layer the
artist picks (`.usdc`, never `.usda`, per S42), and re-parents the live sublayer beneath it.

### 2.3 What "subdividing a tube" means (and does not mean)

Subdivision in this tool is **clump subdivision, never geometric subdivision surfaces**. A tube is
never refined into a smoother mesh; it is **split into a small number of child tubes** (four by
default, `subdivide:count` in 2..8) whose shapes are **derived from the parent's shape**:

1. The parent's root region is partitioned into `count` sub-regions (§4.1 K14: k-means over the
   root cross-section in the parent's root frame, seeded for determinism, or along an artist-drawn
   splitting edge in Graph mode).
2. Each child's **center curve** starts as the parent's center curve offset by the sub-region's
   centroid, carried up the tube through the parent's section interpolation, so the children together
   occupy exactly the parent's volume.
3. Each child's **cross-sections** are the parent's sections clipped to the child's sub-region at
   every `t` and re-fitted to the child's ring CV count.
4. The children **inherit** fill parameters, hierarchy level `parent.level + 1`, and the parent's
   sculpt frames; the parent **persists** as their parent (contract 7).

Consequences that the rest of the plan relies on:

* A parent tube is always a **valid tube on its own**. Its center curve and sections are real data,
  not an aggregate; the artist can edit at any level.
* A child tube's identity is `(parentId, childIndex)` plus a stable `tubeId`; its shape is stored as
  **deltas relative to the derived shape** (§2.1 `sculpt`), so re-deriving from an edited parent
  keeps the artist's child edits.
* Subdividing is reversible (§2.4) and the guides of a subdivided tube are filled **per child**, never
  per parent; the parent's own fill is suspended while it has children (its density becomes the
  sum of the children's).

> **2026-09-19 (V0b).** Fill params now live per tube (`TonicModel::TubeRecord::fill`),
> children inherit the parent's set at the split, and the committer fills only
> LEAF tubes. Suspension keys on a *subdivided* child: a bridge import is not
> a subdivision, so a tube that only carries imports keeps filling, and an
> imported tube is never filled at all (its shape is the artist's geometry).
> The root hash stream is keyed by the tube id, so siblings that inherited one
> set of params still draw different roots.

### 2.4 Bidirectional levels: moving up and down the hierarchy

Artists move between levels in both directions at any time, on any selection. Every level is a
first-class editing surface; there is no "final" level.

| Direction | Action | What happens to the model |
|---|---|---|
| **Down** (refine) | `Subdivide` on tubes at level *n* | K14 creates level *n+1* children per §2.3. Children carry zero deltas, so the groom looks identical the instant after the split. |
| **Down** (navigate) | `Enter level` / double-click a tube | Selection and gizmos target the children; the parent draws as x-ray. No model change. |
| **Up** (navigate) | `Exit level` / `Backspace` | Selection and gizmos target the parent; children draw as x-ray or hidden per the level toggles. No model change. |
| **Up** (edit propagation) | edit a **child** | Its deltas change; if "lock parents" is off, K7 refreshes the parent's center curve **bottom-up** by averaging (contract 8) so that the parent stays the enclosing shape of its children. The parent's sections are re-fitted to the union of the children's rings at each `t`. |
| **Down** (edit propagation) | edit a **parent** | K6 re-derives every descendant **top-down** (length-preserving; deltas re-applied in the parent's new local frames), so children follow the parent while keeping their own sculpt. |
| **Up** (coarsen) | `Merge children` on a parent | Children are removed; before removal K7 runs one last bottom-up pass so the parent captures their aggregate shape; child guides are replaced by the parent's fill. Undoable. |
| **Up** (coarsen, partial) | `Merge selected` on siblings | The selected siblings fold into one child at the same level (§4.1 K14 inverse); the others are untouched. |
| **Sideways** | `Re-subdivide` | Equivalent to merge + subdivide with a new `count` or seed; deltas of the old children are discarded with a confirm. |
| **Any** | `Solo level n`, `Show levels ≤ n` | Display only. |

Two switches control propagation: **lock parents** (child edits do not refresh ancestors) and
**lock children** (parent edits move descendants rigidly with the parent's frames, K6 with deltas
frozen). Both default off. Propagation runs inside the same gesture on `tonicStream`, so a drag on an
L1 tube with 2 400 L3 descendants is one K6 pass per move (gate TN-1).

### 2.5 Hydrate (stage → model)

Opening the tool on a stage that already holds a `UsdGenTonicGroom` builds `TonicModel` from the
prims above. Guides on the stage are **not** re-read as authoritative geometry: they are regenerated
from tubes + fill params + seed, and the committer asserts bit-equality with the stored guides
(same kernels, same seed) so a stage produced by the tool always round-trips. Foreign guides (a DCC,
hand-authored) are imported as **L3 locked tubes** (§5.7) rather than as fill output.

> **2026-09-19 (V0b, plan/18 §7 G3).** Hydrate rebuilds every level: the L1
> tube is restored outright, each deeper level is re-created by re-running the
> stored `(tubeId, seed)` subdivision and then installing the stored shape and
> deltas, on-the-fly parents come back through their `members`, and imports
> through `ImportLockedTube`. A stored curve is FOREIGN when no model tube
> claims it (`primvars:tubeId` absent or naming no tube); those are imported
> under the deepest tube whose K14 cell contains the curve's root, so the
> import lands one level below it — L3 for the usual L1/L2 groom, which is
> where the "L3 locked tubes" wording comes from. Tool-produced guides keep
> the hard bit-equality assert.

---

## 3. The asynchronous commit pipeline (the non-blocking requirement)

### 3.1 Why it must be a three-stage pipeline

USD authoring is main-thread-only in practice (`UsdStage` change processing, `Sdf` change blocks,
usdview's own listeners). So the expensive part — building thousands of property specs — runs
off-thread into a layer that **no stage has opened**, and the main thread does only a bulk swap.

```
 UI thread                 commit worker                     UI thread (idle)
 release → enqueue(v) ──► snapshot(v) → build SdfLayer L_v ──► TransferContent(live ← L_v)
                            (anonymous, not on any stage)      inside one Sdf.ChangeBlock
                                                               UpdateViewport()
```

* **Enqueue** stores only the version number. The worker takes the *latest* version when it wakes
  (coalescing), so ten fast strokes produce one layer build.
* **Snapshot** reads the host mirrors for the changed tubes (the model tracks a dirty set per
  version). Device → host copies for guides happen here, on `tonicStream`, with a CUDA event the
  worker waits on — never the UI thread.
* **Build** writes into a fresh anonymous `SdfLayer`, using `SdfCreatePrimInLayer` and typed
  `SdfAttributeSpec` field sets. No `UsdStage` involved: `Sdf` is safe for independent layers.
* **Swap** on the UI thread: `SdfLayer::TransferContent(live, L_v)` inside one `SdfChangeBlock`.
  This is a single change-processing pass that resyncs the tool's subtree once. It runs from a Qt
  idle timer (`QTimer.singleShot(0)`), is skipped while a gesture is active, and is skipped if a newer
  version is already being built (the worker will hand a newer layer soon). Measured budget: ≤ 5 ms
  for a reference-scale groom (gate TN-4, §7). If the measured swap exceeds the budget, the fallback is
  **partial transfer**: one child prim subtree per idle slot (`SdfCopySpec` per tube), which keeps every
  slot under budget at the cost of a few frames of staleness in the stage — acceptable because the
  viewport is showing the model, not the stage.

> **2026-09-19 (V6, plan/18 §7 G6).** The "Snapshot" bullet above described
> a dirty set the model never had: `TonicSnapshotFromModel` copied every
> tube's host vectors and `TonicGuidesFromSnapshot` re-ran K8/K9/K10 over
> every tube, on every version. Moving one CV of one tube in the reference
> groom refilled all 12 000 guides to change five of them.
>
> As built now, the dirty set is a **content hash per snapshot entry**
> (`TonicSnapshotTube::contentHash`, FNV-1a over the tube bytes the fill
> reads, by float bit pattern so the key is as exact as the fill). The
> committer owns a `TonicGuideCache` keyed on it: a tube whose hash is
> unchanged has its guide slice copied, a tube whose hash moved is refilled.
> A changed scalp copy invalidates every slice at once; tubes a version
> dropped are evicted. `TonicGuidesFromSnapshot` with no cache — which is
> what hydrate's bit-equality assertion uses — still refills everything, so
> the round-trip proof is never answered by the writer it checks.
>
> Measured at the §7 reference scale (2 400 tubes, 12 000 guides × 16 CVs,
> `testUsdGenTonicCommit`), one version that moved one CV of one tube:
>
> | Guide refill for a one-tube version | Before | After |
> |---|---|---|
> | worker-thread time | 35.5 ms | 2.4 ms |
> | tubes refilled | 2 400 | 1 |
>
> The result is asserted byte-identical to the uncached one, element by
> element across points, counts, frames, ids, tube ids, levels and region
> ids. Two things in the bullet above are still NOT built and are now
> stated as such rather than promised: the snapshot still **copies** every
> tube's host vectors (the copy is host-vector assignment, tens of
> microseconds at this scale, and skipping it would need a per-tube version
> counter on all 70 `++_version` sites), and the guide device→host copy is
> still the synchronous path, with no `tonicStream` CUDA event — guides are
> generated on the CPU on the worker, so there is no D2H to overlap yet.

### 3.1a The Ptex bake pipeline (asynchronous, independent of the stage pipeline)

The region map bake is a **second write-behind pipeline**, with the same shape as §3.1 and the same
rule: nothing in it ever runs on the UI thread or on the interaction stream, and nothing in §3.1
waits for it.

```
 graph/hierarchy edit ──► enqueue(mapVersion m)
   bake worker:  rasterise dirty faces (K3, bakeStream) → D2H (pinned, event) → write regionMap.v<m>.ptx (tmp → rename)
   UI idle:      author usdGen:map:file = regionMap.v<m>.ptx on <groom>/RegionMap   (one attribute, one ChangeBlock)
```

* **Own version counter.** `mapVersion` bumps only on graph or hierarchy changes (the only edits
  that change ids). Tube and sculpt edits never enqueue a bake, so §3.1 swaps for those carry
  guides only and are never delayed by a map.
* **Own CUDA stream.** K3's texel pass runs on `bakeStream` at low priority
  (`cudaStreamCreateWithPriority`, lowest), never on `tonicStream`, so a bake in flight cannot
  delay a move. The D2H copy is to pinned memory with an event the bake worker waits on; the UI
  thread never synchronises with either stream.
* **Own worker thread** (`TonicBakeWorker`), coalescing like the commit worker: it takes the latest
  `mapVersion` when it wakes, and a bake that is superseded mid-run is cancelled at its next tile
  boundary (per-face rasterisation is chunked, ~4 K faces per chunk, checked between chunks).
* **Incremental rasterisation.** The worker keeps a per-face texel cache; only faces whose region
  or tube ids changed (K3's dirty set from the graph diff) are re-rasterised. The Ptex writer still
  writes the full file (the format has no in-place update), which is pure I/O on the worker.
* **Own swap.** When the file is complete the worker posts an idle task that authors exactly one
  attribute, `usdGen:map:file` on `<groom>/RegionMap`, in one `SdfChangeBlock`. That is the only
  main-thread work of the bake (microseconds). Because the filename is versioned, the swap never
  points at a half-written file and usdGen's `imageMapCache` (keyed by path) never serves a stale
  map. Older `.ptx` versions are deleted by the worker once no live layer references them.
* **Ordering with the stage swap.** The stage swap for version *v* may land before the map for the
  graph change in *v* is baked. During that window GuideInterpolate cooks against the previous map
  (or, for a brand-new region, against no region for those roots, which its "nearest guide"
  fallback handles). The HUD shows "map v<m−1>, stage v<v>" in amber until they match. This is
  deliberate: the amplified hair is a preview, the viewport draws the model, and a bake must never
  gate a guide swap.
* "Save groom" writes the final `regionMap.ptx` beside the saved layer (a copy of the latest baked
  version, on the worker) and repoints the asset in the same layer re-parent step.

> **2026-09-19 (V0b, plan/18 §7 G5).** The repointed asset path is written
> RELATIVE to the saved layer (`./regionMap.ptx`), so a saved groom survives
> being moved; it used to be absolute. `TonicSaveGroomAndMaps` runs on the UI
> thread (it re-parents the session layer stack, which is main-thread work) —
> the header used to say the opposite. The bake's D2H is now an async copy on
> `bakeStream` into pinned memory with an event the worker waits on, and
> `NoteSwapped` sets its own wake reason so the sweep runs when a swap frees
> an older file with no bake behind it.
* Cost, for the record: a 60 K-face scalp at 32×32 texels/face is ≈ 60 M texels; the full first
  bake is ~100–200 ms on the worker, incremental bakes are proportional to the dirty faces plus the
  file write. Neither number is visible to interaction (gate **TN-7**, §7).

### 3.2 Interaction with the usdGen cook

The stage swap dirties `<groom>/Guides` and, through `UsdGenDirtyRouter`, the linked description's
`GuideInterpolate` node; the groom scene index cooks on its own commit thread as today (one cook per
dirty batch, gate SI-3). Two rules keep that cook from hurting interactivity:

1. **"Show amplified hair" is off during gestures.** The Tonic index publishes the amplified tiles'
   `visibility = false` override while a gesture is active, and the committer holds swaps back
   (§3.1), so no cook is even requested until the artist pauses.
2. **Superseded cooks are abandoned.** The existing "superseded mid-run keeps the previous
   generation" behaviour (`08-tools.md` §2.4) is already what we want; the Tonic committer just adds a
   cancellation token on the description's session so a cook older than the latest swap stops early.

The tool never calls `UsdGenImaging_Commit()`. The cook is a side effect of the stage swap, exactly
as `08-tools.md` §2.4 rules, only later and coalesced.

> **2026-09-19 (V6, plan/18 §7 G7).** Both rules are built now; neither was
> before V6 (rule 1's flag was written by the panel and read nowhere, and
> rule 2 had no token at all).
>
> **Rule 1.** `TonicModel::ResolveHairDisplay` answers one question for both
> halves: tiles are up only when the artist's `showAmplifiedHair` is on AND
> no gesture is live, and the Tonic guide preview draws exactly when they
> are not. The scene index remembers every prim under a `__usdGenRender`
> scope as it passes through, and authors a `visibility = false` overlay
> over those paths when tiles are down — an overlay, so the tile keeps
> every other data source the groom index published and comes straight back
> on release. A flip dirties the tile paths and the guide prims, nothing
> else. `testUsdGenTonicIndex` drives the whole cycle: off → hidden tiles +
> preview, on → tiles + no preview, press → hidden tiles + preview back,
> release → tiles back, and the artist's own setting survives the gesture.
>
> **Rule 2.** The cancellation token is a monotonic counter on
> `UsdGenImagingSession` (`CancelCooks()` / `CookToken()`).
> `CommitAsync` stamps each request with the token it was accepted under,
> and the session's owner frame drops a request stamped before the latest
> `CancelCooks()` as `Superseded` **without handing it to the engine**.
> Requests the engine already holds keep the engine's own supersede
> behaviour, which is what §2.4 already gave us. `UsdGenSessionStore::
> CancelCooks(groomRoot)` cancels by groom root, and
> `TonicCommitter::CancelDescriptionCooks()` (C ABI
> `Tonic_CommitterCancelCooks`) points that at the committer's linked
> description. `TonicSession.beginGesture` calls it on the outermost press:
> the cook the last swap started is describing a groom the model has
> already left, and its tiles are hidden for the length of the gesture
> anyway. Cancelling is not a latch — the next request publishes normally.
> T1: `testUsdGenSessionStoreOwner` (stale dropped, fresh published, cancel
> scoped by groom root) and `testUsdGenTonicCommit` (the committer and the
> C ABI cancel our description's session and no other).

### 3.3 Undo

Undo is on the **model**, not on the stage (`TonicUndo`: per-gesture sparse deltas with a 200-entry
limit and a memory budget, mirroring `08-tools.md` §7.3's `UndoStack` shape). An undo is a model
mutation like any other and enqueues a commit. Consequently the stage never needs `SubtreeSnapshot`
for Tonic prims, and usdview's own edit-target undo is untouched. The one stage-level action that
must be undoable through the stage is "Save groom" (a layer re-parent), which uses
`08-tools.md` §7.3's recorder.

### 3.4 Failure modes

| Failure | Behaviour |
|---|---|
| worker throws (bad snapshot) | log, keep the previous live layer, status line red, model unaffected |
| stage closed / reloaded under the tool | committer detaches; the model survives; "Reattach" re-hydrates or re-creates the groom prim |
| scalp mesh topology changes | all graph nodes carry `(faceId, uv)`; a topology change invalidates the binding and freezes editing until the artist re-binds (Tonic's own requirement is a stable growth surface) |
| CUDA OOM | the model is host-authoritative for CVs; kernels fall back to the CPU reference implementations (§4.1) at reduced density and the HUD says so |

> **2026-09-19 (V0b, plan/18 §7 G5).** Both worker loops now catch: the commit
> worker keeps the previously built layer and the live layer, the bake worker
> keeps the previous `.ptx`, both record the message on their existing
> diagnostic path, and both threads survive. A version whose build failed is
> parked so the worker does not re-run the same failing build in a tight loop;
> the next enqueue (a newer version) clears it.

---

## 4. CUDA kernels (`TonicKernels`)

Every kernel has a CPU reference implementation (same file, `__host__ __device__` where the
`irExec.h` pattern applies, else a separate `.cpp` twin) proven bit-identical or tolerance-identical
by a T0 test, as `testUsdGenCudaExpressionParity` does today. The CPU twin is the fallback under §3.4
and is what T0 CI runs on hosts without a GPU.

### 4.1 Kernel list

| # | Kernel | Input → output | Notes |
|---|---|---|---|
| K1 | `scalpBvhBuild` / `scalpRaycast` | scalp rest mesh → LBVH; ray → (faceId, uv, P, N) | Used by every mouse pick on the scalp. LBVH via Morton codes, refit-only on deform. |
| K2 | `graphEdgeTrace` | node pair → on-surface polyline | Straightest geodesic on the mesh (Polthier–Schmies discrete geodesic, bounded step count), used for edge display and region rasterisation. |
| K3 | `regionRasterise` | graph regions → per-face region id (and per-texel, per-level for the Ptex bake) | Point-in-region via winding number in the mesh's per-face tangent chart; runs when the graph changes, not per frame. Also produces the **coverage map** (faces with no region) and the **root-intersection map** (faces claimed twice). |
| G1 | `graphOps` (CPU, `tonicGraph.cpp`) | node/edge edits → welded planar graph + region loops | Weld, unweld, connect, split-edge, delete, snap search (kd-tree over nodes in the surface chart), planar face extraction, linked-region union-find. Graph scale is hundreds of nodes, so this stays CPU and is Qt-free and unit-tested at T0. |
| K4 | `centerFrames` | center CVs → RMF frames along the sampled curve | Double-reflection rotation-minimising frames; parent frames feed K6. |
| K5 | `tubeTessellate` | center + sections → tube mesh (rings × segments) | Cross-sections interpolated along `t` by cubic Hermite of the section CV vectors in the RMF frame; emits positions, normals, `tubeId`, `sectionT`. This is the > 500 K vertex display set. |
| K6 | `hierarchicalSculpt` (top-down) | edited parent → every descendant's derived shape + re-applied deltas | Length-preserving: a child keeps its arc length and its offset in the parent's local RMF frame (the SIGGRAPH 2018 description, our reconstruction). Re-derives §2.3 steps 2–3 from the parent's new shape and re-applies the child's stored deltas; with `lockChildren` the deltas are frozen in the new frames. One pass per move over the edited subtree. |
| K7 | `parentAverage` (bottom-up) | children center curves + sections → parent center curve + sections | Recursive arc-length-resampled averaging of the children's centers; parent sections re-fitted to the union of the children's rings at each `t`. Used to create on-the-fly parents, to refresh persistent parents after child edits (unless `lockParents`), and as the last step of `Merge children`. |
| K14 | `tubeSubdivide` / `tubeMerge` | parent tube + `count` + seed (or a splitting edge) → `count` child tubes; inverse: siblings → one tube | Implements §2.3: root sub-regions by seeded k-means in the root frame (or by the drawn edge), child centers via the parent's section interpolation, child sections as clipped and re-fitted parent sections. Deterministic per `(tubeId, seed)`, so hydrate reproduces children bit-exactly and only deltas need storing. Merge is K7 over the selected siblings followed by removal. |
| K8 | `guideRootSample` | region faces + density → root (faceId, uv) | Blue-noise over the region: per-face Poisson-disk in the tangent chart with a hash-based seed (`usdGenMath/hash.h`), deterministic per `(tubeId, seed)`. |
| K9 | `guideFill` | roots + tube → guide CVs | A root's normalised position in the root cross-section is carried up the tube through the section interpolation of K5; `edgeBias` and `lengthProfile` modulate radius and length. This is the unpublished fill kernel; ours is documented as such. |
| K10 | `guideResample` | guide CVs → fixed `cvCount` | Arc-length resample; emits `UsdGenCurveAPI` root frames. |
| K11 | `tonicPick` | screen-space query → nearest tube vertex / center CV / section CV / node / guide | Reuses `gpu/picking.cu`'s screen-projection convention (row-major USD viewProj, top-left origin) and its scalar readback; adds a per-primitive-kind mask so a mode only picks what it can edit. |
| K12 | `tubeIntersect` | tube meshes → per-tube overlap flags at the root ring | Root-level intersection check (requirement 6), broad phase by ring AABBs, narrow phase ring-vs-ring polygon overlap; runs after a gesture, not during. |
| K13 | `smoothnessScore` | center curves → per-CV curvature spikes | Drives the HUD warning and the optional "relax" action. |

> **2026-09-19 (V6, plan/18 §7 G12).** Four rows above describe published
> algorithms we did not implement. V6's job was to implement them or say
> what the code does and why; these four stay as built, and the reasons are
> here rather than in a comment nobody reads. (The two G12 items that were
> *fixed* instead are in §5.2: the auto-tube root section is now fitted to
> the region boundary, and the authored ring CV count is 8..32.)
>
> * **K2 is a projected chord, not a straightest geodesic.** `lerp(a, b)`
>   is sampled and each sample is projected to the nearest surface point
>   (`TonicTraceEdgeCpu`, `TonicLaunchEdgeTrace`; ends pinned exactly).
>   Polthier–Schmies would give a curve that is straight *in the surface*;
>   the chord projection gives one that is straight in space and lies on
>   the surface. On a scalp — a height-field-like surface with no handles
>   over the span of one region boundary — the two agree to well inside the
>   snap radius, and the projection is O(samples × faces) with no
>   half-edge structure, no unfolding and no special cases at saddle
>   vertices. The divergence that would matter is a boundary drawn across a
>   deep crease or around an ear, where the chord can leave the surface far
>   enough that its projection jumps; that is a scalp the artist would
>   split into two strokes anyway. Revisit if a real head model shows a
>   jump.
> * **K3 classifies by planar point-in-polygon, not tangent-chart
>   winding.** `TonicFlattenLoops` gives each region a Newell normal and a
>   tangent basis, and the point is tested in that one plane
>   (`TonicPointInRegionCpu` / `TonicLaunchClassifyPoints`, one shared
>   formula so CPU and device are bit-identical). A winding number in each
>   face's own chart would be correct for a region wrapping more than a
>   hemisphere; the single plane is correct for a region whose boundary is
>   star-shaped about its own Newell plane, which every region an artist
>   draws on a scalp is. The failure mode is visible and cheap to
>   recognise: a region that wraps too far reports uncovered faces in the
>   coverage HUD, which is already the artist's cue to split it.
> * **G1's snap search is brute force, not a kd-tree.** `tonicGraph.h`
>   says so on the query: graph scale is hundreds of nodes, so the search
>   is a few hundred distance tests per stroke sample, which does not
>   register against the K1 raycast in the same sample. A kd-tree would
>   have to be rebuilt on every weld, split and delete — more code and more
>   invalidation than the scan it replaces. Revisit at thousands of nodes.
> * **Mirror-X is a closest-point, not a symmetry map.** `TonicMirrorXCpu`
>   reflects the point across x = 0 and takes the nearest surface point
>   (`TonicClosestPointCpu`). On an asymmetric mesh the twin lands on the
>   surface but not on the topologically mirrored face, so the two halves
>   of a groom can drift where the model is not symmetric. The E-4 kNN
>   symmetry map §5.1 refers to belongs to the usdGen deformation lane and
>   is not built here. Mirror-X is off by default, which is the honest
>   default for a feature with this caveat; the HUD does not yet warn, and
>   that is the thing to add when a user asks for it.

### 4.2 Per-move dependency order

`K6 (descendants, if a parent moved) → K7 (ancestors, if a child moved and !lockParents) → K4 → K5
→ [K9 → K10 if guides shown] → K11 footprint → publish`. K3, K8, K12, K13 and K14 run on gesture
end or on explicit actions (subdivide, merge, graph edits), never per move. Guides during a drag are refilled at a
**preview density** (default 25 %, the fallback ladder lowers it further, §7) and at full density on
release.

### 4.3 Transport to Storm

* **Phase A, host-staged (ships first).** Each published buffer is copied device → pinned host on
  `tonicStream`; the Tonic index hands `VtArray`s that alias the pinned block (`HdVtBufferSource`
  over an external buffer with a keep-alive) so no second copy is made in Python or in Hydra. At the
  reference (500 K tube verts × 12 B pos + 12 B N ≈ 12 MB) a PCIe 4 copy is ≈ 0.5 ms; Storm's
  upload is the same cost it pays for any changed mesh. This is fully inside the §7 budget.
* **Phase B, CUDA-GL interop (later).** Route tube meshes and guide previews through
  `UsdGenCudaGlComputation` once the OpenUSD patches (`patches/openusd/`, commit `cb0be00`) are
  restored and the stock-Storm handoff is finished. Nothing in the model or kernels changes; only the
  publisher's buffer sources do. This is the same work that unblocks CUDA-lane display for usdGen
  itself, so it is scheduled with it, not ahead of it.

> **2026-09-18 (plan/18 V0).** Two corrections to the paragraph above, both
> as built. (1) The `VtArray`s do **not** alias the pinned block: `VtArray`
> has no external-buffer form, so every publish makes one copy out of the
> pinned block into the array Hydra retains. The pinned block stays the D2H
> target, and it is now grow-only (`TonicPinnedStaging::Ensure`), so a
> gesture allocates on its first frame and never again. (2) Staging is
> per level and per tube, not per buffer: `tonicPublish.{h,cpp}` keeps a host
> mirror plus a content hash per tube, and a publish re-tessellates only the
> tubes whose hash changed. The D2H itself is still a blocking `cudaMemcpy`
> after a stream sync (audit row "Device to pinned host on tonicStream"),
> which V6 revisits with the rest of G6.

> **2026-09-19 (V6, plan/18 §7 G6).** V6 revisited it and the copy stays.
> Measured (`testUsdGenTonicIndex`, RTX 4090 / CUDA 12.6): filling the
> retained `VtArray`s for a whole reference-scale tube set — 770 000
> vertices of positions plus normals, 17.6 MB out of a genuinely pinned
> block — costs **3.7 ms**. That is the worst case the aliasing promise was
> about, and three things make it the wrong thing to remove:
>
> 1. **There is nothing to remove it with.** `VtArray` has no
>    external-buffer constructor. The "no second copy" form was
>    `HdVtBufferSource` over a borrowed pointer, a Hydra 1.x spelling; a
>    Hydra 2.0 retained data source owns its array. Aliasing would mean
>    either patching `VtArray` or keeping our own `HdBufferSource`
>    subclass alive past the scene index, and the second is exactly the
>    keep-alive bug class that phase B's interop work removes properly.
> 2. **No publish pays the worst case.** The publisher restages only the
>    tubes whose content hash moved (§2.2 / plan/18 §2.2), so a one-CV drag
>    copies one tube's slice, not 17.6 MB. The 3.7 ms is what a full
>    re-topology costs, which happens on subdivide and merge, not per move.
> 3. **Phase B replaces the whole TU anyway.** CUDA-GL interop through
>    `UsdGenCudaGlComputation` removes the staging *and* the copy together,
>    and it is scheduled with the usdGen CUDA-lane display work, not ahead
>    of it.
>
> So the promise is withdrawn rather than met: phase A copies once out of
> the pinned block, and the sentence above this note is the accurate one.

### 4.4 Tube display quality

Tube meshes carry `tubeId` and `hierarchyLevel` uniform primvars; the Tonic index binds a small
glslfx (`usdGenShaders/resources/shaders/tonicTube.glslfx`) that colours by level/region, draws
selected tubes with a fresnel rim, and supports **x-ray** (see-through parents while editing
children). Guides use the existing `UsdGenHairPreview` material at the usdview complexity level.

> **2026-09-18 (plan/18 V0, section 2.4a).** Colouring by level/region is
> replaced by colouring by **clump**, which is what the published Tonic
> stills show: one saturated colour per clump, no level tint. The index
> publishes a `clumpColor` primvar (uniform, per face) from a 16-entry
> palette indexed by the L1 region id, with lightness steps by `childIndex`
> so a lock stays one hue family; `TonicClumpColor` in `tonicModel.h` is the
> single table, and the scalp region tint reads it too, so a region's patch
> on the head and the tube rooted in it match. The glslfx therefore drops
> `levelColor1..3` and `regionShift`, and shades with a headlight Lambert
> plus a soft specular instead of the facing-shade.
>
> `selected` and `xray` are now primvars rather than material uniforms
> (`HD_HAS_`-guarded), so one material serves every level and every
> selection state and a selection change dirties one primvar leaf instead
> of rebinding materials. `tubeId` and `hierarchyLevel` are still published
> (as ints, for the tools and the tests) but no longer read by the shader.
> A third material prim, `material_overlay`, is the same glslfx with its one
> parameter `unlit` set; the center curves, section rings and CV dots bind
> it so they draw as flat `displayColor` over the shaded tubes.
>
> Uniform primvars are sized to the real face or curve count. The P3 index
> published them as single-element `constant` arrays because it had one tube
> per mesh; a level mesh carries many tubes, so uniform is both the correct
> interpolation and correctly sized, and the "Storm rejects a 1-element
> uniform" note no longer applies.

### 4.5 Region maps and the Ptex bake

`regionRasterise` (K3) runs at two resolutions from the same inputs:

* **per face** → `primvars:usdGen:tonicRegion` (live, every swap);
* **per Ptex texel** → the multi-channel `.ptx` of §2.2 (worker bake, when the graph or hierarchy
  changed). Per-face texel resolution follows face area (`log2` of area over the median, clamped to
  4..64 per side, artist override in the Output panel); faces fully inside one region collapse to
  1×1. Texels on a region boundary take the region on the geodesic-inside of the edge (K2 polyline
  winding), so the map is geodesic by construction, the Strange World point. Border faces with no
  region (coverage gaps) bake as `-1` and are reported by the HUD before the bake runs.

Channel layout: `[L1 region, L2 tube, L3 tube, …]`, one float channel per hierarchy level, so a
single file feeds `GuideInterpolate` (`firstChannel = 0`) and `Clump` at any level
(`firstChannel = level - 1`) through separate `UsdGenPtexMap` prims that share the file.

> **2026-09-19 (V0b, plan/18 §7 G4).** How channel *k* is decided, ours (the
> publications say nothing): K14 partitions a parent's root ring in the
> parent's root frame and each child's center starts at its cell's centroid
> (§2.3 step 2), so the cells ARE the Voronoi cells of the children's root
> centers in that plane. A texel walks down from the L1 tube of its region,
> taking the nearest child root center at each level
> (`TonicOwningChildCell`). Points outside the parent's root ring still land
> in a cell, which is what a Clump needs: every strand gets an id. The walk
> runs on the host even when channel 0 was classified on the device; a device
> lane for it is V6 work, and the same partition decides which scalp faces a
> child fills, so a strand's guide and its map channel always agree.

The bake is validated by a T1 test that samples the `.ptx` through the real `ptex()` expression on
a `GuideInterpolate` and asserts every strand's region equals the per-face primvar of its root.

### 4.6 Picking and selection

Screen-space GPU pick (K11) at every press and, throttled to ≥ 3 px of travel, during hover for
highlighting. Selection kinds: graph node, edge, region, tube, center CV, section CV, section ring,
guide, hierarchy level. Marquee/lasso selection is K11 with a polygon test. Occlusion: tube vertices
carry the depth from the last frame's projection, so the pick prefers un-occluded candidates; if
gate TN-3 (§7) shows occlusion errors, the documented fallback is `HdxPickTask` over the Tonic index
only (a small render index), as `08-tools.md` §6.2 already reserves.

---

## 5. Artist-facing feature set (Tonic parity)

Modes are a shelf; each mode has one tool loop, one gizmo family, one hotkey set. All numbers in
panels are generated from the model's parameter descriptors (the `08-tools.md` §5.2 rule).

### 5.1 Graph mode (Tonic contract 1–2)

The scalp graph is one shared planar graph on the growth surface. Its control vertices (nodes) are
the artist's handles, they are **shared between neighbouring regions**, and they can be connected
and welded so the graph stays watertight (Tonic's coverage requirement means adjacent regions must
share their boundary, not merely touch).

* **Draw**: freehand stroke on the scalp (K1 pick per sample) → Douglas–Peucker simplification in
  the surface chart → a chain of nodes and edges. A stroke that starts or ends within the snap
  radius of an existing node or edge **welds** to it (a stroke ending on an edge splits that edge
  and welds to the new node). A stroke that closes on itself becomes a region immediately. This is
  the primary way to "draw regions onto the scalp".
* **Place / move**: click adds a single node; drag moves it (`(faceId, uv)` stored, so it survives
  scalp deformation). Dragging a node **onto** another node within the snap radius welds them
  (`Weld`, also `Shift+W` on a two-node selection); dragging onto an edge splits and welds. Welding
  merges the node ids, re-links every incident edge, removes degenerate edges and re-extracts
  regions. Undoable.
* **Connect**: select two nodes and `C` (or click-click in Connect sub-mode) adds an edge along K2's
  geodesic; connecting across an existing region splits it into two regions (each gets a tube stub,
  the old tube's children re-derive against the new root regions per §2.3, or the artist is asked
  to merge first if the tube already carries child deltas).
* **Unweld / split node** (`Shift+U`): a node shared by *n* regions becomes *n* coincident nodes,
  one per region, for the cases where a boundary must slide independently; the HUD shows the gap
  until they are re-welded, and coverage reports it.
* **Delete node / edge**: deleting an edge merges its two regions; deleting a node removes its edges
  and re-links the loop if the node had valence 2.
* **Snap radius** in screen pixels (default 8) and on-surface distance; **Weld all within radius**
  and **Weld coincident** batch actions for imported graphs.
* Regions update live (planar-graph face extraction on the CPU, µs at graph scale); each closed
  region gets a colour and a tube stub. Node valence and shared-edge status are visible (shared
  edges draw solid, border edges dashed, unwelded coincident nodes draw as a warning ring).
* Regions can also be **connected** to each other for interpolation purposes without a shared
  boundary: `Link regions` authors a symmetric pair list that the bake writes as the same L1 id in
  channel 0 (one usdGen interpolation region spanning two tubes) while the tubes stay distinct.
* HUD overlays: **coverage** (uncovered faces tinted), **root intersections** (K12), open edges.
* Region operations: split (draw an edge across), merge (delete the shared edge), mirror-X
  (symmetric node creation across the scalp's symmetry plane via the E-4 kNN, off by default).

> **2026-09-19 (V6, plan/18 §7 G14).** "Each closed region gets a tube
> stub" is true now; before V6 the model held one L1 tube and the second
> region an artist drew was silently unrepresentable (the V4 note in §7
> records finding it). As built: `TonicModel` keeps a region→tube map
> (`_regionTube`) plus, per L1 tube, the sorted node loop it was rooted in
> (`_tubeRegionKey`). Region ids are renumbered densely by every
> extraction, so identity travels on the loop, not the id.
> `SyncRegionTubes`, which `Rasterise` calls at gesture end, re-attaches
> the tubes a graph edit left behind: an unchanged loop keeps its tube, a
> split hands the tube to the part that kept most of the loop and leaves
> the other part stub-less for `BuildTubeFromRegion`, and a merge keeps the
> tube with the larger share and removes the other with its subtree. It
> **refuses**, with the reason in `GetDiagnostic` and nothing applied, when
> a tube it would re-derive or remove carries child deltas or imports —
> the artist is asked to merge the children first, exactly as the Connect
> bullet above says. Region ownership rides `HierarchyRollback`, so undoing
> a stub build puts the map back instead of leaving a second tube to be
> appended for the same region; `BuildTubeFromRegion` pushes one undo step
> and no longer clears the stack, so a graph stroke stays undoable through
> the stub that follows it. The legacy single-tube entry points are the
> first L1 tube (tube 0). The committer, hydrate and the bake channels
> serialise every L1 root: `testUsdGenTonicCommit` commits and hydrates a
> two-region scalp bit-exactly with distinct channel-0 ids, and
> `testUsdviewTonicGraph` strokes two regions through the real controller
> and finds two tubeId values in `/__usdGenTonic/tubes/L1` and two tube
> prims on the stage.

### 5.2 Tube mode (contract 3–4)

* Auto-tube from region: center curve seeded along the mean region normal with `N` sections, root
  section fitted to the region boundary.
* Center CVs: translate gizmo (screen-plane or normal-constrained), insert/delete CV, length slider,
  "match surface" (project first CV to scalp).
* Sections: per-ring translate/scale/rotate gizmo, per-CV drag, add/remove ring at `t`, copy ring
  along, twist. Ring CV count is per tube (8 default, up to 32).
* Soft selection along the center (falloff radius in `t`), and along the hierarchy (§5.4).
* Smoothness relax (K13-driven), root snap.

> **2026-09-19 (V0b, plan/18 §7 G2).** Every operation above takes a tube id;
> the single-tube spellings are the same call with id 0 and keep their exact
> behaviour (soft selection included). Two of them are REFUSED on a derived
> child: inserting or deleting a center CV, and adding or removing a section
> ring. A derived child's layout belongs to its parent's subdivision — the
> deltas are stored against that layout — so the honest answer is to edit the
> parent or re-subdivide. An imported tube has no derivation and accepts them.

> **2026-09-19 (V6, plan/18 §7 G12).** Two divergences in the bullets above
> are fixed rather than documented. **The auto-tube root section is fitted
> to the region boundary**, not a circle: `_RegionTubeDescLocked` takes the
> area-weighted centroid and normal of the faces the region claims, runs
> the center column along that normal, and places the root ring's CVs on
> the region boundary in the root plane (the mean fitted radius becomes the
> display width scale). A tube seeded from a long thin region is now long
> and thin. **The authored ring CV count is 8..32**, the range this section
> states: `BuildTubeFromRegion` refuses anything outside it by name. The
> bridge import path keeps 3..32, deliberately — a swept mesh arrives with
> whatever ring it was modelled with, and refusing to import it would be
> worse than accepting a triangle cross-section.

### 5.3 Fill mode (contract 5)

* Per tube or per selection: density, CV count, length profile ramp, edge bias, seed.
* Live preview of guides at preview density; full-density refill on release.
* "Freeze roots": keep root positions when density changes (re-seed only new roots).

### 5.4 Hierarchy mode (contract 7–8; the §2.3/§2.4 semantics)

* **Subdivide** (`Shift+D`): the selected tubes each become `count` children (default 4, spinner
  2..8) shaped by the parent per §2.3. Optional: draw one splitting edge across the root region
  first, and the split follows it (`splitMode = edge`). The groom looks identical right after the
  split; the parent persists (L1 → L2 → L3 …, no depth limit, HUD shows the level).
* **Merge children** (`Shift+M`): the inverse; the parent absorbs its children's aggregate shape
  (K7) and their guides are replaced by its own fill. **Merge selected** folds a subset of siblings
  into one. Both undoable.
* **Re-subdivide**: merge + subdivide with a new count/seed (confirm, since child deltas are lost).
* **Navigate down/up**: `Enter level` (double-click / `Ctrl+Down`) and `Exit level`
  (`Backspace` / `Ctrl+Up`) move the editing focus between a tube and its children; the level
  breadcrumb in the HUD is clickable. Focus is per selection, so an artist can be at L3 on one lock
  and L1 on another.
* **Edit at any level, propagate both ways**: parent edits re-derive descendants (K6, length
  preserving, child sculpt kept); child edits refresh ancestors (K7). `Lock parents` / `Lock children`
  toggles per tube or globally, both off by default.
* **Group / on-the-fly parent**: select any tubes → K7 builds a parent; transient by default;
  "Make persistent" keeps it (`UsdGenTubeHierarchyAPI` on commit).
* Level visibility, x-ray for non-focused levels, `Solo level`, `Show levels ≤ n`.
* Hierarchy annotations are the prim hierarchy on commit (contract 9); `childIndex`,
  `subdivide:*` and the deltas make the hierarchy reproducible on hydrate.

### 5.5 Sculpt mode (the hierarchical sculpt tool, contract 8)

Brushes over **center curves and guides** (not over amplified hair; that stays the M5 shelf):
grab, smooth, comb (direction), lengthen/shorten, twist, all length-preserving by default with
hierarchy propagation. Symmetry mirror-X. Falloff by screen radius and by `t`.

> **2026-09-19 (V6, plan/18 §7 G13).** **Brushes reach center curves, not
> guides, and that is deferred rather than pending.** A brush over guides
> needs somewhere to put the result, and the only honest place is a
> guide-CV delta store per tube. That store breaks the stage contract this
> plan is built on: `<groom>/Guides` is *reproducible* — the committer
> writes what K8/K9/K10 generate, hydrate regenerates it and asserts
> bit-equality, and a mismatch is a hard failure rather than a silent
> accept (§2.5, §3.1). Guide deltas would make the Guides prim authored
> data that no fill can reproduce, so hydrate's assertion would have to
> become "regenerate, then apply stored deltas, then compare" — which is
> not a weaker version of the same check, it is a different contract, with
> its own invalidation question every time a fill parameter, a root seed or
> a parent shape moves under a delta. That is a plan/17 §2.2 schema change
> and a §2.5 hydrate change, not a V6 gap-closing change, and it is where
> it belongs: a phase of its own, with the delta-invalidation rule decided
> first.
>
> **Soft selection along the hierarchy is deferred with it**, for a
> different reason. Soft selection along the center is built and, as of
> V6, works on any tube: `MoveTubeCenterCV` applied the `t` falloff on
> tube 0 only, so the same drag feathered on a root and snapped on a
> child. It now runs the identical `TonicSoftWeight` on both paths, which
> is what the §5.2 V0b note already claimed. The hierarchy form
> — one move spreading to nearby tubes with a falloff in hop distance —
> collides with the K6/K7 propagation that runs on the same move: a
> descendant inside the soft-selected set is both moved by the falloff and
> re-derived from its parent in the same call, and getting the two to
> compose rather than fight means deciding whether the soft move is an
> edit to the child's `actual` (which the re-derive then re-applies as a
> delta, compounding down the subtree) or a one-shot displacement that
> propagation must not see. Both readings are defensible and they give
> different grooms. That decision belongs with whoever builds the guide
> brushes, because it is the same question about what a brush stroke *is*
> in a hierarchy, so the two are deferred together.

### 5.6 Output / amplifier panel

* "Show amplified hair" (runs the usdGen description on committed guides, §3.2).
* Map status: last baked version, texel resolution override, per-level channel list, "Rebake now",
  and "Wire Clump to level *n*" (§2.2). "Save groom", "Export center curves" (§5.7).
* Status line: model version, committed version, swap time, last cook time, GPU memory.

### 5.7 Bridges (contract 9–10, production workflow)

* Export center curves (selected level) as `BasisCurves` to a file layer; import swept meshes or
  curves as **L3 locked tubes** under a chosen parent (a tube whose sections come from the mesh's
  rings, or a single-curve tube from a curve). This reproduces the Tonic ↔ a DCC braid round trip
  without any a DCC-specific code.

### 5.8 Explicitly not built (and why)

Noise, curl, clumping, painting and expressions live in usdGen operators and `plugin/usdGenTools`;
mixing them into the Tonic model would break the contract that tubes are authoring-side only.
The M5 brush shelf over amplified hair remains `08-tools.md`'s scope.

---

## 6. Phases

Each phase ends with a demo scene in `examples/` and the gates listed. Phases 1–3 are the vertical
slice that proves D1/D2 end to end; they should not be reordered.

| Phase | Deliverable | Depends on | Exit |
|---|---|---|---|
| **P0 Skeleton** | `usdGenTonic` lib + `UsdGenTonicSceneIndex` publishing a static test tube from `TonicModel`; `plugin/usdGenTonicTools` with the mode shelf and empty modes; host-staged transport; CMake, plugInfo, launch script updates; T1 test that the index publishes and dirties leaf-exact. | — | `testUsdGenTonicIndex` (T1) green; tube visible in usdview via `bin/launch_usdview.ps1`. |
| **P1 Commit pipeline** | Schema additions; `TonicCommitter` (worker + idle swap); hydrate; "Save groom"; the `GuideInterpolate` relationship fill-in; coalescing and cancellation. | P0 | **TN-4** swap ≤ 5 ms at the reference scene; `testUsdGenTonicCommit` proves version coalescing and that no swap runs during a gesture; round-trip test bit-equal. |
| **P2 Scalp graph + region bake** | K1–K3, G1, Graph mode (draw, place, connect, weld, unweld, split, delete, snap, link regions), coverage / intersection HUD, live region primvar, the worker Ptex bake with versioned files, `RegionMap` + `RegionExpr` + `GuideInterpolate` wiring, mirror-X. | P1 | `testUsdGenTonicGraph` (T0: weld merges ids and re-links edges, welded graphs are watertight, unweld/reweld round-trips, connect splits regions, linked regions share an id); `testUsdGenTonicRegionBake` (T1: bake → `ptex()` expression → GuideInterpolate regions equal the per-face primvar for every strand; a stale-version file is never referenced); T3 `testUsdviewTonicGraph.py` draws two adjoining regions by stroke, welds them, and checks the shared boundary. |
| **P3 Tubes + fill** | K4, K5, K8–K10, Tube and Fill modes, gizmos, guide preview, full-density on release, guides committed with `UsdGenCurveAPI`; the usdGen description amplifies them ("show amplified hair"). | P1, P2 | **TN-1** move ≤ 8 ms at the reference scene; **TN-2** pick ≤ 0.5 ms at 500 K tube verts; `examples/tonic-ponytail.usda` renders through `record_usd.ps1` with amplified hair. |
| **P4 Hierarchy + sculpt** | K6, K7, K12, K13, K14, Hierarchy and Sculpt modes, subdivide / merge / re-subdivide, level navigation both ways, edit propagation both ways with the two lock switches, on-the-fly parents, persistent parents, level x-ray. | P3 | `testUsdGenTonicHierarchy`: subdivide leaves the union of children's volumes equal to the parent's (Hausdorff ≤ 1e-3 of the tube radius); subdivide → merge round-trips the parent shape bit-exactly when children are untouched; length preservation to 1e-4 relative; parent averaging idempotent; hydrate re-derives children bit-exactly from `(tubeId, seed)` + deltas. T3 `testUsdviewTonicLevels.py` walks L1 → L3 and back with edits at each level. `examples/tonic-braid-hierarchy.usda`. |
| **P5 Bridges + maps** | Center-curve export, L3 import, Ptex bake, geodesic map validation against the primvar map. | P3 | Import/export round-trip test; baked `.ptx` matches K3 primvar per face. |
| **P6 Hardening** | Undo limits and memory budget, CUDA OOM fallback, stage reload/reattach, fallback ladder tuning, workstation protocol run, artist docs. | P4 | **TN-3** pick accuracy; **TN-5** 30-minute soak with no swap > budget and no leak; docs in `docs/tonic-tool.md`. |
| **P7 GL interop (with the usdGen CUDA-display work)** | Publisher switch to `UsdGenCudaGlComputation`; drop host staging for tubes and guide previews. | OpenUSD patches restored | TN-1 re-measured; device-resident path proven by `testUsdGenTonicInterop` under the EGL harness (T2). |

Rough sizing, single engineer, given the existing GPU primitives: P0 1 wk, P1 1.5 wk, P2 1.5 wk,
P3 3 wk, P4 2 wk, P5 1 wk, P6 1.5 wk, P7 2 wk (shared). These are ASSUMPTION-tagged per the plan's
evidence rules until P0 lands and the first measurements exist.

---

## 7. Performance targets and the fallback ladder

**Reference scene ("reference-scale"):** a 60 K-face scalp, 80 L1 regions, 400 L2 tubes, 2 400 L3 tubes
(20 rings × 16 CVs → ≈ 770 K tube vertices), 12 000 guides × 16 CVs. Built by
`examples/tools/make_tonic_reference.py`.

| Gate | Tier | Assertion | Threshold | Status |
|---|:--:|---|---|---|
| **TN-1** move | T3 | press / move×N / release in Tube mode on an L1 parent (K6 over the subtree), guides at preview density | ≤ 8 ms tool-side per move, ≤ 16.7 ms frame | PROVEN at reference fanout: 1.16 ms/move over a 36-tube L1 subtree with preview guides (`testUsdGenTonicHierarchy` k6budget gate at 4 ms; the fused derive-once-per-parent K6 replaced 70 redundant subdivides per move with 6). Fixture 0.124 ms. Whole-root depth-3 moves at 85 tubes now project ≈4 ms (was 31.6); full reference-scene T3 run still open |
| **TN-2** pick | T1 | K11 at 770 K tube verts + 192 K guide CVs | ≤ 0.5 ms | PROVEN: lane 0.126 ms at 770K + production 0.125 ms at 1600+192K (`testUsdGenTonicTubes` TN-2/pick-scale gates; production reduces tube verts + guide CVs on device, small kinds on CPU) |
| **TN-3** pick accuracy | T3 | K11 vs `view.pick()` at 100 pixels | disagreements only where occluded | PROVEN (`testUsdviewTonicPick`, 3 consecutive ctest passes): 100 vert-anchored pixels, 31 agree + 69 occluded-allowed + 0 forbidden (no K11 hit ever in front of GL's surface); K11 distPx matches Gf within 0.0001 px; 10 background pixels miss both sides |
| **TN-4** swap | T3 | main-thread `TransferContent` of the reference groom | ≤ 5 ms per idle slot, zero swaps during gestures | T1 PROVEN at reference scale (`testUsdGenTonicCommit`, gate:TN-4): a full `TransferContent` is 104 ms, so the partial fallback engages and converges in 2 418 slots with a worst slot of 2.27 ms. G6 does not move this gate (it is worker-side); the worker build it feeds fell from 135.5 ms to 102.6 ms for a one-tube version (2026-09-19 note in §3.1). T3 idle-slot run unmeasured |
| **TN-5** soak | T4 | 30 min scripted editing | no frame > 33 ms attributable to the tool, no growth in device memory | PROVEN (`testTonicSoak.py`, seed 7): 4.14M ops in 30 min, 0 hard failures, p999 < 4.3 ms every op, worsts 22-31 ms, device memory flat at 1531.5 MB. One 54.5 ms topology-reject at it=1077694 is NOT tool-attributable: deterministic `--until` replay reaches the identical state (tubes=6, undoDepth=45) and times the same op at 0.190 ms (286x faster), and the out-of-process scheduler control saw a 55.1 ms stall in the same window. An earlier uninstrumented 30-min run showed 4 such transient worsts (86-517 ms); the 10-min instrumented run showed zero over-budget events in 1.38M ops. **2026-09-19: the CONTROLLER soak (`testTonicSoakController`, the same gate driven through QtTest gestures instead of the C ABI) is RED** — see the dated note below |
| **TN-6** parity | T0 | every kernel's CPU twin vs CUDA | bit-identical except transcendentals (tolerance) | PROVEN for every lane that exists: K1–K5/K8–K11 (`testUsdGenTonicGraph`, `testUsdGenTonicTubes`) and K6/K7/K8-mesh/K12/K13/K14 bit-exact in `testUsdGenTonicKernels` (2026-09-19 note below). No device lane exists for the K1 BVH build, the K3 per-face pass or G1 |
| **TN-7** bake isolation | T3 | a Tube-mode drag (TN-1 script) while a full bake of the reference scalp is in flight | no move exceeds TN-1 by more than 0.5 ms; zero UI-thread time in the bake beyond the one-attribute swap | PROVEN (`testUsdviewTonicBakeIsolation`, gate:TN-7, three consecutive ctest runs and eight consecutive full-suite runs): the loaded drag's median move is 0.700-0.770 ms against a 0.591-0.676 ms unloaded baseline measured in the same run, a median delta of +0.094 to +0.127 ms and a worst-move delta of -0.047 to +0.376 ms, all inside the 0.5 ms allowance and an order of magnitude inside TN-1's own 8 ms; all 50 of 50 moves ran with the bake in flight |

> **2026-09-19 (plan/18 V7, TN-7 bake isolation).** Measured, and green.
> `testUsdviewTonicBakeIsolation` drags a center-CV gizmo through 50 real
> `QtTest` mouse moves twice on `examples/tonic-reference.usda`: once idle,
> once with a full reference-scalp bake in flight. The forced bake runs
> 967-1029 ms against a 37-39 ms drag, so every move of the loaded run
> overlaps it, and the script fails rather than passes if fewer than 8
> do. Four consecutive runs (the last full-suite pass, then three
> standalone): loaded median
> 0.700/0.757/0.708/0.770 ms against baselines 0.591/0.630/0.600/0.676,
> worst loaded move 1.239/1.097/1.085/1.293 ms. The drain at the end
> also checks the only UI-thread cost TN-7 allows, the one-attribute
> map swap, actually happens: the idle pump lands map v5.
>
> Two things had to be fixed before the gate meant anything. The bake is
> forced big through the Output panel's texel resolution override (the V7
> wiring below), because at the automatic resolution the 64-face stub
> reference scalp bakes in under a millisecond and the drag would never
> overlap it. And the override has to be set BEFORE the graph edit that
> triggers the bake: a gesture's release already enqueues one
> (`GraphLoop.endOfEdit`), the worker tracks progress by map version
> alone, and a second enqueue of the same version therefore reads as
> "idle" the moment the release's own bake lands. The first version of
> this script rebaked after the release and measured 0 of 50 moves
> overlapping while the worker was in fact busy.
>
> **2026-09-19 (plan/18 V7, TN-5 through the controller).** RED, with two
> product bugs found and fixed on the way and one open measurement
> problem. `testTonicSoakController` had never run to completion: the
> first 30-minute ctest run died at 150 s (SEGFAULT) and a 1-minute run
> reported 196 over-budget ops, `hover` alone climbing from 80 ms to
> 1150 ms. It runs the full 30 minutes now.
>
> * **The guide preview asked Storm for a shader it cannot compile.**
>   `/__usdGenTonic/guides/L<n>` binds `UsdGenHairPreview`, which reads
>   the ribbon orientation vector `inData.Neye`, and stated no refine
>   level of its own — so at usdview's default complexity Storm drew the
>   WIRE repr, whose curve vertex block has no `Neye` member, and the
>   material failed to COMPILE. Storm retries a failed compile on every
>   draw, so this was not a missing shade but a per-frame cost. The prim
>   now publishes `displayStyle/refineLevel = 2`, the level whose repr
>   carries the vector (the tile pipeline solves the same problem from the
>   other end, by hiding the binding below that level). A 1-minute soak
>   went from 376 `Failed to compile shader for prim
>   /__usdGenTonic/guides/L1` warnings to zero, and the tonic goldens are
>   unchanged.
> * **The workspace dock leaked a shelf per mode switch.** Rebuilding the
>   sub-mode shelf and the action buttons dropped the old ones with
>   `deleteLater()`, and a `DeferredDelete` event is only delivered by a
>   running event loop — which a T3 script, running between turns of
>   usdview's loop, never gives it. Under the main window,
>   `QToolButton`/`QPushButton`/`QButtonGroup` grew 3 770 → 6 897 objects
>   in 45 s and every later event dispatch paid for them: per-op cost
>   climbed 180 → 550 ms. The dock now retires them by hand (dropped from
>   the parent at once, freed at the next rebuild), which needs no event
>   loop and never destroys a widget inside its own callback. Objects are
>   now flat at ~860 for a whole run and throughput went from 2 458 to
>   7 119 iterations per 105 s.
>
> Attribution was wrong as well, and is now fixed. A script runs BETWEEN
> turns of usdview's event loop, so the events every op posts (the idle
> pump's timer, the redraw request, the dock's refresh) pile up until
> some op spins the loop — QtTest's mouse move does — and that op is
> charged for all of them, which is why `hover` alone looked
> catastrophic. Each op now drains its own posted events inside its own
> timing, which is what the artist's running loop does anyway, and the
> cost spreads out honestly. In a one-minute run (seed 7, 2 091
> iterations) p50 per op then runs from 7.1 ms (`hover`) to 48.8 ms
> (`sculpt-stroke`), with the worst committer swap at 2.7 ms.
>
> **The full 30-minute run now completes** — it no longer dies at 150 s
> — and it is still red on two gates. 23 222 iterations, seed 7:
>
> | | |
> |---|---|
> | hard failures / rejects | 0 / 0 |
> | device memory | 1 665.7 → 1 647.1 MB (no growth) |
> | ops over the 33 ms budget | 16 167 of 23 222, 13 explained by the control |
> | p50 per op | 12.5 ms (`hover`) … 95.9 ms (`topology`) |
> | worst committer swap | 26.3 ms, against TN-4's 5 ms |
> | throughput | 550 iterations per 15 s at the start, 107 at the end |
>
> Two things are left, and neither is the per-op attribution above.
> First, the 33 ms figure is being applied to a whole multi-sample
> gesture plus whatever the event loop owes it, where TN-5 words the
> budget per FRAME; each op carries roughly 20 ms of deferred work that
> is not yet named. Second, and more serious: **something still slows
> down over the run** — throughput falls 5x and the committer swap
> reaches 26 ms — while device memory is flat and the Qt object count,
> the tube count and the undo depth (capped at 50) are not growing. It
> is host-side and unidentified. The two suspects worth probing first
> are the per-version map files the bake leaves behind (a graph edit per
> ~12 iterations means thousands of `regionMap.v<n>.ptx` versions over
> 30 minutes, and usdGen's image-map cache is keyed by path) and the
> live layer the committer swaps into, which the worsening swap points
> at directly.

> **2026-09-19 (plan/18 V9-perf, dock cost + the per-frame reading).**
> Two of the three V9-perf items landed; the third did not start. First,
> the dock stopped re-reading the model on every publish: `refresh()` is
> called on every publish, every idle pump and a 250 ms timer — about
> four times per artist op in the controller soak, where the full
> warnings re-read alone (three bounded ABI reads, 4 096 smoothness
> scores among them) cost 2.6–5.9 ms per op. `tonicHud.warningsKey` is
> the cheap signature of everything `warnings()` reads (model version,
> fallback, committer attachment); the dock skips the re-read and the
> widget rebuild on an unchanged key, re-reads at its own 250 ms cadence
> at most, defers all content past a live gesture, and skips the rebuild
> when the texts did not change. The soak meters each part
> (`dock-refresh`, `dock-warnings`, `dock-params`, `dock-strip`) and a T0
> pins the key contract (`testUsdGenTonicToolsPanels`). Second, the
> per-frame reading V7 asked for is opt-in:
> `USDGENTONIC_SOAK_PACE=1` drains the event loop after every input
> event, so each event is timed as one frame, and reports the per-frame
> figures beside the default whole-gesture reading; the gate does not
> move with the switch, because the paced reading changes the workload
> (it un-compresses the input Qt would compress) and the two readings
> bracket the truth instead. Third, the committer-swap regression and
> the host-side growth behind it were not touched — no C++ changed in
> V9-perf, no new 30-minute run exists, and both controller-soak gates
> stay red exactly as V7 left them. The suspects above (live layer, then
> versioned map files against the path-keyed image-map cache) stand.

> **2026-09-19 (plan/18 G11, hierarchy device lanes).** K6, K7 and K14 now
> have batched CUDA lanes beside their CPU twins (`tonicKernels.{h,cu}`:
> `TonicSubdivideTubesDevice`, `TonicParentAverageDevice`,
> `TonicHierarchicalSculptApplyDevice`), and the three printf stubs in
> `testUsdGenTonicKernels` are real parity tests over the reference fanout
> (a 36-tube L1 subtree: 6 parents, 35 children). **All three lanes are
> bit-identical to their twins** -- worst |device - cpu| is exactly 0 across
> every center CV and ring CV -- so the TN-6 row above is no longer PARTIAL
> for these three. The K8 mesh lane, which had no test and no caller, is
> proven too (bit-exact over 48 roots).
>
> Which lane production takes is now a measurement, not a guess. At the
> reference fanout on an RTX 4090 (sm_89, CUDA 12.6), 20 repetitions,
> device figures including upload, launch and readback over a reusable
> device arena:
>
> | Kernel | CPU twin | Device lane | Production |
> |---|---|---|---|
> | K14 subdivide, 6 parents -> 35 children | 0.65 ms | 2.23 ms | CPU twin |
> | K7 parent average, 8 groups | 1.74 ms | 0.29 ms | **device lane** |
> | K6 sculpt apply, 35 children | 0.16 ms | 0.88 ms | CPU twin |
>
> K14 loses because its per-child work is a serial double-precision
> polygon clip (Sutherland-Hodgman plus the arc-length pad, one FP64 sqrt
> per edge per step) over only 35 children: sm_89 runs FP64 at 1/64 rate
> and there is no width to hide it. K6 loses to the fixed cost of moving
> four packed tube batches across PCIe for ~0.16 ms of arithmetic. K7 wins
> because its twin is the expensive one (per-section union refit with an
> angular sort) and the batch gives one launch for every group.
>
> **TN-1 re-measured with the lanes wired: 1.30 ms per move over the
> 36-tube L1 subtree** (three consecutive runs: 1.297, 1.304, 1.303),
> against 1.25 ms for the all-CPU baseline on the same box. The gate is
> unchanged in practice, and that is the expected result: the per-move
> chain's K7 refresh goes through `TonicMergeTubesCpu`, whose hint path is
> a K14 re-derivation (CPU-favourable), while the K7 device lane is wired
> at `MergeSelected` and `GroupTubes`, which are gesture-end actions. A
> K7 win on the move path would need the up-propagation batched across
> ancestors instead of one group per call.
>
> K8/K9/K10 keep their CPU twins in production for a correctness reason,
> not a speed one: those lanes agree with their twins only to a tolerance
> (sqrt/cos/sin in the dart stream), while the committer and hydrate
> compare generated guides bit for bit (`tonicModel.h`,
> `TonicGenerateGuides`), and the committer runs on a worker with no CUDA
> mirror. A mixed lane would break that equality. K13 smoothness and K12
> root intersection are bit-exact and are now dispatched to the device
> whenever the mirror is healthy, with the twin as the fallback.

**Fallback ladder** (applied automatically when a move exceeds TN-1, restored on release):

1. Guides preview density 25 % → 10 % → 0 % (tubes only).
2. Tube rings/segments halved for tubes outside the edited subtree.
3. Non-edited hierarchy levels drawn as center curves only.
4. Hover highlighting off.

The ladder degrades only what is drawn; the model is always edited at full fidelity, and the release
frame always shows full fidelity.

> **2026-09-19 (plan/18 V5, the ladder is real).** The ladder existed only
> as this list: nothing measured a move, so nothing could step it (the
> audit's G15 row, "ladder has no trigger"). It is now
> `tonicLadder.FallbackLadder`, Qt-free, driven by the viewport
> controller, which times each move END TO END -- the loop's model work,
> `Tonic_Publish` and the `UpdateViewport` request -- and hands the
> milliseconds over.
>
> **Trigger: three CONSECUTIVE moves over 8 ms step one rung.** One slow
> frame never costs fidelity (a committed layer swapping in, a resize),
> and a drag that is genuinely too heavy walks down a rung roughly every
> fifth of a second at a 60 Hz sample rate. A single move inside the
> budget resets the run. Each rung is one ABI write, and release restores
> every value the ladder touched from the base it captured at press, then
> republishes `TonicDirty_All`. The position is `ladderStep` on the tool
> state and in `TonicSession.status()`, which is what the dock's status
> strip prints.
>
> The six rungs are the four above, spelled one ABI call each:
> preview 0.25 -> 0.10 -> 0 (`Tonic_SetPreviewFraction`), display segments
> halved (`Tonic_SetDisplaySegments`), non-edited levels to centers only
> (`Tonic_SetLevelDrawMode`, new in V5: `Tonic_SetLevelDisplay`'s
> visible/x-ray pair cannot say "centers only", and hiding a level takes
> its center curves with it), hover off.
>
> **One divergence from rung 2 above, recorded rather than quietly
> dropped:** "halved for tubes outside the edited subtree" is not
> expressible -- `Tonic_SetDisplaySegments` is one number for the whole
> model -- so the halving is global. The edited subtree keeps its fidelity
> at the rung that can say so: rung 3 leaves the FOCUSED level alone and
> takes every other level down to its center curves.
>
> **TN-1 measured through the real controller** (RTX 4090, sm_89, CUDA
> 12.6; `status()["lastMoveMs"]` at the end of each T3, so it includes the
> publish and the refresh request):
>
> | Gesture, through the installed event filter | ms/move | Budget |
> |---|---:|---:|
> | Hierarchy select + Shift+D + level walk (`testUsdviewTonicLevels`) | 0.14 | 8 |
> | Sculpt Grab stroke over a 5-CV tube (`testUsdviewTonicSculpt`) | 0.76 | 8 |
>
> At the fixture scale the ladder never leaves step 0, which is the right
> answer and is asserted as one: both T3s finish at `ladderStep 0`. The
> figure that matters for the trigger is the headroom -- a sculpt move
> spends 9.5 % of its budget here, so the ladder starts working at roughly
> a tenfold heavier scene, which is the reference groom, not the fixture.
> The rungs themselves are proven on synthetic timings in
> `testUsdGenTonicToolsLoopsHier.py` (three slow moves step, two do not, a
> fast move in between resets, and release restores every value).

> **2026-09-19 (plan/18 V4, Tube + Fill loops).** **TN-1 re-measured through
> the real controller**, which is what the V4 row asks for: 50 REAL QtTest
> mouse moves dragging a center CV's translate gizmo in Tube mode
> (`testUsdviewTonicTube.py`), each timed by `ViewportController.onMove`
> end to end -- the per-tube `Tonic_MoveTubeCenterCV` (K6 down the
> subtree), the preview-density `Tonic_RefillGuides`, `Tonic_Publish` and
> the `UpdateViewport` request. Three consecutive ctest runs:
>
> | Run | Median move | Worst move |
> |---|---|---|
> | 1 | 0.609 ms | 1.090 ms |
> | 2 | 0.741 ms | 0.975 ms |
> | 3 | 0.665 ms | 0.938 ms |
>
> That is the FIXTURE groom (one L1 tube, 5 center CVs, 128 guides at the
> 25 % preview fraction), so it measures the controller's own overhead
> rather than the model's: the reference-fanout number stays the T1
> `k6budget` gate above (1.30 ms over a 36-tube subtree). The two together
> say the tool-side path costs well under a millisecond on top of whatever
> the model does, and the 8 ms budget is not in question at this scale.
> Guides during the drag follow plan/17 §4.2 exactly (preview fraction on
> press, refill per move, stored fraction + full refill on release).
>
> Two things V4 found and did NOT fix, both recorded on the plan/18 G14
> row. **Both are closed in V6** — see the 2026-09-19 note in §5.1; the
> two bullets below are kept as the record of what V4 saw, not as current
> behaviour:
>
> * **A groom still has one L1 tube.** `Tonic_BuildTubeFromRegion`
>   *rebuilds* tube 0 rather than appending, so the "each closed region
>   gets a tube stub" rule of §5.1 is honoured for the FIRST closed region
>   only (`TonicSession.ensureRegionTubes`, called on every graph release,
>   builds a stub when the groom has no tube and does nothing when it
>   has). A second region cannot have its own stub until the model grows
>   multiple L1 roots, which is a `tonicModel` change, not a tool one.
> * **That first stub clears the undo stack**, because
>   `BuildTubeFromRegion` calls `_ClearUndoLocked` ("a rebuild invalidates
>   pre-mutation snapshots"). The artist's graph strokes up to that point
>   stop being undoable. The fix is for the build to push one undo entry
>   instead of clearing when there was no tube to invalidate, and for
>   `HierarchyRollback` to carry `_tubeRegionId` (it does not today).
>
> One ABI entry was added for the Ring/Section gizmos:
> `Tonic_GetTubeSectionFrame` (`tonicApiStage.{h,cpp}`,
> `tonicLibStage.sectionFrame` + `worldToChart`). A gizmo drag arrives in
> world units and `Tonic_MoveTubeSectionRing` takes (du, dv) in the ring's
> own two-dimensional chart; the conversion needs the K4 frame at the
> ring's t, and nothing else exposed it. `testUsdGenTonicToolsStage.py`
> proves the round trip: convert a world delta, push it through the
> per-tube ABI, and the ring's centroid moves by exactly that delta.
> Because the chart has no third axis, the w handle of a ringTRS gizmo
> twists instead of translating.

---

## 8. Testing

* **T0** (`tests/testUsdGenTonicKernels.cu` + CPU twins): every K-kernel, parity, determinism per
  seed, region extraction on synthetic graphs, length preservation, parent averaging, K14
  subdivide/merge round trip and volume conservation, top-down then bottom-up propagation
  converging in one pass (no ping-pong between K6 and K7).
* **T1** (`tests/testUsdGenTonicIndex.cpp`, `testUsdGenTonicCommit.cpp`): scene-index publication
  and leaf-exact dirties; committer coalescing, cancellation, no-swap-during-gesture, round-trip
  bit-equality; the `GuideInterpolate` relationship fill-in; hydrate from a hand-authored groom;
  `testUsdGenTonicRegionBake` (the baked `.ptx` read back through a real `ptex()` expression on a
  `GuideInterpolate` and, per level, on a `Clump`, versioned-file swap safety).
* **T3** (`plugin/usdGenTonicTools/testenv/*.py` via `testusdview`): one script per mode, plus TN-1,
  TN-3, TN-4. Same interpreter rule as `testUsdGenToolsExprEditor` (`PY` or the shebang).
* **Image checks**: `examples/tonic-*.usda` recorded through `bin/record_usd.ps1` at `-Complexity
  veryhigh`, compared with golden PNGs (`tests/golden`).
* **Examples**: `tonic-ponytail.usda` (one region, one tube, fill), `tonic-braid-hierarchy.usda`
  (L1/L2/L3 with an imported L3), `tonic-reference.usda` (the §7 scene, generated).

---

## 9. Risks and open decisions

| Id | Risk / decision | Mitigation / owner |
|---|---|---|
| R1 | Host-staged transport could dominate at > 1 M tube verts | Ladder step 2; P7 interop; measure at P3 before widening the reference scene |
| R2 | `TransferContent` cost scales with prim count, not with delta size | Per-tube `SdfCopySpec` partial transfer (§3.1); keep tubes as few, fat prims (arrays, not per-ring prims) |
| R3 | Hierarchical sculpt formulation is a reconstruction, not Tonic's | Documented as ours in §4.1 K6; artist review at P4; parameters exposed (frame choice, arc-length policy) |
| R4 | Fill kernel is ours (Gap G4) | K9 is pluggable; `edgeBias`/`lengthProfile` cover the published behaviour (center profile + tube extent) |
| R5 | Two scene indices both publishing under a description | Tonic prims live under `/__usdGenTonic/`, never under the description; only the guide prims are shared, and they are stage prims |
| R6 | Scalp mesh animated (usdRig/UsdSkel upstream) | Graph binds to `(faceId, uv)`; K1 BVH refits per frame; editing while scrubbing aborts the gesture (as `08-tools.md` §10) |
| R7 | Without `_usdGen` the Python array transport is ctypes + numpy | Acceptable for P0–P3 because arrays cross Python only for gizmo/HUD data, not for geometry; geometry goes C++ → Hydra directly |
| D-open-1 | ~~Region map default: primvar vs Ptex~~ **Resolved 2026-09-18**: the Ptex is the contract and is baked by the commit worker (versioned files); the primvar is the live preview | §2.2, §3.1, §4.5 |
| R8 | The Ptex bake (~100–200 ms first bake) lands after the stage swap that carries the same graph change, so the amplified preview can briefly cook against the previous map | Accepted by design (§3.1a): the bake is its own async pipeline and never gates a swap; the HUD shows map/stage version skew in amber; GuideInterpolate's nearest-guide fallback covers unmapped roots in the window |
| D-open-2 | Live sublayer under the session layer vs the edit target | Proposed session sublayer; "Save groom" is the only file write |
| D-open-3 | Whether Sculpt-mode brushes may also touch amplified hair | Proposed no; that is M5's shelf and its synchronous contract |

---

## 10. Relationship to the existing plan

* `08-tools.md` remains the spec for brushes over amplified hair (M5). Its C ABI (`cApi.h`), the
  `_usdGen` array module and the CPU CV picker are still wanted; this overlay does not depend on them
  to ship P0–P4.
* `14-hierarchy-cuda-implementation.md` / `15-resource-aware-execution.md` own the usdGen CUDA lane.
  `usdGenTonic` reuses `gpu/` primitives (`DeviceBuffer`, picking, root frames) but never a usdGen
  generation; its display does not wait on the CUDA-display handoff, which is why P7 is last.
* `13-codebase-alignment.md` should gain a row: "Tonic authoring tool (overlay 17): Not started."
* Milestone placement: propose **M5b** after M2 (SculptLayer + Freeze) and in parallel with M5, since
  its only hard dependency on the engine is the guide contract that already exists.
