# G — Instancing: cards, archives, and native instances

**Gap:** the request is an XGen-like *instancing* plugin (XGen primitive types include Splines,
Spheres, **Cards** and **Archives** — A7 §1.1), yet no prior report designs emitting an
`HdInstancer` from the hair system. This report determines, with running code, exactly how a
`usdGen` scene index can emit an instancer whose prototypes are stage-authored prims, how that
interacts with UsdImaging's own point-instancer and native-instancer propagation, and what a
groom under a natively-instanced scalp looks like.

Everything below is either a `file:line` citation into `/home/burkard/work/OpenUSD` (tag v26.08,
`ee47c679a`) or the output of a probe that was **built and run headless in this session**. Sources
and captured outputs live in
`/tmp/claude-1000/-home-burkard-work-usdRig/887eb74a-2f4d-45ff-88d7-6c9ab67fd9a7/scratchpad/probes/instancing/`
(`instProbe.cpp`, `instProbe2.cpp`, `instProbe3.cpp`, `instProbe4.cpp`, `makeStage.py`,
`out-probe1.txt` … `out-probe4.txt`, `README.md`). No GPU/display was used or needed: everything
here is data-source-level and goes through `Hd_UnitTestNullRenderDelegate`
(`/home/burkard/work/OpenUSD_26_08/include/pxr/imaging/hd/unitTestNullRenderDelegate.h:16-80`,
symbol present in `libusd_hd.so`).

---

## 0. Verdict up front

| Question | Answer (evidence) |
|---|---|
| Can a **downstream** (post-propagation) SI synthesize a valid instancer over stage-authored prototypes? | **Yes, fully.** Probe 1 §B/§C: synthesized instancer round-trips through Hydra-1.0 emulation — `sd->GetInstancerId(proto)` = our instancer, `GetInstanceIndices` = `[0,1,3]` (mask applied), all four instance-rate primvars readable. |
| Do we have to author `instancedBy` and hide the standalone prototype ourselves downstream? | **`instancedBy`: yes. Hiding: no** — authoring `instancedBy` alone makes the prim draw *only* through the instancer (`hdSt/primUtils.cpp:1164-1220`, `mesh.cpp:2923`). It is drawn once per instance index, never at its own location. |
| Is there a cleaner place? | **Yes — insert the generator *upstream* of `UsdImagingPiPrototypePropagatingSceneIndex`** (`sceneIndices.cpp:222-225`). Probe 2 proves propagation then does prototype re-rooting, `instancedBy` authoring, prototype hiding and nested-instancer composition **for free**. |
| Is hair generated under a native prototype automatically instanced? | **Yes**, if generated upstream of `UsdImagingNiPrototypePropagatingSceneIndex`. Probe 2: a `basisCurves` added at `/__Prototype_1/Hair` appears at `…/UsdNiPrototype/Hair` with `instancedBy.paths=[…/UsdNiInstancer]` authored by UsdImaging. Downstream it is **not** automatic — probe 3 shows we can compose it by hand, and the nested chain survives emulation. |
| Adapter subprim (`GetImagingSubprims`)? | Works as an instancer (probe 2 case f) but **cannot own its prototypes in namespace** — `SdfPath("/World/Groom.groomInstancer").AppendChild("Proto")` is rejected (`sdf/path.cpp:841`, verified in Python). So it loses the free prototype-hiding and forces stage-side prototypes. Use a *child prim* instancer, not a subprim, for cards/archives. |

**Recommended design:** `usdGen` emits card/archive instancers from a scene index inserted at the
`overridesSceneIndexCallback` slot (`sceneIndices.cpp:222-225`), with the prototype emitted as a
**namespace child of the instancer prim** (the USD `PointInstancer` convention). Also register a
`UsdImagingSceneIndexPlugin` for the post-propagation work (curve deformation against
already-skinned/rigged scalps) — the two halves are separate scene indices, joined by the fact that
propagation copies prims and data sources verbatim.

---

## 1. The instancer contract in 26.08 (and a stale doc to ignore)

`extras/imaging/docs/hydra_prim_schemas.dox:233-245` describes `instancerTopology/instanceIndices`
as `HdVectorDataSource` of **`HdInstanceIndicesSchema`** containers (`instancer`,
`prototypeIndex`, `instanceIndices`). **That is wrong for 26.08.** The file declares itself "a
snapshot from November 2023" (`:4-5`). The real accessor is
`HdInstancerTopologySchema::GetInstanceIndices() -> HdIntArrayVectorSchema`
(`pxr/imaging/hd/instancerTopologySchema.h:125-126`), i.e. a plain vector of `VtIntArray`, one per
prototype. `HdInstanceIndicesSchema` exists but belongs to *selection*
(`hdx/selectionSceneIndexObserver.cpp:138,176,237`; `usdImaging/selectionSceneIndex.cpp:88,407-450`;
`hd/schemaTypeDefs.h:32`). Build against the header, not the dox.

