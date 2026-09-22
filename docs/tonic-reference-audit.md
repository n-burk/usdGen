# Tonic reference audit

This audit compares the visible reference material with the usdGen Tonic
authoring workflow. The sources are the [Disney Animation Tonic
page](https://disneyanimation.com/process/look-development/?drawer=/technology/tonic/)
and its [embedded video](https://www.youtube.com/watch?v=F86vm8-mtds). The
observations below refer to the video sequence around 12–35 seconds. The
comparison uses visible workflow and presentation cues only; it does not
infer proprietary algorithms or promise pixel-identical output.

## Observable source cues

The video shows the following useful cues:

* At 12s an unpainted scalp sits beside a compact dock, suggesting that
  the workspace can be opened before regions exist and that the controls stay
  close to the viewport.
* At 15s a clicked polygon/CV outline has orange feedback, making the
  region's editable points and active selection legible.
* At 18s several saturated, enclosed neighbouring patches are separated
  by readable white boundaries.
* At 21s tube shaping appears as a distinct authoring step after the region
  work.
* From 27s onward the hierarchy appears as coloured clumps, with descendants
  remaining visually related to their parent.

These observations support a region-first, tube-second, hierarchy-aware
workflow and a strong display distinction between authored boundaries and
the resulting clumps. They do not specify the source tool's data structures,
sampling kernels, cache policy, or renderer implementation.

## Resulting usdGen workflow

The workspace is a persistent right-docked panel. Its Geometry block keeps
the current mesh path visible and opens **Bind geometry…**, a picker of usable
stage Mesh prims. A replacement requires explicit confirmation because it
clears the bound groom's graph and maps. After binding, Graph is the active
mode. **Bind scalp (selection)** remains the menu shortcut when a usable mesh
is already selected.

Graph defaults to **Create region** (`R`). The artist clicks successive scalp
CVs, clicks the first CV or presses `Enter` to close after three or more CVs,
uses `Backspace` to remove the most recent draft CV, and uses `Escape` to
cancel the draft. Closing commits one region edit, rasterises it, and creates
its L1 auto-tube/root. Clicking an existing CV while creating another region
reuses that exact graph node, including a CV already shared by another region,
so adjacent regions can share an authored boundary. **Draw** (`D`) remains
available for freehand strokes;
the existing Place, Connect, Weld, Unweld, Delete, and Link tools remain
available for graph repair and editing. New region tubes default to **Match
region CVs** (Auto), with one longitudinal column per ordered region CV and a
base following the polygon's projected fitted support plane. A higher ring CV
count adds edge controls while preserving corners; construction uses neither
a polar nor an averaged fit, and existing sculpted tubes are not regenerated
when the setting changes. Regions above the 32-CV construction limit retain
their authored graph and report that diagnostic. **Reposition** (`M`) drags an
existing CV or whole edge along the scalp. A rigid whole-region move carries
the attached tube and subdivided descendants by one translation/rotation,
preserving sculpted centers, sections, and child offsets while the base stays
aligned to its support plane. When a CV or edge move changes the region
footprint, the existing tube base is refit, upper sculpting is retained, and
generated subdivided attachments are refreshed; imported tubes with explicit
authored shapes are preserved. A shared boundary updates both owning
subtrees; unrelated regions are unchanged. The live preview is one undo step,
`Escape` cancels it, and empty clicks do not create or auto-weld.

Graph and Output expose one synchronized **Ptex density** control, labelled
**Ptex texels per face side**. `auto` uses the per-face plan, keeps
region-boundary faces at least
64×64, and collapses only conservatively proven uniform interior faces to
1×1. Explicit choices are powers of two from 4 through 4096 and trigger an
immediate rebake.

Output also provides **Build/update description**, **Description density**,
and **Strand width** after the first build. Density `1` uses the tube's Fill
density; changing it regenerates strands without increasing the sparse source
curve count. The build commits sparse tube-shape guides to
`<groom>/OutputCurves`, bakes a PTex ownership map at `<groom>/OutputRegionMap`,
and creates `CurveSource` interpolation plus `Width` under `<groom>/Output`.
The source, map and description publish together; saving flushes the newest
commit and includes the map asset. Show amplified hair and generated-guide
visibility remain separate: hiding or clearing guides does not regenerate or
delete the committed output. After the first build, tube, region, and Sculpt
edits regenerate it automatically on commit while the model is live. A saved
sparse output can generate hair without the model; reopening the model and an
explicit build action activate or rebuild it when needed.

Graph mode displays the scalp patches and their authored boundaries while
hiding tube geometry, so the graph remains readable. Tube, Fill, Hierarchy,
and Sculpt use the mode display policy for tubes, control curves, rings,
guides, and x-ray levels. Output is a result view: authoring tube walls,
center controls, and generated guide helpers are hidden, while **Show
amplified hair** independently controls the committed generated result. There
is no guide fallback before the first build. Colours communicate region/clump ownership;
they are a deliberate presentation choice rather than a claim about the
reference implementation's palette. Selection drags show a visible,
mouse-transparent Qt `QRubberBand` marquee: Graph uses `Shift`-drag for CV
selection while plain Create-region clicks still author CVs. Tube gives
component CV hits fixed 8 px priority and prehighlights them; body drags use
the selected Box/Lasso marquee rather than stealing the gesture for a
whole-tube selection, with `Shift` adding to it. StageView rollover picking and its hover tooltip are suppressed
while the dock is active and restored when it closes or hides. Tube center-CV,
ring, and Section-CV selection or hover highlights the owning tube mesh, and
the reported tube id is retained when the hit is on a child tube. Ring edits
scale a selected ring uniformly; Section edits move only the individual CV
inside its ring plane. Geometry drags request a viewport redraw on each move,
while the commit and full-density guide refill remain release/idle work.
Centered section-core handles use the actual section-area centroid, while an
edit remains attached to the same selected center CV. Hold `F` and drag
horizontally with the left mouse button to resize the Sculpt brush live.
Hierarchy navigation is explicit: subdivision selects the created children and
focuses their level; entering a selected child level selects its children,
while exiting restores the parent selection. A rapid Tube double-click does
not enter a hierarchy level. Sculpt first targets a center CV, falls back to
the visible tube body when no CV is hit, and uses the scalp only for the idle
brush ring. The active plane and camera/depth context freeze at press; Grab
keeps its initial CV weights through background updates, its ring stays
camera-facing at the cursor, and other brushes apply a screen-space footprint.
**Brush strength** also scales Grab. Interrupted gestures roll back to their
press-time shape, while a normal mouse leave during a held drag leaves it
active.

Hierarchy depth is branch-local: entering root A exposes A's children while a
separate root B stays at its current frontier, so A and B may be visible at
different levels. Exiting A restores that branch's parent selection and
visibility without changing B. Whole-tube Move applies one atomic translation;
a parent transform preserves their sculpted centers, sections, child offsets,
and individual CV offset slots. K7 inherits only the original outer boundary
from L1 to drive the parent layout; internal sibling edges and added L2 detail
remain child edits, with no full-volume refit. A child edit stays local while
the parent layout is preserved. Holding a boundary is exact when the target is
coplanar with existing L1 sections; bent or non-coplanar inherited edges use
the existing planar sections' projection instead of claiming an arbitrary 3D
edge fit.

Tube editing also exposes a persistent **Show generated curves** checkbox and
an explicit **Clear generated curves** action. Clear removes generated guide
output from the model while preserving authored tubes; its committed
suppression is undoable and leaves automatic refill suppressed until
**Refill guides** is pressed. The separate visibility control does not
regenerate cleared data. Tube component selection is explicit
through Whole Tube (`F8`), Center CV (`F9`), Ring (`F10`), and Section CV
(`F11`), with Box/Lasso marquee shape and `Shift` additive selection. The
Transform tool field shows `Q` Select, `W` Move, `E` Rotate, and `R` Scale.
Visible component dots follow the active choice: F8 Whole Tube shows no
component dots, F9 shows center CV dots, and F10/F11 show section control dots;
the centerline guide may remain visible.
Hidden generated curves are excluded from Tube/CV picking while authored tubes
remain editable.

The screen-space gizmo geometry adapts the upstream material in
`../usdRig/usdRig` (RigExec's `gizmoScreen.py`); the requested
`../usdGen/usdGen` source tree is absent. The shared geometry is consumed by
the Qt overlay with the Hydra scene-index path as fallback.

## Backend gaps addressed

* Closed regions can occupy only part of one coarse mesh face. Root sampling
  tests the graph region itself, so separate subface regions keep separate
  root support and receive separate auto-tubes.
* The Ptex worker keys its cached classifier inputs, including mesh, loops,
  hierarchy, and resolution. A graph boundary that moves inside one coarse
  face therefore cannot reuse stale texels merely because the face label is
  unchanged.
* Automatic resolution keeps boundary-bearing faces at 64×64 and collapses a
  face to 1×1 only after boundary checks and corner classification agree that
  the face is uniform. This is intentionally conservative around small
  enclosed regions.
* The display overlay lifts the patch and graph layers enough to remain
  readable over the bound mesh, and the Graph display policy hides tube
  geometry while the graph is being edited.
* Region-rooted base tubes retain their fitted support-plane frame when center
  CV1 moves, and commits persist that frame alongside the sampler version.

## Validation coverage

The Qt CV-region acceptance path exercises binding through the dock picker,
creating distinct regions on one coarse mesh face, changing the Ptex control,
and undoing the region edit. Compatibility checks cover opening the legacy
braid through strict old-generator validation. New commits carry
`usdGen:tonic:rootSampler = region-v3` and each tube's root-frame attributes.
The v3 Fill sampler follows complete section polygons, including the final
ring; older versions retain their original generator for validation before
migrating. Hydrate rejects unknown sampler versions and edited guides. Graph rendering
snapshots record the intentional white-boundary and CV-overlay differences.

## Known limits

The reference material is visual and process-oriented. The implementation
uses its own graph classification and deterministic root/fill rules; it does
not reproduce an unpublished Disney algorithm. “Exact boundary” means the
authored graph boundary at the current CV/polyline resolution, not a claim of
pixel equivalence to the reference frames.

`auto` is an area-based heuristic with a conservative boundary rule. Explicit
resolution improves sampling detail but increases bake cost and memory. Mesh
binding accepts a USD Mesh with usable points, face counts, and face indices;
non-mesh or incomplete topology is rejected. Degenerate or overlapping graph
regions remain diagnosable through coverage/intersection warnings and do not
gain special proprietary semantics. Root density is estimated from polygon
projected area; linked overlapping polygons currently sum those projected
areas, so density can overcount their overlap. Curved-surface classification
also remains chart projection rather than a proprietary intrinsic-surface
algorithm.

Pinned-root hierarchy averaging/merge and hierarchical sculpt use
descriptor-aware CPU twins because the older CUDA packed layout lacks
root-frame metadata. Viewport tessellation and the other device paths retain
their GPU routes; this is an intentional correctness/performance tradeoff,
not a user-facing warning.
