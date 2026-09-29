# 18 — Pomade as one viewport tool (overlay on plan/17)

Date: 2026-09-18. Status: **implemented 2026-09-20** (V0–V9), supersedes the UI parts of plan/17 §5
and the P0 "mode shelf" as built. Everything in plan/17 §1–§4 (model, commit pipeline, kernels,
transport, schema) stays in force; this document says how the artist reaches it from usdview. See
§8 for what landed per phase and §7's coverage table for what closed each numbered gap.

## 0. Review verdict: what exists, what does not

The C++/CUDA side of plan/17 is substantially built (see §7 for the item-level audit). The
usdview side is not a tool. It is a list of 40+ flat menu commands, and the geometry the artist
edits is never drawn. Findings, in severity order:

| # | Finding | Evidence |
|---|---|---|
| F1 | **Two models, no bridge.** `Pomade_Create` allocates a `PomadeModel` inside the DLL (`pomadeApi.cpp:23`, `PomadeModelContextImpl`). `UsdGenPomadeSceneIndex` allocates *its own* `PomadeModel` in its constructor (`pomadeSceneIndex.h:110`) and only ever publishes the static test tube (`USDGENPOMADE_TEST_TUBE`). No code path connects the two. Every graph stroke, tube edit, subdivide and sculpt the plugin performs is invisible in the viewport. | `Refresh()` is called only from the index constructor and `testUsdGenPomadeIndex.cpp`; the plugin never calls into the index. |
| F2 | **The index publishes one tube.** `_published` is a single `PomadeStagedTubeMesh` at `TestTubePath()`; the model's `_tubes` map (hierarchy children, groups, imports) has no publication path. Level colours, x-ray and the selection rim exist in `pomadeTube.glslfx` but are fed constant `selected = 0`, `xray = 0`. | `pomadeSceneIndex.cpp:758-800` |
| F3 | **Only Graph mode has a viewport loop.** `pomadeGraphUI.GraphController` is the only event filter. Tube, Fill, Hierarchy, Sculpt and Output are Qt-free helper modules plus ctypes calls exercised by T3 scripts that bypass the viewport. No gizmo, drag, hover, marquee or K11 pick is wired to the mouse. | `pomadeTube.py`, `pomadeSculpt.py`, `pomadeHierarchy.py`, `testUsdviewPomadeTube.py` header |
| F4 | **Menus instead of a tool.** 6 mode items + 7 graph + 4 graph actions + 5 hierarchy + 8 hierarchy actions + 5 sculpt + 2 sculpt actions, all under `usdGen → Pomade → …`. `Mode.hotkey` is declared and never installed. No dock, no shelf, no parameter panel: `subdivideCount`, `previewFraction`, `brushRadiusPx` etc. exist on `PomadeToolState` with no widget that edits them. | `__init__.py:configureView` |
| F5 | **Stale "not implemented" text.** Hierarchy actions print "Needs the P4 C ABI (Pomade_SubdivideTube)" although the ABI exports it and `pomadeHierarchy.py` binds it. | `__init__.py:_makeHierarchyActionCallback` |
| F6 | **Surface pick per move through Hydra.** Draw mode calls `view.pick(x, y)` on every stroke sample (≈1.3 ms each, plan/08 §6.1 says once per press). `Pomade_Raycast` (K1) exists and is unused by the UI. | `pomadeGraphUI._surfacePick` |
| F7 | **Idle pump only on release.** `_pumpIdle` runs once after a gesture; nothing polls for the bake worker's completed files or a coalesced commit that lands later, so the stage and the map go stale until the next gesture. | `pomadeGraphUI._releaseCommon` |
| F8 | **No undo hotkey, no redo.** `Pomade_Undo` exists; nothing binds `Ctrl+Z`; there is no `Pomade_Redo`; graph edits are outside the undo stack (docs say so). | `pomadeApi.h`, `docs/pomade-tool.md` "Undo" |
| F9 | **Escape does not cancel.** plan/17 §1.3 requires press-time base restore on Escape. Not wired anywhere. | — |
| F10 | **Fallback ladder not wired** (docs admit it). | `docs/pomade-tool.md` |

F1 and F2 mean plan/17 P0's exit criterion ("tube visible in usdview") was met by the test tube
only, and every later phase's usdview exit was met at the ABI, not in the viewport.

## 1. Target: what "a solid tool" means here

One dockable **Pomade workspace**, one **viewport controller**, six modes on a shelf with number keys,
everything the artist looks at drawn by Hydra through the Pomade scene index, and three menu items.
A DCC/Pomade conventions: left-click acts in the current mode, `Alt`/`Meta` drags always belong to the
camera, `Escape` cancels the live gesture, `Ctrl+Z`/`Ctrl+Y` undo/redo the model, number keys switch
modes, letters switch sub-modes inside a mode, `[`/`]` change the brush radius.

```
┌ usdview ─────────────────────────────────────────────────────────────────────┐
│ usdGen ▸ Pomade ▸ Open workspace | Bind scalp (selection) | Save groom… | …   │
├──────────────────────────────────────┬───────────────────────────────────────┤
│                                      │ Pomade                              ▣ │
│   viewport                           │ [1 Graph][2 Tube][3 Fill][4 Hier]    │
│   scalp tint + graph                 │ [5 Sculpt][6 Output]                 │
│   tubes (level colours, x-ray)       │ ─ sub-modes of the active mode ───── │
│   center curves, rings, guides       │ [D Draw][P Place][C Connect] …       │
│   selection rim, hover               │ ─ parameters (generated) ─────────── │
│   gizmo, brush ring                  │ snap radius   [ 8 px ]               │
│                                      │ mirror-X      [x]                    │
│                                      │ ─ warnings ───────────────────────── │
│                                      │ ⚠ 3 uncovered faces  ⚠ T2∩T5         │
│                                      │ ─ status ─────────────────────────── │
│                                      │ L1 ▸ T4 ▸ T4.2   model v88 stage v88 │
│                                      │ map v12 (amber if behind)  swap 1.2ms│
└──────────────────────────────────────┴───────────────────────────────────────┘
```

## 2. Architecture changes (C++ first, then Python)

### 2.1 One model, registered (fixes F1)

Add `usdGenPomade/pomadeRegistry.{h,cpp}`:

```cpp
namespace usdGenPomade {
class PomadeRegistry {                       // process-global, mutex-guarded
public:
    static PomadeRegistry &Get();
    int  Register(PomadeModel *model);       // returns modelId (>0); called by Pomade_Create
    void Unregister(int modelId);           // Pomade_Destroy
    void SetActive(int modelId);            // Pomade_Activate; 0 = none
    PomadeModel *Active() const;
    // Scene indices attach on construction and detach on destruction.
    void AttachIndex(UsdGenPomadeSceneIndex *index);
    void DetachIndex(UsdGenPomadeSceneIndex *index);
    // Main thread only. Calls Refresh(dirty) on every attached index.
    void Publish(uint32_t dirtyMask);
};
}
```

