# 17-P1 — Committer design (schema + async commit pipeline)

Date: 2026-09-18. Parent: `17-pomade-authoring-tool.md` §2.2 (stage schema),
§2.5 (hydrate), §3 (commit pipeline), §6 P1 exit. Scope: P1 only — schema
additions, `PomadeCommitter` (worker + idle swap), hydrate, "Save groom", and
the `GuideInterpolate` relationship fill-in. Graph/fill/hierarchy kernels
(P2–P4) are out of scope; the design carries them without a second code path.

This doc is DESIGN. The code owner implements
`libs/usdGenPomade/usdGenPomade/pomadeCommit.{h,cpp}`; the CMake owner wires the
build (§7). This doc touches neither.

---

## 1. Schema additions (done, verified)

`libs/usdGenSchema/schema.usda` gains four codeless types under one
"Pomade authoring" banner. Authoring-side only: no usdGen kernel reads them;
the committer writes them and hydrate reads them back.

| Type | Base | Contents (plan/17 §2.2) |
|---|---|---|
| `UsdGenPomadeGroom` | `Xformable` | rel `usdGen:pomade:scalp`, rel `usdGen:pomade:description`, token `usdGen:pomade:version = "1"` |
| `UsdGenScalpGraph` | `Boundable` | `nodeFaceIds`, `nodeUVs`, `edges`, `regionNodeCounts`, `regionNodeIndices`, `regionColors`, `linkedRegions`, `snapRadius = 0.01` |
| `UsdGenTube` | `Xformable` | `regionId`, `level = 1`, `centerPoints`, `sectionT`, `sectionCvCount = 8`, `sectionCvs`, `childIndex = -1`, `centerDeltas`, `sectionDeltas`, `subdivide:count = 4`, `subdivide:seed`, `subdivide:splitMode = "kmeans"` (`kmeans`\|`edge`), `fill:density = 100.0`, `fill:cvCount = 8`, `fill:lengthProfile`, `fill:seed`, `fill:edgeBias = 0.0`, `locked`, `lockParents`, `lockChildren` |
| `UsdGenTubeHierarchyAPI` | `APISchemaBase` (single-apply, never auto-applied) | rel `usdGen:pomade:members`, bool `usdGen:pomade:persistent = false` |

C1 rules: every attribute carries `doc` ending in an "Expression evaluation:
groom" sentence plus `customData.usdGen.evaluation = "groom"`, restored into
the generated schema by `restore_generated_metadata.py`. Relationships and
`allowedTokens` tokens carry `doc` only, matching the existing convention
(e.g. `UsdGenDescription` rels, `usdGen:curve:basis`). No dead knobs: every
attribute is read by the hydrate path (§5).

Two deliberate deviations from the §2.2 text block, both recorded here:

1. `sectionCvs` / `sectionDeltas` are `float2[]`, not the `point2f[]` §2.2
   writes. `point2f` is not a USD type (`Sdf.ValueTypeNames` has no
   `Point2f`; probed against the usdRig install, 2026-09-18). `float2[]`
   matches the file's existing 2D convention (`nodeUVs`, ramp knots).
2. `usdGen:pomade:fill:edgeBias` is declared although the §2.2 block omits
   it. The parameter exists in §2.1 `FillParams`, §4.1 K9 and §5.3; declaring
   it now keeps the fill record whole and hydrate-complete. Range [-1, 1],
   default 0 (uniform).

### Regen record

Exact command (from the repo root, PowerShell):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File bin/gen_schema.ps1
```

Result 2026-09-18: SUCCESS. `usdGenSchema` processed all 47 classes
including the four new ones, wrote
`plugin/usdGenSchema/resources/{plugInfo.json,generatedSchema.usda}`,
restored evaluation metadata on 281 properties and full doc on 387, and the
post-process step completed ("regenerated plugin/usdGenSchema/resources").
Re-running over the already-regenerated tree is stable (identical diff:
`generatedSchema.usda` +415/−10, `plugInfo.json` +32). No toolchain failure;
no blocker. Uses the default USD prefix `D:\work\usdRig\usd-install`
(`$env:USD` unset) and the Python 3.10 on PATH. (The `__init__.py` /
`CMakeLists.txt` / `module.cpp` warnings and the
`USD_DISABLE_PRIM_DEFINITIONS_FOR_USDGENSCHEMA` banner are pre-existing
noise, not errors.)

---

## 2. Pipeline shape (plan/17 §3.1)

```
UI thread                 commit worker                     UI thread (idle)
release → Enqueue(v) ──► snapshot(v) → build SdfLayer L_v ──► SwapIfIdle()
                            (anonymous, on no stage)          TransferContent(live ← L_v)
                                                              in one Sdf.ChangeBlock
                                                              UpdateViewport()