### 1.1 What a valid `instancer` prim must contain

| Locator | Type | Required | Evidence |
|---|---|---|---|
| prim type `"instancer"` | token | yes | `hd/tokens.h:117`; emulation dispatch `sceneIndexAdapterSceneDelegate.cpp:274-275` |
| `instancerTopology/prototypes` | `VtArray<SdfPath>` (prims **or subtree roots**) | yes | `instancerTopologySchema.h:64-70,122-123` |
| `instancerTopology/instanceIndices` | vector of `VtIntArray`, one per prototype | yes | `:125-126`; USD builds it with `HdRetainedSmallVectorDataSource` at `dataSourcePointInstancer.cpp:370-379` |
| `instancerTopology/mask` | `VtBoolArray`, empty == all true | no | `:70-72,128-129` |
| `instancerTopology/instanceLocations` | `VtArray<SdfPath>` — **implicit/native only, leave null for ours** | no | `:83-90` |
| `primvars/hydra:instanceTransforms` | `VtArray<GfMatrix4d>` or `GfMatrix4f`, interp `instance` | one of | `hd/tokens.h:124`; `hdSt/instancer.cpp:110-131` |
| `primvars/hydra:instanceTranslations` / `…Rotations` / `…Scales` | `Vec3f` / `Quath` / `Vec3f`, interp `instance` | one of | `hd/tokens.h:125-127`; `dataSourcePointInstancer.cpp:249-301` |
| any other `instance`-interpolated primvar | anything | no | **no filtering at all**: `hdSt/primUtils.cpp:211-221` (`// XXX: Can we do filtering?` then `return primvars;`) |
| `xform`, `visibility` | instancer transform / visibility | yes in practice | `dirtyBitsTranslator.cpp:445-447,442-444` |
| `primOrigin` | `HdPrimOriginSchema` | **needed for picking** — see §6 | `primOriginSchema.h:35-40,70-95` |

Composition order in Storm is `T * R * S` applied **after** `instanceTransforms`
(`hdSt/shaders/instancing.glslfx:38-87`, comment at `:47-49`). Instance-rate primvars must all have
the **same element count** or Storm warns and trims to the minimum
(`hdSt/instancer.cpp:145-167`); an empty array is silently skipped (`:141-143`).

### 1.2 What the prototype prim must contain

`instancedBy/paths = [<instancerPath>]` and `instancedBy/prototypeRoots = [<prototypeRoot>]`
(`instancedBySchema.h:35-38`, Builder at `:153-171`). **Exactly one path**: emulation issues
`TF_CODING_ERROR("Prim <%s> has multiple instancer ids, using first.")` when `paths.size() > 1`
(`sceneIndexAdapterSceneDelegate.cpp:2665-2695`, error at `:2684-2688`). Storm's `HdInstancer`
levels come from walking that chain (`hd/instancer.cpp:30-44`, `hdSt/primUtils.cpp:1161-1199`).