* `UsdGenPomadeSceneIndex` stops owning a model. It holds nothing but the published snapshot and
  reads `PomadeRegistry::Get().Active()` inside `Refresh`. `GetModel()` is removed; the T1 test
  creates a model through `Pomade_Create` + `Pomade_Activate` like the plugin does.
* `USDGENPOMADE_TEST_TUBE` stays as a record-harness convenience: with no active model the index
  publishes the test tube; the moment a model is activated the test tube is removed
  (`PrimsRemoved`) and never comes back.
* New C ABI: `Pomade_Activate(ctx)`, `Pomade_Deactivate(ctx)`, `Pomade_Publish(ctx, uint32 dirtyMask)`
  (main thread; returns the number of indices refreshed), `Pomade_GetModelId(ctx)`.
* `Pomade_Publish` is the **only** way the viewport learns about a model change. The Python
  controller calls it after every mutating ABI call in a move, then `UpdateViewport()`.

### 2.2 Publish the whole model, per level (fixes F2)

Replace the single-tube publication with one prim family per hierarchy level, few fat prims
(plan/17 R2), all under `/__usdGenPomade/`:

| Path | Type | Content | Primvars |
|---|---|---|---|
| `tubes/L<n>` | mesh | every tube at level *n* tessellated (K5) into one mesh | `tubeId` (uniform int), `clumpColor` (uniform color3f, §2.4a), `hierarchyLevel` (constant), `selected` (uniform int 0/1/2: none/selected/hover), `xray` (constant 0/1), `displayOpacity` |
| `centers/L<n>` | basisCurves (linear) | center curves of level *n* | `tubeId` (uniform), `displayColor` (uniform = clump colour), `widths` (uniform: focused level thick), `selected` (uniform) |
| `centerCVs/L<n>` | points | every center CV of level *n* | `tubeId`, `cvIndex` (vertex), `displayColor` (vertex: clump / white selected / yellow hover), `widths` (vertex) |
| `rings/L<n>` | basisCurves (linear, closed via repeated first CV) | every section ring of level *n* | `tubeId`, `sectionIndex` (uniform), `displayColor`, `selected` |
| `ringCVs/L<n>` | points | section CVs of level *n* (Tube mode / selected tubes only) | `tubeId`, `sectionIndex`, `cvIndex`, `displayColor`, `widths` |
| `guides/L<n>` | basisCurves | K9/K10 output per level (preview density during a gesture) | as today (`hairT`, widths) + `tubeId` |
| `graph/nodes`, `graph/edges`, `graph/regions` | as today | | `nodeSelected` (vertex), `edgeSelected` (uniform) |
| `gizmo` | basisCurves | the active gizmo's handles in world space (3 axes / ring / plane), rebuilt each move | `handleId` (uniform), `active` (uniform) |
| `brushRing` | basisCurves | screen-facing circle at the hit point, radius = brush radius | — |
| `materials/{tube,hair,overlay}` | material | `pomadeTube.glslfx`, `UsdGenHairPreview`, a constant-colour unlit material for gizmo/rings/brush | — |

* Level visibility, Solo, Show ≤ n and x-ray become **visibility and the `xray` primvar** on the
  level prims; nothing is re-tessellated. The glslfx switches from the `selected`/`xray` material
  uniforms to reading the primvars (`HD_HAS_selected` guard), so one material serves all states.
* Dirty discipline: `Refresh(dirtyMask)` emits leaf-exact dirties per level prim: selection change
  → only `primvars/selected/primvarValue`; a move → points/normals/extent of the touched levels;
  subdivide/merge → topology of the touched levels. A `PomadeDirty` bitmask replaces `TakeDirty()`
  as the contract between model and index (`TUBE_POINTS_L<n>`, `TUBE_TOPO_L<n>`, `SELECTION`,
  `GUIDES`, `GRAPH_*`, `GIZMO`, `BRUSH`), with a per-tube dirty set behind it so a single-tube
  move re-stages only that tube's slice of the level mesh (the staged mesh keeps per-tube ranges).
* Transport stays host-staged pinned (plan/17 §4.3 phase A); the per-level staging buffers grow
  with a free-list and never shrink during a gesture.

### 2.3 Selection lives in the model

plan/17 §2.1 lists `selection` in `PomadeModel`; it is not there. Add `PomadeSelection` (tubes,
center CVs `(tubeId, cv)`, section CVs `(tubeId, ring, cv)`, rings, graph nodes, edges, regions,
guides, hover item) and the ABI:

```
Pomade_SelectClear(ctx, kindMask)
Pomade_SelectSet / Pomade_SelectAdd / Pomade_SelectToggle(ctx, kind, const int *ids, const int *subIds, const int *subSubIds, int n)
Pomade_SelectRect(ctx, viewProj[16], w, h, x0, y0, x1, y1, kindMask, mode)     // marquee (plan/17 §4.6)
Pomade_SelectPolygon(ctx, viewProj[16], w, h, const float *xy, int n, kindMask, mode)   // lasso
Pomade_SetHover(ctx, kind, id, subId)                                           // -1 clears
Pomade_ReadSelection(ctx, kind, int *outIds, int *outSubIds, int cap, int *outCount)
Pomade_GetSelectionBounds(ctx, float outMin[3], float outMax[3])              // for gizmo placement / frame
```

`Pomade_Pick` (K11) already returns kind/index/subIndex/depth; the rect/polygon variants reuse its
projection and add the polygon test on the device (tube verts, guide CVs) and on the CPU for the
small kinds, exactly as the point pick is split today. Selection is model state, so it survives a
commit, is cleared by topology changes that invalidate ids (subdivide of a selected parent moves the
selection to its children), and is **not** in the undo stack.

### 2.4 Gizmo and brush geometry

`Pomade_SetGizmo(ctx, kind, const float origin[3], const float frame[9], float sizeWorld, int activeHandle)`
with kinds `none | translate | ringTRS | nodeTranslate` and `Pomade_SetBrushRing(ctx, const float
center[3], const float normal[3], float radiusWorld)`; both just set model-side overlay records and
mark `GIZMO`/`BRUSH` dirty. The scene index turns them into the `gizmo` and `brushRing` prims. Handle
hit-testing is screen-space in Python (`pomadeMath.gizmoHit(viewProj, w, h, x, y, origin, frame,
sizeWorld)`), unit-tested Qt-free.

### 2.4a The viewport look

The scene index draws the authoring model in its own terms. There is no external
still, film frame, or local PDF to match. What the index draws:

| Element | Reference look | Implementation |
|---|---|---|
| Backdrop and character | flat mid-grey background, matte grey unlit-looking head/body | usdview default; the tool never recolours the scalp mesh itself, only the tint overlay |
| Tubes | **opaque, smoothly shaded volumes, one saturated colour per clump** (blue / yellow / magenta / green / purple / orange…), no wireframe, no level tint | `clumpColor` (uniform color3f) per tube, from a fixed 16-entry saturated palette indexed by the **L1 region id**; children of one L1 tube take the parent's hue with ±lightness steps per `childIndex` so a lock stays one hue family; `pomadeTube.glslfx` drops `levelColor1..3` + `regionShift` and reads `clumpColor`; shading becomes a simple headlight Lambert with a soft specular (the 2014/2018 tubes are clearly lit, not "facing shade"); `xray` renders the level at 25 % opacity with the same colour |
| Scalp region / clump map | the head painted in **the same colours as the tubes rooted there** (2014 (d), 2018 Fig. 2 right); uncovered faces visibly flagged | `graph/regions` tint already exists; its `displayColor` must use the identical palette lookup (`PomadeRegionColor` → same table as `clumpColor`), at the L1 level in Graph mode and at the focused level otherwise (channel *k* of the bake); uncovered faces dark red, doubly claimed magenta hatching as today |
| Center curves | **thick lines in the clump colour**; the currently editable level's curves thicker than descendants (2018 Fig. 1 middle: "thick curves are the control curves") | `centers/L<n>` basisCurves with per-curve `widths`: focused level 3 px-equivalent, other visible levels 1 px, hidden levels off; colour = `clumpColor`; selected curves white-rimmed (brighter, +1 px) |
| Control vertices | **round dots on the control curves** (2018 Fig. 4), larger on the focused level | `centerCVs/L<n>` points prim, `widths` 8 px focused / 5 px else, colour = clump colour, selected CVs white, hovered CVs yellow |
| Cross-section rings | thin closed rings in the clump colour with CV dots (the "orthogonal planar cross-sections" of the 2014 contract; never shown in the stills as solid) | `rings/L<n>` basisCurves 1 px + `ringCVs/L<n>` points 5 px, shown only in Tube mode (Ring/Section sub-modes) and on selected tubes elsewhere |
| Guides | thin lines, coloured by their clump (2014 (e), 2018 Fig. 2 middle) | `guides/L<n>` gets `displayColor` uniform per curve = clump colour; the hair-preview material keeps its width/shading; when "show amplified hair" is on, the amplified tiles draw in their own material and the guides hide |
| Scalp graph | nodes as dots, edges as lines on the head (2014 (d)) | as today; node dots white, shared edges solid white, border edges dashed grey, selected/hovered yellow |
| Selection | selected tube brighter with a rim; hover a lighter tint | `selected` primvar → glslfx rim (kept) + 15 % lightness lift; hover = 8 % lift, no rim |
| Gizmo / brush | host-application translate gizmo (red/green/blue axes, yellow when active), brush ring as a thin circle | `gizmo` and `brushRing` prims with the unlit overlay material |