```

Thread rules (hard): the worker never touches Qt, `UsdStage`, the bake, or
`pomadeStream`'s UI waits; the UI thread never serialises, cooks, or waits on
CUDA. Device→host guide copies happen at snapshot time on `pomadeStream`
behind a CUDA event the worker waits on. Only the committer TU links `usd`
(hydrate + save run on the UI thread); gate B-1 stays true for `usdGen`
itself.

Owned subtree: the committer owns every prim under `groomPath`
(`<groom>`, `<groom>/Tubes/...`, `<groom>/ScalpGraph`, `<groom>/Guides`,
`<groom>/RegionMap`, `<groom>/RegionExpr`, per-level maps/exprs) plus the
`GuideInterpolate` op under the linked description — and nothing else
(`PomadeCommitPaths`).

## 3. Enqueue: versions, not data

`Enqueue(stage)` (UI thread, at release) stores only a version number plus a
`PomadeFillPlan` captured from the composed stage (§6). The worker takes the
*latest* pending version when it wakes, so ten fast strokes produce one
layer build (**version coalescing**). `EnqueuePlan(plan)` is the ctypes
variant: Python plans over pxr and passes flags, since no `UsdStage` crosses
the C ABI.

The worker snapshots the live model itself under the model's mutex at build
time (`PomadeModel::Snapshot` → `PomadeSnapshot`: plain data, no model access,
safe to move). The snapshot always matches the version it builds; host
mirrors are refreshed lazily by version and the model tracks a dirty set per
version so the snapshot copies only changed tubes.

## 4. Build + swap + cancellation

**Build** (`PomadeBuildCommitLayer`, worker): writes a fresh anonymous
`SdfLayer` with `SdfCreatePrimInLayer` + typed `SdfAttributeSpec` field
sets. No `UsdStage` involved — `Sdf` is safe for independent layers. Every
prim the committer owns is rebuilt from the snapshot, so a swap never
carries stale opinions: groom + `Tubes` + `ScalpGraph` shell + `Guides` +
`RegionMap` shell + `RegionExpr`, plus the `GuideInterpolate` op exactly as
the snapshot's fill-in flags say. P1 commits L1 tubes and the empty graph
shell; P2 fills the shell, P4 adds deeper levels — same builder.

**Swap** (`SwapIfIdle(live, gestureActive)`, UI thread, from a Qt idle timer
only): `SdfLayer::TransferContent(live, L_v)` inside one `SdfChangeBlock` —
a single change-processing pass that resyncs the tool's subtree once. Skip
rules, in order:

1. `SkippedGesture` — a gesture is active; the swap waits. Zero swaps
   during gestures (TN-4 second half).
2. `SkippedStale` — a newer version is already being built (`_building`
   past `_readyVersion`); the worker will hand a newer layer soon.
3. `NothingPending` — no built version newer than live.

**Cancellation** is by supersession, at two points: a built layer replaced
in `_ready` by a newer build before any swap counts as dropped
(`_droppedCount`, never lands); a cook older than the latest swap carries a
cancellation token on the description's session so it stops early (existing
"superseded mid-run keeps the previous generation" behaviour). A worker
throw is caught: log, keep the previous live layer, red status line, model
unaffected.

**Partial-transfer fallback** (over-budget swaps): when a measured full swap
exceeds the budget (default 5 ms, `SetSwapBudgetMs`), the committer copies
one child-prim subtree per idle slot (`SdfCopySpec` per tube:
`PartialProgress` until converged). Every slot stays under budget at the
cost of a few frames of stage staleness — acceptable because the viewport
shows the model, not the stage. Mitigates R2 (cost scales with prim count);
tubes stay few, fat prims (arrays, never per-ring prims).

**Cook interaction** (plan/17 §3.2): "show amplified hair" is off during
gestures and swaps are held back, so no cook is even requested until the
artist pauses. The tool never calls `UsdGenImaging_Commit()` — the cook is a
side effect of the swap.

## 5. Hydrate + bit-equality (plan/17 §2.5)

`PomadeHydrateModel(stage, groomPath, model)` (UI thread) builds `PomadeModel`
from a stage that already holds a `UsdGenPomadeGroom`:

1. Read the scalp graph (P2; P1 reads the shell) and the L1 tube(s) under
   `<groom>/Tubes` into the model.
2. Regenerate the guides from tubes + fill params + seed with the same
   kernels, and assert **bit-equality** with the stored `<groom>/Guides`
   points. A stage the tool produced always round-trips, so a mismatch is a
   hard failure (`ok = false` + diagnostic), never a silent accept.
3. Same rule for the graph: extracted loops and the re-rasterised live
   primvar must equal the stored opinions bit-exactly (`graphRoundTrip`).

Foreign guides (a DCC, hand-authored) fail step 2 by design; P5 imports
those as L3 locked tubes instead of fill output. Result struct reports
`guidesBitEqual`, `graphRoundTrip`, counts, and a diagnostic string.

Proven by `testUsdGenPomadeCommit`: hydrate round-trips the model
bit-exactly (tube + guides).

## 6. GuideInterpolate fill-in: empty connections only

`PomadePlanGuideInterpolateFill(stage, paths)` runs on the UI thread at
enqueue time against the COMPOSED stage (the worker must not read the
stage); the resulting flags ride the snapshot. The tool never rewrites an
operator stack an artist has hand-authored; it only fills in what is empty:

* No `UsdGenGuideInterpolate` under `<desc>/Ops` → create one (named by
  `InterpOpPath()`) with `usdGen:guides → <groom>/Guides` and
  `usdGen:region → <groom>/RegionExpr` (prim-path connection, resolved to
  `outputs:result`).
* An op exists → set `usdGen:guides` only when it has no targets; connect
  `usdGen:region` to the `RegionExpr` only when it has no connection.
* Missing description (or empty `descriptionPath`) → no fill-in.

Related, P2: when a `UsdGenClump` exists and its `usdGen:clump:map` is
unconnected, the tool OFFERS `RegionExprL<n>` for the deepest level
(`PomadePlanClumpFill` returns the offer) — it never writes the connection
itself. Per-level `RegionMap`/`RegionExpr` prims share the versioned `.ptx`
with `firstChannel = level − 1` (plan/17 §4.5).

Proven by `testUsdGenPomadeCommit`: the fill-in creates the op only when
absent and only touches empty relationships/connections.

## 7. Save groom

`PomadeSaveGroom(stage, live, filePath, err)` (UI thread):

1. Copy the live layer's content into the file layer at `filePath` (created
   when missing; **`.usdc`, never `.usda`**, per S42) and save it.
2. Re-parent the session sublayers to `[live, file]` — the live layer stays
   strongest so editing continues there; the file layer beneath holds the
   saved groom.

Any failure returns false with `*err` set and leaves the stage untouched.
`PomadeSaveGroomAndMaps` (P2) adds the bake side: copy the latest baked
version beside the saved layer as `regionMap.ptx` (caller runs this off the
UI thread for large maps) and repoint `usdGen:map:file` on
`<groom>/RegionMap` in the SAVED file layer; the live layer keeps its
versioned reference untouched. "Save groom" (a layer re-parent) is the one
stage-level action undoable through the stage, via the `08-tools.md` §7.3
recorder; all other undo is on the model (`PomadeUndo`, plan/17 §3.3).

---

## 8. Required build wiring (text for the CMake owner — not applied here)

P1 needs no new targets beyond what P0 created; it needs the committer TU,
its test, and the schema dependency kept in step:

1. `usdGenPomade` SHARED glob already covers `pomadeCommit.cpp`
   (`file(GLOB_RECURSE USDGEN_POMADE_SOURCES libs/usdGenPomade/*.cpp)`).
   Keep it in the glob; keep `usd`/`usdGeom` link-private to this lib so
   gate B-1 stays true for `usdGen` itself. No new link deps for P1
   (Sdf/Usd come from the existing `hd sdf tf gf vt` / `usd usdGeom` set).
2. `testUsdGenPomadeCommit` (T1): `add_executable` on
   `tests/testUsdGenPomadeCommit.cpp`, linked `PRIVATE usdGenPomade usd sdf`,
   registered with `ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}"` (it needs
   the schema plugin for the typed groom prims, like every
   adapter-dependent T1 test), `LABELS "T1;pomade;gate:TN-4"`, `TIMEOUT 300`.
3. Schema gate: the pomade types must stay covered by the schema-uptodate
   check (`generatedSchema.usda`/`plugInfo.json` in step with
   `schema.usda`); regen command is §1. The P1 test asserts the four type
   names resolve in the live registry.
4. No P1 CUDA: `pomadeKernels.cu` joins in the `USDGEN_ENABLE_CUDA` branch
   (P2+); the P1 pipeline builds and passes with CUDA off.

## 9. P1 exit checks (for the test owner)

* `testUsdGenPomadeCommit` green: one-swap landing, coalescing (N enqueues →
  one build), cancellation (superseded build never lands), no swap during a
  gesture, no stale swap while a newer build is in flight, partial fallback
  converges to identical content, hydrate bit-equality, fill-in
  empty-only, Save-groom `.usdc` + re-parent, C ABI parity.
* TN-4: reference-scale swap ≤ 5 ms per idle slot (measured in the test).
* Schema: regen clean (§1), no dead knobs (every new attribute read by
  hydrate), C1-style metadata on every property.