`ComputeInstanceIndicesForProto` matches by **prefix** (`instancerTopologySchema.cpp:55-59:
`path.HasPrefix(prototypes[i])`) and applies the mask (`:61-...`), so a **subtree prototype**
(exactly what an *archive* is) works: every descendant gprim resolves the same instance index
array. Probe 1 §B confirms: `mask=[1,1,0,1]` + `instanceIndices=[[0,1,2,3]]` ⇒
`ComputeInstanceIndicesForProto(proto) = [0,1,3]`, and a non-prototype path returns an empty array.

---

## 2. Probe 1 — what a downstream SI actually sees, and can it build an instancer

`instProbe.cpp` opens a stage with (a) a scalp mesh natively instanced twice
(`/World/HeadA`, `/World/HeadB`, `instanceable = true`), (b) a `UsdGeomPointInstancer` with a card
mesh prototype, (c) a standalone card mesh. It runs `UsdImagingCreateSceneIndices` and dumps the
terminal prim tree, then appends a filtering SI that synthesizes an instancer.

**Terminal tree, abridged** (`out-probe1.txt`):

| Path | primType | notable data sources |
|---|---|---|
| `/World/Cards/PI` | `instancer` | `instancerTopology.prototypes = [/World/Cards/PI/Prototypes/Card/ForInstancer9c60c467c223c3c0]`, `instanceIndices=[[0,1,2]]`, `mask=[]`; primvars `hydra:instanceTranslations|Scales|Rotations` (all `instance`) |
| `/World/Cards/PI/Prototypes/Card` | **`''`** (hidden) | still carries `mesh` ds; `__usdPrimInfo.piPropagatedPrototypes = {ForInstancer9c60…: /World/Cards/PI/Prototypes/Card/ForInstancer9c60…}` |
| `…/Card/ForInstancer9c60…` | `mesh` | `instancedBy.paths=[/World/Cards/PI]`, `prototypeRoots=[…/ForInstancer9c60…]` |
| `/World/HeadA`, `/World/HeadB` | `''` | `instance = {instancer: /UsdNiPropagatedPrototypes/NoPrimvars___usdUpAxisd827…/__Prototype_1/UsdNiInstancer, prototypeIndex: 0, instanceIndex: 0|1}`; `__usdPrimInfo.niPrototypePath = /__Prototype_1` |
| `…/__Prototype_1/UsdNiInstancer` | `instancer` | `prototypes=[…/UsdNiPrototype]`, `instanceIndices=[[0,1]]`, **`instanceLocations=[/World/HeadA, /World/HeadB]`**, `mask=[1,1]`, primvar `hydra:instanceTransforms` (`instance`) |
| `…/UsdNiInstancer/UsdNiPrototype` | `''` | `instancedBy` → UsdNiInstancer; `__usdPrimInfo.isNiPrototype = 1`; `primOrigin.scenePath = OriginPath(.)` |
| `…/UsdNiPrototype/ScalpMesh` | `mesh` | `instancedBy` → UsdNiInstancer; `primOrigin.scenePath = OriginPath(ScalpMesh)` (**relative**) |

Two facts a downstream SI must internalise:

1. **Native-instance prototype paths are hashed and unpredictable.** The aggregation bucket name
   `NoPrimvars___usdUpAxisd827b8678191286` derives from the instance data-source names
   (`sceneIndices.cpp:140-167`); the point-instancer prototype suffix `ForInstancer9c60c467c223c3c0`
   is `TfStringPrintf("ForInstancer%zx", TfHash::Combine(instancerPathString, propagatedPrototypeString))`
   (`piPrototypePropagatingSceneIndex.cpp:406-433`). **Never construct these by hand** — discover
   them from `__usdPrimInfo.piPropagatedPrototypes` (a name→path container,
   `usdPrimInfoSchema.h:48`) and `__usdPrimInfo.isNiPrototype` / `niPrototypePath` (`:46-47`).
   Probe 3 discovers both at runtime this way.
2. **Prim origins are relative inside prototypes** (`OriginPath(ScalpMesh)`, `OriginPath(.)`), which
   is what makes pick round-trips work (§6).

**Synthesis result** (probe 1 §B/§C, `out-probe1.txt`). Our SI inserted
`/World/HairCards/CardInstancer` (type `instancer`) and overlaid `instancedBy` on the *existing*
stage mesh `/World/HairCards/CardProto`:

```
proto.instancedBy.paths = [/World/HairCards/CardInstancer]
proto still has 'mesh' ds: 1                       <- overlay preserved upstream data
ComputeInstanceIndicesForProto(...) = [0, 1, 3]    <- mask honoured
--- through Hd_UnitTestNullRenderDelegate emulation ---
GetInstancer(/World/HairCards/CardInstancer) = FOUND
sd->GetInstancerId       = '/World/HairCards/CardInstancer'
sd->GetInstanceIndices   = [0, 1, 3]
sd->GetInstancerPrototypes = [/World/HairCards/CardProto ]
instance-rate primvar descriptors: hydra:instanceTranslations hydra:instanceRotations
                                   hydra:instanceScales displayColor
```

`displayColor` — an *arbitrary* instance-rate primvar — survives, confirming
`hdSt/primUtils.cpp:219-220`. This is the mechanism for per-card colour from a ptex/SeExpr lookup
without a per-card rprim.

> Caveat found: `HdRprim::GetInstancerId()` is still empty pre-Sync (`rprim.h:157` is a cached member
> filled by `_UpdateInstancer` during `Sync`), so tests must query the *scene delegate*, not the
> rprim. Recorded so the plan's unit tests don't chase a phantom bug.

---

## 3. Probe 2 — insert the generator *upstream* of propagation and let UsdImaging do the work

`instProbe2.cpp` passes a filtering SI through
`UsdImagingCreateSceneIndicesInfo::overridesSceneIndexCallback`, which
`sceneIndices.cpp:222-225` splices in **before** `UsdImagingExtentResolvingSceneIndex` (`:232-234`),
`UsdImagingPiPrototypePropagatingSceneIndex` (`:239-241`) and
`UsdImagingNiPrototypePropagatingSceneIndex` (`:281-284`). Five cases, all in `out-probe2.txt`:

| Case | What we injected upstream | What the terminal SI shows |
|---|---|---|
| (a) hair under a **native** prototype | `basisCurves` at `/__Prototype_1/Hair` | `…/UsdNiInstancer/UsdNiPrototype/Hair`, type `basisCurves`, `instancedBy.paths=[…/UsdNiInstancer]` — **auto-instanced across both scalps** |
| (b) hair under a **point-instancer** prototype | `basisCurves` at `/World/Cards/PI/Prototypes/Card/Hair` | original made `type=''`; propagated copy `…/Card/ForInstancer9c60…/Hair` type `basisCurves` with `instancedBy=[/World/Cards/PI]` — **subtree prototypes work** |
| (c) our own instancer, prototype **outside** it | `/World/HairCards/CardInstancer`, prototypes `[/World/HairCards/CardProto]` | pi-propagation adopted it: rewrote topology to `[…/CardProto/ForInstancer2d04f1b3dd73655]`, authored `instancedBy` on the copy. **But `/World/HairCards/CardProto` stays `type='mesh'` → also drawn standalone.** |
| (d) our own instancer, prototype **as namespace child** | `/World/SelfCards/Inst` + `/World/SelfCards/Inst/Proto` | propagated copy `…/Proto/ForInstancerb00dc941cb1d5a3c` is `basisCurves` with `instancedBy`; **original `Proto` forced to `type=''`** — free hiding |
| (f) instancer at a **subprim/property path** | `/World/HairCards.groomInstancer` + `/World/HairCards/SubProto` | works — topology rewritten, `instancedBy.paths=[/World/HairCards.groomInstancer]` — but `SubProto` stays renderable (property paths can't have children) |
| (e) our instancer **inside** the native prototype | `/__Prototype_1/NiCardInst` + `/NiCardInst/Proto` | **two-level instancing, automatic**: `…/UsdNiPrototype/NiCardInst` carries `instancedBy=[UsdNiInstancer]` *and* its own topology pointing at `…/NiCardInst/Proto/ForInstancerf3deba0854927837`, whose `instancedBy=[…/NiCardInst]` |

Case (d) is the design rule. The hiding comes from `UsdImaging_PiPrototypeSceneIndex`: it collects
every prim of type `instancer` (or specifier `over`) into `_instancersAndOvers`
(`piPrototypeSceneIndex.cpp:117-131,158-172`) and forces `primType = TfToken()` on anything with a
*strict prefix* in that set (`_MakeUnrenderable`, `:174-208`; `GetPrim`, `:210-223`). Putting the
prototype under the instancer prim buys exactly the USD `PointInstancer` semantics for free.

Case (e) is the answer to "hair cards on a natively instanced scalp": **a point-instancer-style
card instancer authored inside a native prototype nests correctly with zero extra code**, giving
`N_instances × N_cards` draws from one prototype rprim.

---

## 4. Probe 3 — the downstream path for nested instancing, if you must

If `usdGen` runs post-propagation (a `UsdImagingSceneIndexPlugin` at `sceneIndices.cpp:301-302`, or a
renderer-chain `HdSceneIndexPlugin`), nothing authors `instancedBy` for the prims we add. Probe 3
does it by hand: it discovers the propagated native prototype at runtime by scanning for
`__usdPrimInfo.isNiPrototype == true` and reading that prim's own `instancedBy.paths[0]`, then emits
`…/UsdNiPrototype/HairCardInst` (instancer, `instancedBy = [UsdNiInstancer]`, `prototypeRoots =
[UsdNiPrototype]`) and `…/HairCardInst/Proto` (`basisCurves`, `instancedBy = [HairCardInst]`).

`out-probe3.txt`:

```
discovered niProto     = /UsdNiPropagatedPrototypes/NoPrimvars___usdUpAxisd827…/__Prototype_1/UsdNiInstancer/UsdNiPrototype
discovered niInstancer = /UsdNiPropagatedPrototypes/NoPrimvars___usdUpAxisd827…/__Prototype_1/UsdNiInstancer
hasInstancer(synth) = 1
proto -> instancer   = …/UsdNiPrototype/HairCardInst
instanceIndices      = [0, 1, 2, 3, 4]
instancer -> parent  = …/__Prototype_1/UsdNiInstancer
parent indices for child instancer = [0, 1]
parent -> grandparent= ''
```

So the two-level chain survives emulation exactly as Storm needs it
(`hdSt/primUtils.cpp:1161-1199` walks `instancer->GetParentId()` assigning one instance-primvar
drawing-coord slot per level; `drawingCoord.h:102-107` — no fixed level cap, `_instancePrimvar` is
`int16_t`). Total draws = 5 cards × 2 scalp instances.

**Cost of the downstream route:** we replicate what `piPrototypePropagatingSceneIndex` does for free
(hash-named prototype copies per instancer, cycle detection at `:487-499`, prototype hiding), and we
must not let a prototype be claimed by two instancers (`paths.size() ≤ 1`). **Benefit:** it is the
only place that sees the *deformed* scalp — i.e. after rigExec/UsdSkel have run
(`sceneIndices.cpp:301-302` is after everything). The plan should therefore **split**: generation
(scatter, instancer topology, prototypes) upstream; deformation/styling of already-instanced curves
downstream.

---

## 5. Prototype visibility, per-instance materials, categories

* **Standalone-prototype suppression.** Downstream, authoring `instancedBy` is sufficient — Storm
  only ever draws such an rprim through the instancer (`hdSt/primUtils.cpp:1164-1220`,
  `hdSt/mesh.cpp:2923 bool hasInstancer = !GetInstancerId().IsEmpty();`). Upstream, put the
  prototype under the instancer and `_MakeUnrenderable` does it. If you must hide a prim
  explicitly, set `primType = TfToken()` (the same trick UsdImaging uses) — do **not** rely on
  `visibility=false`, which still allocates the rprim.
* **Per-instance material bindings do not exist** in Storm. Material is per-rprim
  (`hdSt/mesh.cpp:3267,3338`, `hdSt/basisCurves.cpp:609` all use `GetMaterialId()` of the rprim).
  The instancer schema docstring's "material relationships" (`instancerTopologySchema.h:54-55`) is
  aspirational. **Design consequence:** card/archive material variety must be expressed as *multiple
  prototypes* (one per material) with `instanceIndices` partitioned across them — exactly the USD
  `PointInstancer` `protoIndices` model (`dataSourcePointInstancer.cpp:352-363` shows the
  flip from `protoIndices` to per-prototype index arrays). Continuous variation (tint, melanin,
  root/tip colour) belongs in `instance`-interpolated primvars instead, which are unrestricted.
* **`instanceCategories`** is the per-instance channel that *does* exist, for light-linking /
  collections (`hd/instanceCategoriesSchema.h`, consumed at
  `sceneIndexAdapterSceneDelegate.cpp:2595-2599`, invalidated at `dirtyBitsTranslator.cpp:448-455`).
* **Native-instance aggregation splitting.** If two scalp instances must carry *different* grooms,
  they must not aggregate into one prototype. The hook is
  `UsdImagingSceneIndexPlugin::InstanceDataSourceNames()` (`sceneIndexPlugin.h:69-83`), merged into
  the aggregation key at `sceneIndices.cpp:159-164`. Listing `usdGen`'s prim-level data-source name
  there makes instances with different groom bindings aggregate separately. Symmetrically,
  `ProxyPathTranslationDataSourceNames()` (`sceneIndexPlugin.h:85-92`, used at
  `sceneIndices.cpp:180-188` and `:292-296`) is what translates a `usdGen:scalp` relationship that
  targets an instance proxy (`/World/HeadA/ScalpMesh`) into the prototype path. usdRig's redesign
  doc names exactly these two hooks as its deferred task #16
  (`/home/burkard/work/usdRig/docs/imaging-datasource-redesign.md:209-217`, exposure restated at
  `:332-334`); `usdGen` should implement them from day one rather than inherit the deferral.

---

## 6. Picking round-trip (probe 4)

`HdxPickHit.objectId` is the **render-index rprim path**, never remapped
(`hdx/pickTask.cpp:907,1031`), and `instanceIndex` is the *flattened* index across all instancing
levels (`:1144-1152`, decomposed at `:1188-1201` with `i = instanceIndex % n; instanceIndex /= n`).
`ctx.instanceId = instanceInfo.instanceIndices[i]` (`:1201`) — the reported id is the **value** in
the instance-indices array, i.e. the index into instance-rate primvars, which is exactly the
per-hair / per-card id we want; masked-out instances shrink `n` consistently.

`instProbe4.cpp` builds `HdxPickHit`s by hand and calls `HdxPrimOriginInfo::FromPickHit`
(`hdx/pickTask.cpp:1225-1235`) — no GL required. `out-probe4.txt`:

| Case | `GetFullPath()` | `ComputeInstancerContext()` |
|---|---|---|
| (a) synthesized instancer, **no** `primOrigin` | `/World/HairCards/CardProto` | **empty** |
| (b) same, `primOrigin.scenePath = /World/Groom/CardInstancer` | `/World/HairCards/CardProto` | `[(/World/Groom/CardInstancer, 2)]` |
| (c) stage USD point instancer (control) | `/World/Cards/PI/Prototypes/Card` | `[(/World/Cards/PI, 1)]` |
| (d) natively instanced scalp (control) | **`/World/HeadB/ScalpMesh`** | empty (native instancers contribute a path prefix, not a context entry) |

Case (a) is a real bug source: usdview reads `hit.instancerContext[0]` and falls back to
`Sdf.Path.emptyPath, -1` when empty (`usdviewq/stageView.py:2331-2340`;
`usdImagingGL/engine.cpp:1348-1372` builds it from `ComputeInstancerContext`). **Every prim
`usdGen` synthesizes — instancer and prototype alike — must carry an `HdPrimOriginSchema` data
source** naming the authored `usdGen` prim, or interactive selection of a card/archive silently
degrades. Case (d) also shows the rule for prims inside prototypes: `primOrigin.scenePath` must be
**relative** (`OriginPath(ScalpMesh)`), because `_AppendPrimOriginToPath` appends relative paths and
replaces on absolute ones (`hdx/pickTask.cpp:1271-1295`), and that is what turns instance 1 into
`/World/HeadB/…`.

---

## 7. Cards and archives specifically

| XGen concept (A7 §1.1) | usdGen expression | Notes |
|---|---|---|
| **Cards** ("individual NURBS planes attached to the patch at the plane pivot") | one `mesh` prototype (a quad) as a namespace child of the `usdGen` instancer; per-card T/R/S in `hydra:instanceTranslations/Rotations/Scales`; width/length variation via `instanceScales`; per-card colour/UV-offset via arbitrary `instance` primvars | Storm composes `T*R*S*instanceTransforms` at `instancing.glslfx:38-87`; no per-card rprim, so 1M cards = 1 rprim + 1 instancer |
| **Archives** (`.xarc` Alembic instances) | a **subtree** prototype: reference the asset under `…/Instancer/Prototypes/<Archive_k>`; `prototypes` may be subtree roots (`instancerTopologySchema.h:64-70`) and `ComputeInstanceIndicesForProto` matches by prefix (`instancerTopologySchema.cpp:55-59`) | Probe 2 case (b) verifies a 2-prim subtree prototype: both prims got `instancedBy` |
| **Spheres** | implicit `sphere` prim as prototype, or a `points` rprim — cheaper | out of scope here |
| Per-archive/per-card **variation by material** | one prototype per material, `instanceIndices` partitioned | forced by the per-rprim material limit (§5) |
| Per-instance **randomised look** | `instance`-interpolation primvars feeding the material network | unrestricted (`hdSt/primUtils.cpp:219-220`) |

---

## 8. Dirty propagation for instancers

`HdDirtyBitsTranslator::InstancerDirtyBitsToLocatorSet` (`dirtyBitsTranslator.cpp:418-456`) maps:

| Locator dirtied on the instancer | Storm dirty bit |
|---|---|
| `instancedBy` | `DirtyInstancer` |
| `instancerTopology` | `DirtyInstanceIndex` |
| `primvars` | `DirtyPrimvar` |
| `visibility` | `DirtyVisibility` |
| `xform` | `DirtyTransform` |
| `instanceCategories` + `categories` | `DirtyCategories` |

Practical rule for interactive editing: changing *only* per-card transforms should dirty
`primvars/hydra:instanceTranslations` (→ `DirtyPrimvar`, re-uploads one BAR,
`hdSt/instancer.cpp:178-200`) and **not** `instancerTopology`, which forces an index rebuild for
every prototype rprim (`hdSt/primUtils.cpp:1174-1178,1201-1220`). Adding/removing cards necessarily
dirties `instancerTopology`.

---

## 9. UNMEASURED — benchmark protocol for a workstation with a display

No GPU number appears in this report; nothing here was rendered (no DISPLAY, GLX-only garch —
ENVIRONMENT.md). Protocol to run where Storm can execute:

1. **Scene.** `probe.usda` scaled up: one scalp mesh (20k faces), one `usdGen` card instancer with
   prototype = 1 quad, `N ∈ {10³, 10⁴, 10⁵, 10⁶}` instances. Variant B: the same scalp made
   `instanceable` and referenced 8×. Variant C: prototype = a 500-poly archive subtree.
2. **Command.** `usdview --renderer GL --quitAfterNumFrames 200 scene.usda` with
   `HD_ENABLE_GPU_FRUSTUM_CULLING=1`, plus `usdrecord --renderer GL --imageWidth 1920`.
3. **Counters to record** (via `--traceToFile` / `Trace` + `HdPerfLog`):
   `hdSt/drawCalls`, `hd/instancer` sync time, `HdStInstancer::_SyncPrimvars` inclusive time,
   `rebuildBatches`, `instBatchDrawCalls`, GPU frame time from `HgiGL` timers, and
   `HdPerfTokens->instBatch*`.
4. **Expected shape (hypothesis to falsify, do not quote as fact):** draw-call count independent of
   `N`; frame time dominated by vertex work `∝ N × prototypeVerts`; `_SyncPrimvars` cost linear in
   `N` and re-triggered on every `primvars` dirty.
5. **Interactive edit test.** Dirty only `primvars/hydra:instanceTranslations` on 10⁶ instances vs.
   dirtying `instancerTopology`; record the ratio of re-sync times. This is the number that decides
   whether a "comb the cards" tool can run at interactive rates.
6. **Native-instance test.** Compare Variant B (8 instanceable scalps, hair inside the prototype)
   against 8 flattened copies: expect one prototype rprim vs eight, and `~1/8` the
   `HdStBasisCurves::Sync` time.

---

## Key facts

- **`instancerTopology/instanceIndices` is `HdIntArrayVectorSchema`** (vector of `VtIntArray`), not
  a vector of `HdInstanceIndicesSchema` containers as `extras/imaging/docs/hydra_prim_schemas.dox:233-245`
  claims; that dox is a self-declared November-2023 snapshot (`:4-5`). Authority:
  `pxr/imaging/hd/instancerTopologySchema.h:125-126`; `HdInstanceIndicesSchema` is a *selection*
  type (`hd/schemaTypeDefs.h:32`, `hdx/selectionSceneIndexObserver.cpp:176`).
- **A downstream scene index can synthesize a fully working instancer over stage-authored
  prototypes.** Verified end-to-end through Hydra-1.0 emulation with `Hd_UnitTestNullRenderDelegate`:
  `sd->GetInstancerId` = synthesized instancer, `GetInstanceIndices` = `[0,1,3]` with
  `mask=[1,1,0,1]`, `GetInstancerPrototypes` correct, four instance-rate primvars (including a
  custom `displayColor`) readable (`probes/instancing/out-probe1.txt` §C; `instProbe.cpp`).
- **Storm applies no filter to instance-rate primvars** — `hdSt/primUtils.cpp:211-221` returns the
  delegate's `HdInterpolationInstance` descriptors verbatim (`// XXX: Can we do filtering?`), so
  arbitrary per-card/per-hair primvars reach the shader.
- **A prim carrying `instancedBy` is drawn only through its instancer**; no separate hiding step is
  needed downstream (`hdSt/mesh.cpp:2923`, `hdSt/primUtils.cpp:1164-1220`). `instancedBy/paths`
  must hold **exactly one** path or emulation raises `TF_CODING_ERROR`
  (`sceneIndexAdapterSceneDelegate.cpp:2684-2688`).
- **Inserting the generator upstream of propagation makes UsdImaging do the hard parts.** With the
  SI spliced at `sceneIndices.cpp:222-225`: a `basisCurves` added at `/__Prototype_1/Hair` emerges
  at `…/UsdNiInstancer/UsdNiPrototype/Hair` with `instancedBy` authored; an instancer we author is
  adopted by pi-propagation (prototype re-rooted to `…/ForInstancer<hash>`, `instancedBy` authored);
  and if the prototype is a **namespace child of the instancer**, the original is forced to
  `primType=''` for free (`piPrototypeSceneIndex.cpp:174-208`). Evidence: `out-probe2.txt` cases
  (a),(b),(c),(d).
- **Nested instancing (cards inside a natively-instanced scalp) works both ways.** Upstream it is
  automatic (`out-probe2.txt` case (e): inner instancer carries `instancedBy=[UsdNiInstancer]` and
  its prototype carries `instancedBy=[innerInstancer]`). Downstream it can be composed by hand and
  survives emulation: `proto → HairCardInst ([0,1,2,3,4]) → UsdNiInstancer ([0,1])`
  (`out-probe3.txt`, `instProbe3.cpp`).
- **Propagated prototype path names are hashes and must be discovered, not constructed.**
  `ForInstancer%zx` from `TfHash::Combine(instancerPath, propagatedPrototype)`
  (`piPrototypePropagatingSceneIndex.cpp:406-433`); discovery vectors are
  `__usdPrimInfo.piPropagatedPrototypes`, `.niPrototypePath`, `.isNiPrototype`
  (`usdImaging/usdPrimInfoSchema.h:46-48`), all present in the terminal tree
  (`out-probe1.txt`).
- **Picking needs `primOrigin` on every synthesized prim.** Without it,
  `HdxPrimOriginInfo::ComputeInstancerContext()` returns an empty vector and usdview loses the
  instance selection (`out-probe4.txt` case (a) vs (b); `usdviewq/stageView.py:2331-2340`).
  Inside prototypes the `scenePath` must be **relative** — that is how instance 1 of the scalp
  resolves to `/World/HeadB/ScalpMesh` (`out-probe4.txt` case (d); `hdx/pickTask.cpp:1271-1295`).
- **There is no per-instance material binding in Storm**; material is per-rprim
  (`hdSt/mesh.cpp:3267,3338`, `hdSt/basisCurves.cpp:609`). Material variety across cards/archives
  requires multiple prototypes with partitioned `instanceIndices`.
- **An adapter subprim can be an instancer but cannot own its prototypes**:
  `SdfPath("/World/HairCards.groomInstancer")` works as an instancer prim through the whole chain
  (`out-probe2.txt` case (f)), but `AppendChild` on a property path is rejected
  (`sdf/path.cpp:841`, reproduced in Python), so the automatic prototype-hiding rule (strict prefix
  under the instancer) cannot apply.
- **Native-instance aggregation and instance-proxy path translation are plugin-extensible**:
  `UsdImagingSceneIndexPlugin::InstanceDataSourceNames()` and
  `ProxyPathTranslationDataSourceNames()` (`sceneIndexPlugin.h:69-92`) feed
  `sceneIndices.cpp:159-164` and `:180-188`. usdRig defers exactly this
  (`usdRig/docs/imaging-datasource-redesign.md:209-217`, `:332-334`).
- **Instance transform composition is `T·R·S` applied after `hydra:instanceTransforms`**
  (`hdSt/shaders/instancing.glslfx:38-87`), and instance-rate primvars are trimmed to the shortest
  array with a `TF_WARN` if lengths disagree (`hdSt/instancer.cpp:145-167`).

## Decisions this settles

1. **`usdGen` emits real `HdInstancer` prims for cards, archives and clump-instanced geometry.** The
   contract is small and fully verified here; there is no reason to expand cards into per-card
   meshes.
2. **Two scene indices, not one.**
   (i) A *generator* SI inserted at `UsdImagingCreateSceneIndicesInfo::overridesSceneIndexCallback`
   (`sceneIndices.cpp:222-225`) — it emits instancers, prototypes and prototype-local curves and
   lets pi/ni propagation handle re-rooting, `instancedBy`, hiding and nesting.
   (ii) A *resolver* SI registered as a `UsdImagingSceneIndexPlugin` (`sceneIndices.cpp:301-302`),
   which sees post-rig/post-skel deformed scalps and does deformation, styling and modifier
   evaluation on already-instanced curves.
   Host applications that only offer the plugin slot (usdview today) get (ii); (i) needs an
   application-controlled construction path, which is the same exposure usdRig already accepts.
3. **Prototypes are always namespace children of their instancer** (`…/Instancer/Prototypes/<name>`).
   This buys automatic suppression of the standalone prototype upstream and matches the
   `UsdGeomPointInstancer` convention downstream.
4. **Never hand-construct propagated prototype paths.** Resolve them from
   `__usdPrimInfo.piPropagatedPrototypes` / `isNiPrototype` / `niPrototypePath` at runtime.
5. **Every synthesized prim carries `HdPrimOriginSchema`** — absolute outside prototypes, relative
   inside them — so `usdview` selection, the freeze/comb tooling and `HdxPrimOriginInfo` all resolve
   back to the authored `usdGen` prim.
6. **Card/archive material variety = multiple prototypes**; continuous variety = `instance`
   primvars. Do not design a per-instance material binding; Storm has none.
7. **`usdGen`'s `UsdImagingSceneIndexPlugin` implements `InstanceDataSourceNames()` and
   `ProxyPathTranslationDataSourceNames()` from the first release**, so grooms on natively
   instanced characters aggregate correctly and `usdGen:scalp` relationships targeting instance
   proxies get translated. This is the item usdRig deferred.
8. **Interactive edits dirty `primvars`, not `instancerTopology`**, whenever the card/hair count is
   unchanged.
9. **Do not use an adapter subprim for the instancer.** Use a real child prim.

## Open questions

- **Is the `overridesSceneIndexCallback` slot reachable from stock `usdview`?** `UsdImagingGLEngine`
  exposes no setter for it (`grep` over `usdImagingGL/engine.{h,cpp}` finds only
  `UsdImagingRootOverridesSceneIndex`, `engine.cpp:1668-1669`), and
  `usdImaging/sceneIndex.h:51-54,157` takes it only as a constructor argument. If usdview cannot
  reach it, the "generator upstream" half of decision (2) needs either an app-side hook, an upstream
  patch, or a fallback to full downstream synthesis (probe 3 shows that is possible, at the cost of
  re-implementing propagation).
- **Ordering between `usdGen`'s plugin SI and rigExec's / UsdSkel's.** `GetAllSceneIndexPlugins`
  iterates a `std::set<TfType>` (A4 §5, `sceneIndexPlugin.cpp:41-88`) with no ordering control.
  Unresolved for the downstream resolver.
- **Cost of `HdRetainedSmallVectorDataSource` rebuild for 10⁶-instance topologies** on every
  `instancerTopology` dirty — UNMEASURED (needs the §9 protocol).
- **Whether `instanceLocations` can be usefully abused for `usdGen`**: it is documented as
  implicit-instancing-only (`instancerTopologySchema.h:83-90`) but `hdx/pickTask.cpp:1108-1118`
  reads it to name the picked instance. Populating it with per-card authored paths might give
  card-level selection without a `primOrigin` per instance — untested, and it would make the
  instancer look "implicit" to other consumers.
- **Motion blur for instance-rate primvars**: whether `hydra:instanceTranslations` is sampled at
  multiple shutter times by `HdSceneIndexAdapterSceneDelegate::SamplePrimvar` for instancers with
  the same contract as vertex primvars — cross-check against
  `G-motion-blur-sampling-strategy.md`, not verified here.
- **Ptex on instanced prototypes**: `hdSt/mesh.cpp:2443 _MaterialHasPtex` is per-rprim; whether a
  ptex face id can vary per instance (needed for XGen-style per-patch painted colour on cards) is
  unverified and interacts with the per-rprim material limit.