The palette (sRGB, chosen to match the stills' saturation): `#2F6BFF #FFD400 #E0248F #4CD62B #8A3FFF #FF7A1A #00C8D6 #FF3B3B #A6E22E #F062F0 #1FA3FF #FFB000 #17C77A #C43CFF #FF5E9A #7BD3FF`,
indexed `regionId mod 16`; a T0 test checks that neighbouring regions in `pomade-graph-scalp.usda`
never share an index (the graph module may offset an id's palette slot when its neighbours collide,
recorded in `regionColors` on commit so the stage and the viewport agree).

The T3 image check for V0 is a screenshot of `examples/pomade-braid-hierarchy.usda` at L2 focus that
a reviewer compares with EG 2014 Fig. 1 (c)+(d) side by side: opaque coloured tubes, painted scalp
regions in the same colours, thick control curves with dots.

### 2.5 Undo/redo (fixes F8)

* `Pomade_Redo(ctx)`; the stack keeps a redo list that a new gesture clears.
* Graph edits enter the same stack (snapshot the graph + region ids; they are hundreds of nodes).
* Undo/redo of a graph edit re-rasterises and enqueues a bake like any graph edit.
* `Pomade_GetUndoLabel(ctx, int depth, char *out, int cap)` for the workspace's Edit strip.

### 2.6 Everything above is tested at T0/T1 before Python touches it

* `testUsdGenPomadeIndex`: registry attach/detach; test tube removed on activate; per-level prims
  appear after a subdivide; leaf-exact dirties for move / selection / visibility; per-tube slice
  restage after a single-tube move.
* `testUsdGenPomadeApi`: selection ABI round trips, rect/polygon select against the reference scene
  (agreement with per-item `Pomade_Pick`), gizmo/brush records, redo round trips, graph undo.

## 3. The usdview plugin, rebuilt (Python)

### 3.1 Module map (replaces the flat menu; keeps the Qt-free rule)

| Module | Qt | Role |
|---|---|---|
| `__init__.py` | no | Container: 3 menu commands (Open workspace, Bind scalp (selection), Save groom…) + Export/Import; owns `PomadeToolState`, creates `PomadeSession` lazily. |
| `pomadeSession.py` | no | Model/committer/bake lifetime (from `GraphController.activate/deactivate`), live sublayer, `publish()`, the idle pump (`pump()`), reattach on stage reload, `undo()/redo()`. Pure ctypes, testable headless. |
| `pomadeCamera.py` | no | Resolve the StageView camera once per gesture into `(viewProj float[16], w, h)`, `rayThrough(x, y)`, `pixelsToWorld(x, y, depth)`. Given a frustum and viewport size, unit-testable. |
| `pomadeViewport.py` | **yes** | `ViewportController`: one event filter on the StageView + one application-level key filter. Dispatches press/move/release/escape/hover to the active `ToolLoop`. Owns the idle `QTimer`. Passes `Alt`/`Meta` presses through. |
| `pomadeLoops.py` | no | `ToolLoop` base + `GraphLoop`, `TubeLoop`, `FillLoop`, `HierarchyLoop`, `SculptLoop`. Each loop is `press(pick) / move(dx, dy, pick) / release() / cancel() / hover(pick)` over the session, with no Qt import; the controller feeds them camera-resolved picks. |
| `pomadeGizmo.py` | no | Gizmo state machine: which handle, constraint plane/axis, drag→delta in world units; `pomadeMath` does the projection. |
| `pomadeWorkspace.py` | **yes** | The dock: shelf, sub-shelf, generated parameter panel, warnings, status strip. |
| `pomadePanels.py` | no | Parameter descriptors per mode (`name, label, type, min, max, step, get, set`) — the plan/08 §5.2 rule: panels are generated, never hand-written. |
| `pomadeHud.py` | no | Pure formatting for the status strip and warnings (from today's `hudStatus`, `hierarchyStatus`, `sculptStatus`…). |
| `pomadeMath.py`, `pomadeGraph.py`, `pomadeTube.py`, `pomadeFill.py`, `pomadeHierarchy.py`, `pomadeSculpt.py`, `pomadeBridge.py`, `pomadeLib.py` | no | Kept; loops call them. Stale `REQUIRED_C_API`/`missingEntries` scaffolding removed once the bindings are unconditional. |

`pomadeGraphUI.py` is deleted; its gesture code becomes `GraphLoop`.

### 3.2 The gesture contract (plan/17 §1.3, now real)

```
press   : if Alt/Meta → return False (camera). camera = pomadeCamera.resolve(view) once.
          pick = session.pick(camera, x, y, loop.pickMask)      # K11; K1 raycast for the surface
          loop.press(pick, modifiers) → may open an undo bracket (Pomade_BeginGesture)
          session.publish(); view.updateGL()
move    : throttle < 2 px; loop.move(...) mutates the model from the press-time base
          session.publish(); view.updateGL()          # no stage traffic, no swap
release : loop.release() → Pomade_EndGesture (seals undo, bumps version)
          session.enqueueCommit(); session.enqueueBakeIfGraphChanged(); scheduleIdle()
escape  : loop.cancel() → Pomade_CancelGesture (restore press-time base); publish
hover   : throttled ≥ 3 px, only when no gesture; Pomade_SetHover; publish(SELECTION)
```

`Pomade_BeginGesture/EndGesture/CancelGesture(ctx)` are new; today each ABI mutation calls
`_PushUndoLocked()` with a full `HierarchyRollback` snapshot (`pomadeModel.cpp`, 18 call sites) and
the stack is capped at 50 entries / 256 MB, so a 200-sample drag pushes 200 snapshots and evicts
every earlier step of the artist's history. Begin snapshots once; End seals;
Cancel restores. All mutations between them are "from the press-time base" by construction.

### 3.3 The idle pump (fixes F7)

`ViewportController` owns a `QTimer` (50 ms, started on release or when the committer reports a
pending version, stopped when nothing is pending and no bake is in flight). Each tick, only when no
gesture is active: `Pomade_CommitterSwap(live, gestureActive=0)`; drain `Pomade_BakeTakeCompleted` →
`Pomade_BakeSwap`; refresh the status strip. The stage swap stays under TN-4 because the committer
already does partial transfer; the pump never runs during a gesture because the controller knows.

### 3.4 Hotkeys (fixes F4) — an application-level filter, active only while the workspace is open

| Key | Action |
|---|---|
| `1`–`6` | Graph, Tube, Fill, Hierarchy, Sculpt, Output |
| letters | sub-mode / brush of the active mode, as documented today (`D P C W U X L`, `C R E`, `P V`, `N D M G L`, `G S C L T`) — only when the pointer is over the viewport and no text field has focus |
| `Escape` | cancel gesture, else clear selection, else nothing |
| `Ctrl+Z` / `Ctrl+Y` | `Pomade_Undo` / `Pomade_Redo` (model), then publish + enqueue commit |
| `Delete` | delete the selection in the current mode (node/edge, CV, ring) |
| `[` / `]` | brush radius −/+ (Sculpt), snap radius −/+ (Graph) |
| `Shift+D` / `Shift+M` | subdivide / merge children |
| `Ctrl+Down` / `Ctrl+Up` or double-click / `Backspace` | enter / exit level |
| `Shift+W` / `Shift+U` | weld / unweld the 2-node / 1-node selection |
| `Ctrl+Shift+S` | Save groom |

`F` is left to usdview (Frame Selected). usdview's `AppEventFilter` swallows `Escape` and refocuses
on every mouse move (plan/08 §3.5), which is why the filter is installed on `QApplication`, ahead of
usdview's, and only claims keys when the workspace is open and the viewport is under the pointer.

### 3.5 The workspace dock

`PomadeWorkspace(QDockWidget)`, added to `usdviewApi.qMainWindow` on the right (same pattern as
`ExpressionEditorDock`), remembered in usdview settings. Vertical layout:

1. **Mode shelf** — six checkable `QToolButton`s in a `QButtonGroup`, hotkey in the tooltip and as
   a small label. Switching modes clears the sub-mode to the mode's default and drops any gesture.
2. **Sub-mode shelf** — the active mode's sub-modes/brushes, same widget type; hidden for Output.
3. **Parameters** — a `QFormLayout` generated from `pomadePanels.descriptors(mode)`: spin boxes,
   sliders, checkboxes and a ramp widget for the length profile; each writes through `state` and,
   where the value lives in the model, through the ABI (`Pomade_SetFillParams`,
   `Pomade_SetSnapRadius`, `Pomade_SetPreviewFraction`, subdivide count/seed/split mode, locks…) and
   then `publish()`. Parameters apply to the selection when the mode is selection-based (fill
   params per selected tube).
4. **Actions** — the mode's one-shot buttons (Bind scalp, Weld all, Rebake, Subdivide, Merge,
   Merge selected, Re-subdivide, Group, Make persistent, Match surface, Relax, Snap root, Refill,
   Freeze roots, Export, Import, Save). This is where today's 22 menu actions go.
5. **Warnings** — coverage gaps, root intersections, kink spikes, device fallback, committer
   detached; clicking a warning selects the offending tubes/faces.
6. **Status strip** — breadcrumb (clickable), level, model/stage/map versions (amber skew),
   last swap ms, GPU memory, "show amplified hair" toggle.

Output mode swaps the parameter block for the map panel (texel resolution override, per-level
channel list, Wire Clump to level *n*).

### 3.6 Per-mode loops (what each mouse gesture does)

* **GraphLoop** — today's `GraphController` behaviour, with K1 raycast per sample instead of
  `view.pick`, hover highlight of nodes/edges, marquee node selection, `Delete`. Reposition (`M`)
  transports a whole region rigidly when the footprint moves as one unit; a CV or edge edit that
  changes the region outline instead refits the existing tube base to the new footprint, retains
  upper sculpting, and refreshes generated subdivided attachments. Imported tubes with explicit
  authored shapes are preserved.
* **TubeLoop** — click selects tube / center CV / ring / section CV per sub-mode (K11 with the
  sub-mode's kind mask); a translate gizmo appears on the selection (screen-plane by default,
  axis/normal constraint by handle); Ring sub-mode's gizmo has scale ring + twist handle; drag of a
  section CV moves it in the ring plane; double-click a tube enters its level; soft-selection
  radius from the panel is drawn as a faded span on the center curve.
* **FillLoop** — click selects tubes; the panel edits fill params for the selection; drag on a
  guide adjusts the length profile at that `t` (optional, last).
* **HierarchyLoop** — selection + double-click navigation; Subdivide/Merge/Group from the panel or
  hotkeys; edge split mode: draw one stroke across the root region, then Subdivide.
* **SculptLoop** — brush ring follows the cursor; press picks the nearest center CV footprint
  (`Pomade_Pick` with radius = brush radius), stroke calls `pomadeSculpt.stroke` per move with
  `Pomade_BeginGesture` around it; `[`/`]` radius; mirror-X and length-preserving from the panel.
* **Output** — no viewport gesture; the panel only.

### 3.7 Fallback ladder (fixes F10)

`ViewportController` times each move end-to-end. Three consecutive moves over 8 ms step the ladder
(preview fraction 0.25 → 0.10 → 0; then `displaySegments` halved outside the edited subtree; then
non-edited levels as centers only; then hover off); release restores everything and republishes at
full fidelity. The ladder position is shown in the status strip.

## 4. Tests

* **T0** (`testenv/testUsdGenPomadeToolsLoops.py`, headless): every loop driven with a fake
  session recording ABI calls; gizmo hit-testing; camera maths; panel descriptors round-trip.
* **T1** (§2.6).
* **T3** (`testUsdviewPomade*.py` through `testusdview`, driven with `QtTest.QTest.mousePress/
  mouseMove/mouseRelease` on the StageView, **not** ctypes): one script per mode that performs a
  real gesture and asserts on the *scene index* (via `usdviewApi.stageView`'s render index prim
  list at `/__usdGenPomade/`) that the published geometry changed, on the model that the version
  advanced, and on the stage that the committed guides landed after the pump. Existing ABI-level
  scripts stay as regression tests but are renamed `testPomadeAbi*.py` so their scope is honest.
* **Image**: `record_usd.ps1` over `examples/pomade-braid-hierarchy.usda` with `USDGENPOMADE_ENABLE=1`
  and the tool activated in a script proves the per-level publication renders; golden in
  `tests/golden`.
* **Gates**: TN-1 re-measured through the real controller (move ≤ 8 ms including publish +
  `updateGL` request); TN-3 unchanged; TN-4 measured from the pump; TN-7 measured with the
  reference bake started from the Output panel's texel resolution override (V7, 2026-09-19:
  `testUsdviewPomadeBakeIsolation`, plan/17 §7).

## 5. Phases and who executes

Fable plans (this document). Execution: Opus for the C++ phases and the controller, Sonnet for
the widgets, panels, docs and test scripts. Each phase is one branch/PR-sized unit with its tests
green before the next starts. No phase may add a menu item.

| Phase | Deliverable | Model | Exit |
|---|---|---|---|
| **V0 Registry + publish** | §2.1, §2.2 (levels, primvars, dirty bitmask), glslfx reads primvars, `Pomade_Activate/Publish`, test-tube retirement | Opus | `testUsdGenPomadeIndex` new cases green; `launch_usdview.ps1 examples/pomade-braid-hierarchy.usda` + a 5-line script (`Pomade_Create`, hydrate, `Pomade_Activate`, `Pomade_Publish`) shows every level in the viewport |
| **V0b Stage contract** | G1–G5 (§7): committer serialises the whole hierarchy nested with deltas, `childIndex`, `subdivide:*`, `HierarchyAPI` for persistent groups; per-tube center/section/fill/relax/match/snap ops take a `tubeId`; `Pomade_Hydrate(ctx, stageCacheId/layer id, groomPath)` hydrates every level and routes foreign guides to `ImportLockedTube`; Ptex channels per level; worker try/catch; bake D2H on `bakeStream` pinned + event; stale-file sweep; relative `map:file` on save | Opus | `testUsdGenPomadeCommit`: subdivide → commit → hydrate round-trips L1/L2/L3 bit-exactly; `testUsdGenPomadeRegionBake`: `Clump` at level 2 reads level-2 ids; `examples/pomade-braid-hierarchy.usda` regenerated by the committer, not a script |
| **V1 Selection, gizmo, gesture ABI** | §2.3, §2.4, §2.5, `Begin/End/CancelGesture` | Opus | `testUsdGenPomadeApi` new cases green; selection rim and x-ray visibly driven |
| **V2 Controller + session** | `pomadeSession`, `pomadeCamera`, `pomadeViewport`, `pomadeLoops` with `GraphLoop` only, hotkeys, idle pump, Escape, undo/redo; delete `pomadeGraphUI`; menu trimmed to the three items + Export/Import | Opus | T3 `testUsdviewPomadeGraph.py` rewritten to real mouse events; graph strokes visible while drawing |
| **V3 Workspace dock** | §3.5 with Graph and Output panels; warnings; status strip; stale text (F5) gone | Sonnet | T0 panel test; screenshot in `renders/pomade-workspace.png` |
| **V4 Tube + Fill loops** | gizmos, ring gizmo, soft selection display, fill panel per selection, preview/full refill on release | Opus (loops) + Sonnet (panels) | T3 `testUsdviewPomadeTube.py` real gestures; TN-1 re-measured |
| **V5 Hierarchy + Sculpt loops** | navigation, subdivide/merge/group from the dock and hotkeys, brush ring, strokes, ladder | Opus (loops) + Sonnet (panels) | T3 `testUsdviewPomadeLevels.py` / `testUsdviewPomadeSculpt.py` real gestures |
| **V6 Close plan/17 gaps** | §7 rows marked MISSING/PARTIAL, in the order listed there | Opus | the listed tests |
| **V7 Docs + soak** | `docs/pomade-tool.md` rewritten around the workspace (no menu paths); TN-5 soak driven through the controller instead of the ABI | Sonnet | soak green; docs reviewed. **Exit not met 2026-09-19: the controller soak is red (plan/17 §7 dated note); everything else in V7 is done.** |
| **V8 Look polish** | §2.4a conformance from the evidence of `renders/pomade-workspace.png`: scalp-tint keying + lift, pixel-sized overlay widths (`Pomade_SetDisplayScale`), guide-material ramp, tube key/fill rig | Opus | both screenshots recaptured; `tests/golden/pomade-braid-hierarchy.png` regenerated; 31/31 green |
| **V9 Display policy + perf** | **V9-display:** one display-policy table for all six modes (§2.4a rows), the x-ray translucency mechanism, clump hue steps, committed-guide hiding, the hydrate re-bind fix; close the green-sliver defect. **V9-perf:** dock-refresh cost, the soak's per-frame reading as an opt-in, the committer-swap regression | Opus ×2 (disjoint files, separate build trees) | **Display: exit met 2026-09-19** (policy, overlay, hue, guides, hydrate; the sliver bisected to usdview's origin axes, captures neutralise `DrawAxis`, four screenshots recaptured). **Perf: partial** — dock memoisation and `USDGENPOMADE_SOAK_PACE` landed, gate unchanged; the swap regression and the host-side growth were not started (plan/17 §7 V9 note). |

## 6. Rules for the executing agents

* Read plan/17 §1.3, §3.1, §4.6 and this document before touching code. plan/08 §1.2 (Qt-free
  rule), §1.3 (`ToolState`), §3.5 (hotkey constraints), §5.2 (generated panels) apply.
* Never call `view.pick()` per move. Never touch the stage from a gesture. Never add a menu item.
* Every ABI addition gets a T0/T1 test and a `pomadeLib.py` binding in the same change.
* Windows test notes in memory apply (E-6 gate noise under `--parallel`; DLL static dtor hangs).
* When a plan/17 statement and the code disagree, fix the code or update plan/17 with a dated
  note; do not leave the two silently apart (that is how F1 stayed hidden).

## 7. plan/17 coverage audit (C++/CUDA/schema)

The item-level matrix is `plan/17-audit-2026-09-18.md` (373 lines, one row per plan/17 statement
with file:line evidence). The schema (§2.2), the output contract (Guides + CurveAPI primvars,
RegionMap, RegionExpr, GuideInterpolate fill-in, Clump offer, `pomadeRegion` primvar, Save groom to
`.usdc`), the commit worker/swap/partial-transfer core, the bake worker with versioned files, and
the K1–K5/K8–K13 lanes are DONE. What is not, ranked by how badly it breaks the artist's contract:

| # | Gap | Audit row | Closed in |
|---|---|---|---|
| G1 | **The hierarchy never reaches the stage.** `PomadeSnapshotFromModel` pushes one entry (`tubeId = 0`, `level = 1`), `childIndex` literal, delta arrays empty; the committer writes flat `Tubes/tube%d`, never nests, never applies `UsdGenTubeHierarchyAPI`; `persistent`/`transientParent` are never read. The braid example was script-authored. | Dead #1, §2.2 rows 32–33, §5.4 "Make persistent" | **V0b** |
| G2 | **Tube-0 limitation.** Every §5.2 center/section op except `MoveTubeCenterCV`/`SculptStroke`, all §5.3 fill params, `RelaxCenter`, `MatchSurface`, `SnapRootToScalp` act on the single primary tube; children cannot be section-edited or filled independently. | Dead #11 | **V0b** |
| G3 | **Hydrate is unreachable** (no C ABI), and hydrates only the first L1 tube; foreign-guide import is never routed from a failed hydrate. | §2.5 rows 54–58 | **V0b** |
| G4 | **Ptex channels ≥ 1 are hard-zero**, so "Wire Clump to level *n*" reads zeros. | Dead #2 | **V0b** |
| G5 | **Worker loops have no try/catch**; a throw terminates usdview. Bake D2H is a blocking unpinned `cudaMemcpy` on the default stream (not on `bakeStream` with an event); stale `.ptx` cleanup wake unreachable; saved `map:file` is absolute. | Dead #6, #9, #10; §3.1a rows 81, 87 | **V0b** |
| G6 | ~~Committer snapshot copies whole host mirrors and regenerates guides CPU-side on the worker~~ **CLOSED V6 2026-09-19**: a content hash per snapshot entry drives a `PomadeGuideCache` on the committer, so a one-tube version refills one tube (35.5 → 2.2 ms at reference scale; worker build 135.5 → 102.6 ms), bit-exact against an uncached refill. The snapshot host-vector copy and the missing CUDA event stay, measured and explained in plan/17 §3.1. Transport aliasing is **withdrawn, not deferred**: `VtArray` has no external-buffer form; the copy it was about is 3.7 ms for a whole 770 K-vertex restage and a slice per move (plan/17 §4.3). | §3.1 row 66, §4.3 rows 163–164 | **V6 (done)** |
| G7 | ~~`showAmplifiedHair` read nowhere; no amplified-tile visibility override; no cook cancellation token.~~ **CLOSED V6 2026-09-19**: `ResolveHairDisplay` drives both the tile visibility overlay and the guide preview from (flag AND no gesture); the token is `UsdGenImagingSession::CancelCooks`, scoped by groom root through the store and reached from `PomadeCommitter::CancelDescriptionCooks` / `Pomade_CommitterCancelCooks`, called on the outermost gesture press. T1 in `testUsdGenPomadeIndex`, `testUsdGenSessionStoreOwner` and `testUsdGenPomadeCommit`. | §3.2 rows 95–97 | **V1/V2** (bracket + pump), **V6 (done)** |
| G8 | Undo: full-snapshot deque of 50, no redo, no UI caller, undo does not enqueue a commit; Save groom not undoable. | §3.3 rows 104–108 | **V1/V2** |
| G9 | Picking: `Pomade_Pick` has no UI caller; 5 of 9 kinds; no marquee/lasso; no hover; occlusion is same-frame z tie-break. | §4.6 rows 193–197 | **V1/V2** |
| G10 | Index publishes one mesh; `tubeId`/`hierarchyLevel` are constant floats 0/1; shader `selected`/`xray`/`regionShift` never driven; no centers/rings/gizmo prims; level visibility/x-ray/solo unrepresentable. | §4.4 rows 171–175, §D2 rows 206–207 | **V0** |
| G11 | K6, K7, K14 are CPU-only; six more device lanes run only in tests (production calls the CPU twins unconditionally); `CheckK6/K7/K14Stub` assert nothing; `PomadeLaunchRootSampleMesh` has no caller. | Dead #3–#5, §7 TN-6 | **V6** (production lane switch + real parity tests) |
| G12 | **CLOSED V6 2026-09-19.** Fixed: the auto-tube root section is fitted to the region boundary, and the authored ring CV count is 8..32 (import path 3..32) — plan/17 §5.2. Documented as ours, with the reason and the failure mode each one has, in plan/17 §4.1: K2 chord projection, K3 single-plane point-in-polygon, G1 brute-force snap, mirror-X closest-point. Subdivide edge coefficients are still off the ABI (they round-trip through the stage; no UI asks for them). | §4.1 rows 124–126, §5.1 row 233, §5.2 row 239, §5.4 row 267 | **V6 (done)** |
| G13 | Sculpt: comb/twist/falloff/mirror maths live in Python or nowhere in C++; brushes never touch guides; soft selection along the hierarchy missing. **V6 2026-09-19**: the last two are DEFERRED with reasons in plan/17 §5.5 — guide brushes need a guide-CV delta store, which breaks the reproducible-`Guides` stage contract (§2.5/§3.1) and is a schema phase of its own; hierarchy soft selection collides with the K6/K7 propagation on the same move and needs the same "what is a brush stroke in a hierarchy" decision, so the two go together. | §5.5 rows 281–290, §5.2 row 251 | **V5**, deferred half dated in plan/17 §5.5 |
| G14 | ~~no region→tube map; no per-region tube stub~~ **CLOSED V6 2026-09-19**: one L1 tube per closed region, the map kept in sync by `SyncRegionTubes` at every rasterise (identity on the node loop, not the region id), refusing with a diagnostic when a re-derived or removed tube carries child deltas; region ownership rides `HierarchyRollback` and the stub no longer clears undo; committer, hydrate and bake channels serialise every L1 root. T1 two-region commit+hydrate in `testUsdGenPomadeCommit`, T3 two-region stroke in `testUsdviewPomadeGraph`. Weld-coincident is still off the ABI. | §5.1 rows 219–229 | **V6 (done)** |
| G15 | Gates: TN-1/TN-4 proven by fixture-scale proxies only; TN-5 soak not registered in ctest; TN-7 unmeasured; ladder has no trigger (nothing measures per-move time). No pomade goldens in `tests/golden`; no T3 for Sculpt or Output. Stale "stub (P0)" headers on 4 000-line files. | §7, §8, Dead #13–14 | **V5** (ladder), **V7** (soak/goldens/docs) |

Phase **V0b** is inserted after V0 because G1–G5 are stage-contract bugs that make every viewport
phase after it pointless: an artist who subdivides and saves loses the hierarchy. V0 and V0b touch
disjoint files (index/registry/shader vs committer/bake/model tube ops) and may run in parallel
**only** if neither edits `pomadeApi.h`/`pomadeApi.cpp`/`pomadeLib.py` in the same window; V0 owns
those files, V0b adds its ABI (`Pomade_Hydrate`, per-tube ops) in a second pass after V0 lands.

## 8. Delivered

All of V0–V7 landed by 2026-09-19. `docs/pomade-tool.md` documents the tool as built; this section is
the phase-by-phase record, with the measured numbers plan/17's dated notes carry.

| Phase | What landed |
|---|---|
| **V0** | `PomadeRegistry` (§2.1); per-level prims (`tubes/centers/centerCVs/rings/ringCVs/guides/L<n>`) with the `PomadeDirty` bitmask (§2.2); `pomadeTube.glslfx` reads `clumpColor`/`selected`/`xray` primvars instead of material uniforms (§2.4a); `Pomade_Activate`/`Pomade_Publish`; the static test tube retires the moment a model activates. |
| **V0b** | The stage contract closed G1–G5: the committer nests the whole hierarchy with deltas and `HierarchyAPI`; per-tube ops (center/section/fill/relax/match/snap) take a `tubeId`; `Pomade_Hydrate` rebuilds every level and routes foreign guides to `ImportLockedTube`; Ptex channels ≥ 1 read live per level; both worker loops catch exceptions instead of terminating usdview. |
| **V1** | Selection lives in the model (`PomadeSelection`, rect/polygon pick reusing K11's projection), the gizmo/brush-ring ABI, and `Pomade_BeginGesture`/`EndGesture`/`CancelGesture` replace the old per-mutation snapshot (was 18 call sites pushing a full `HierarchyRollback` each). |
| **V2** | `pomadeSession`, `pomadeCamera`, `pomadeViewport`, `pomadeLoops` (GraphLoop first), the application-level hotkey filter, the 50 ms idle pump, Escape-cancels-gesture, undo/redo wired to `Ctrl+Z`/`Ctrl+Y`; `pomadeGraphUI` deleted; the menu trimmed to the five commands `docs/pomade-tool.md` now documents. |
| **V3** | The workspace dock (mode shelf, sub-mode shelf, generated parameter form, warnings, status strip); the stale "Needs the P4 C ABI" text (finding F5) is gone. |
| **V4** | TubeLoop and FillLoop with their gizmos; `Pomade_GetTubeSectionFrame` added for the ring/section chart conversion. TN-1 re-measured through the real controller: 50 real `QtTest` mouse moves dragging a center-CV gizmo in Tube mode gave a 0.609–0.741 ms median move (three ctest runs), well under the 8 ms budget at fixture scale. |
| **V5** | HierarchyLoop and SculptLoop; the fallback ladder went from a described-but-untriggered list (audit finding G15) to `pomadeLadder.FallbackLadder`, driven by the controller's end-to-end move timing. Trigger: three *consecutive* moves over 8 ms step one rung (preview 25 %→10 %→0 %, segments halved, non-focused levels to centers only, hover off); one in-budget move resets the count; release restores every rung's base value. TN-1 through the installed event filter: 0.14 ms/move (Hierarchy select+Shift+D+level walk), 0.76 ms/move (Sculpt Grab stroke) — both finish at `ladderStep 0`. |
| **V6** | Closed G6 (guide refill cache: a one-tube version's worker-thread refill fell from 35.5 ms / 2 400 tubes refilled to 2.4 ms / 1 tube, bit-exact against the uncached refill), G7 (amplified-hair display + cook-cancellation token), G11 (K6/K7/K14 batched CUDA lanes, bit-identical to their CPU twins; production still takes the CPU twin for K6/K14 and the device lane for K7 — see plan/17 §7's per-kernel table), G12 (auto-tube root section fitted to the region boundary; ring CV count 8..32; K2/K3/G1/mirror-X kept as documented, deliberate approximations — plan/17 §4.1), G13 (sculpt brushes reach center curves in C++ via `Pomade_SculptStrokeShaped`; guide brushes and hierarchy soft selection deferred together, reason in plan/17 §5.5), G14 (one L1 tube per closed region, `SyncRegionTubes` keeps the map current, the stub no longer clears undo). TN-1 re-measured with the CUDA hierarchy lanes wired: 1.30 ms/move over a 36-tube L1 subtree (three runs: 1.297/1.304/1.303 ms), against 1.25 ms all-CPU on the same box. TN-4 stays a T1 proof at reference scale: a full swap is 104 ms, so the partial-transfer fallback engages and converges in 2 418 slots with a worst slot of 2.27 ms. |
| **V7** | This pass: `docs/pomade-tool.md` rewritten around the workspace as built (no menu paths beyond the five real ones); the plan/13 Pomade row and this Delivered section; stale module-header comments (`pomadeHierarchy.py`, `pomadeBridge.py`, `pomadeSculpt.py`) that still described a pre-V0b/P4 "C++ has not landed" state, corrected. TN-5 soak (`testPomadeSoak.py`, seed 7): 4.14M ops in 30 min, 0 hard failures, p999 < 4.3 ms per op, device memory flat at 1531.5 MB; the one 54.5 ms outlier is shown not tool-attributable (a deterministic replay of the same state times the same op at 0.190 ms, and an out-of-process scheduler probe saw a matching 55.1 ms stall in the same window). **2026-09-19, test-hardening pass:** TN-7 is measured and green (`testUsdviewPomadeBakeIsolation`, three consecutive ctest runs plus eight consecutive full-suite runs); the Output panel's texel resolution override now reaches the bake worker (`PomadeSession.setBakeTexelResolution` → `Pomade_BakeSetOptions`, T1 proof that the baked `.ptx` per-face resolution follows it in `testUsdGenPomadeRegionBake`), which is also what makes the TN-7 bake big enough to outlast the drag; Output stopped being a "not built yet" mode (`pomadeLoops.PANEL_ONLY_MODES` replaces `UNBUILT`, T0 in `testUsdGenPomadeToolsLoops.py`); the dock now follows a hotkey mode switch instead of showing the previous mode's shelf and rows (T3 in `testUsdviewPomadeGraph.py`); `renders/pomade-workspace.png` regenerated through `bin/launch_usdview.ps1 -TestScript`. Two product bugs the TN-5 soak exposed are fixed: the guide preview bound a material the WIRE repr cannot compile, so Storm recompiled and failed it on every draw, and the dock leaked a shelf's worth of `QToolButton`/`QPushButton`/`QButtonGroup` per mode switch. TN-5 itself is NOT green — see plan/17 §7's dated note. `testUsdviewPomadePublish`'s intermittent (about 1 full-suite run in 25) was its absolute luminance steps being measured under whatever lighting the viewer happened to have: the failing run drew the same geometry with the same coverage but a ~5x weaker lit term, so the +2.0 selection step came out at +1.68. It now pins the lighting it measures under and prints the settings that were in force; 8 consecutive full-suite runs green after the change. |

| **V8** | Look polish against §2.4a, on the evidence of `renders/pomade-workspace.png`. (1) The scalp tint was keyed on the face's *interp* id (the bake's union-find channel) while the tube took its own *region* id, and the tint mesh carried the scalp's points verbatim, so the two coincident opaque meshes z-fought — the braid golden gave the tint 36 % of the scalp's pixels and the workspace shot 0 %. The tint now keys on `faceRegionIds` (the same id the tube's desc carries) and is lifted off the scalp by 2e-3 of its bounding diagonal; T1 asserts every claimed face equals the clumpColor of the tube rooted in it. (2) Overlay widths were `tubeRadius * factor` and Storm takes widths in WORLD units, so a one-unit tube drew 37 px CV blobs. `Pomade_SetDisplayScale` (world units per screen pixel, measured by the controller at the scalp centre on press, on each idle tick and on `signalFrustumChanged`) drives the §2.4a pixel targets — 8/5 px CV dots, 3/1 px control curves, 1 px rings, 6 px graph nodes (which published no `widths` at all before). A model with no camera keeps the pre-V8 radius-relative widths. (3) The gizmo's fixed pixel length went 72 → 90 px; its "oversized centre disc" was the selected center-CV dot, fixed by (2). (4) The guide preview's material ramped the per-curve clump colour to `UsdGenHairPreview`'s brown `tipColor`; it now lifts to white along the strand so a guide reads against the tube it grew from. (5) `pomadeTube.glslfx` replaced `abs(dot(N, V))` with a fixed eye-space key/fill rig (the headlight peaked at N·V = 0.861 on a vertical tube, which `pow(·, 24)` turned into 2.8 % of the specular gain): the tube now runs 0.31 → 0.79 in diffuse across its width with the bright band left of centre. `tests/golden/pomade-braid-hierarchy.png` regenerated for (1), (2) and (5). New: `plugin/usdGenPomadeTools/testenv/capturePomadeBraidL2.py` → `renders/pomade-braid-hierarchy-l2.png`, the §2.4a L2-focus still. |
| **V9** | Two agents, disjoint files, separate build trees. **Display** (2026-09-19): the §2.4a look is now one table (`PomadePolicyLevelDisplay`/`PomadePolicyRingDisplay` in `pomadeModel.h`, pushed by `Pomade_SetDisplayPolicy`) — Graph and Output opaque with no centers and no rings; Tube·Ring/Section focused-opaque with all rings; Tube·Center, Hierarchy and Sculpt focused at 25 % with centers, others at 10 %; Fill every level at 25 %; guides only in Fill and Output (a full-density preview anywhere else hides the control curves the other modes exist to edit). Before V9 the tool set x-ray nowhere, so the centers, CV dots and guides — all geometrically inside their tube — had never once been visible. The on-top mechanism is translucency, not `displayInOverlay` (usdview's task controller builds no render task for that tag, so tagged prims vanish): an x-rayed level binds a second material with `opacity` below 1, HdSt derives the `translucent` tag from it, and `HdxOitRenderTask` composites it after the opaque pass without touching depth; `pomadeTube.glslfx` no longer hardcodes a materialTag. Clump hue steps went 0.12/(level−1) → 0.22 + 0.10 per further sibling pair, shrinking 0.8 per level (lighten = lerp to white, darken = scale, hue exact; T0 asserts hue, > 2 % luma between neighbours, containment in the ancestor band; both goldens regenerated, intentional). `<groom>/Guides` hides behind a Hydra visibility overlay while a model is active (`Pomade_SetGroomPath` beside `Pomade_CommitterCreate`). Hydrate stopped re-binding the scalp after `Pomade_Hydrate` (the re-bind cleared the graph and painted the braid scalp uniformly dark red). **The green sliver is closed as viewer chrome, not a Pomade defect** (2026-09-20): a 3 px pure-green vertical line in every x-rayed frame bisected past every Pomade prim family, past deactivation, and past hiding every stage imageable — then reproduced with no tool and no test tube at the identical pixel, and vanished when `stageView.DrawAxis` was patched out. It is usdview's unconditional RGB origin axes (pure-green +Y, depth-tested, scaled by camera distance): opaque tubes occlude it, x-rayed tubes do not, which is the whole behaviour. The Pomade index, publisher and clump palette can no longer be suspects (no (0,1,0) anywhere on those paths; proven in the close-out). Both capture scripts now neutralise `DrawAxis` for the shot (T3 pixel tests keep it — their measurements were taken with it on) and all four `renders/pomade-*.png` were recaptured clean. **Perf** (2026-09-19, partial): dock-refresh memoisation (`pomadeHud.warningsKey`, 250 ms warnings cadence, gesture deferral, text-compare skip) and the `USDGENPOMADE_SOAK_PACE=1` opt-in per-frame soak reading landed with the gate unchanged; the committer-swap regression and the host-side growth were not started (plan/17 §7 V9 note). 31/31 green throughout. |

**Deferrals carried forward**, both documented in `docs/pomade-tool.md`'s "Deferred, not built" section:

* **Guide brushes** — Sculpt reaches center curves, not guides; a guide-CV delta store would break
  the `<groom>/Guides` reproducible-stage contract (bit-equal hydrate regeneration). Reason and scope
  in plan/17 §5.5.
* **Hierarchy soft selection** — one tube's center-CV soft selection works everywhere; spreading a
  move across neighbouring tubes by hop distance is undecided because it collides with K6/K7
  propagation on the same move. Deferred with guide brushes for the same underlying decision
  (plan/17 §5.5).
