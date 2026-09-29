# PW-5 — SI-8 pre-work: does API-schema auto-apply propagate to CODELESS derived types?

## 1. What was asked

Roadmap `plan/11-roadmap.md` §1.1, PW-5 row (line 113):

> | **PW-5** | SI-8 | whether the tool must keep applying `UsdGenMaskAPI` explicitly (ADR §2.1 requires it until SI-8 passes) | throwaway codeless schema with `UsdGenMaskAPI` under `AutoApplyAPISchemas`, read from plain Python through `FindConcretePrimDefinition("UsdGenNoise").GetPropertyNames()`. Derived-type propagation is implemented (`pxr/usd/usd/schemaRegistry.cpp:926-940`, the "Collect all the types to apply the API schema to" comment through the `GetAllDerivedTypes` call at `:937`, verified); the codeless case is not |

The open question: **does the derived-type auto-apply propagation fire for codeless types
declared from `plugInfo.json`** (no C++ schema classes)?

Supporting context:

- ADR §2.1 (`plan/design/adr-v1.md:56`): *"UsdGenMaskAPI (AUTO-APPLIED to
  UsdGenOperator via plugInfo AutoApplyAPISchemas; the tool also applies it
  explicitly until gate SI-8 proves auto-apply on codeless types)"*
- Gate SI-8 (`plan/09-performance-and-benchmarks.md` §5.2, line 575):
  *"auto-applied `UsdGenMaskAPI` on a codeless type | the API's properties appear on
  every `UsdGenOperator` subtype | `testUsdGenAutoApply` | M0 pre-work (PW-5), binding M1"*
- Risk RK-02 (`plan/12-risks-decisions-open-questions.md:233`): red outcome would mean
  "the mask block leaves `UsdPrimDefinition::GetPropertyNames()`" until SI-8 is green.
- Question Q-07 (`plan/12-risks-decisions-open-questions.md:316`): "Does an
  auto-applied `UsdGenMaskAPI` reach a codeless derived type in practice?"

The usdGen schema declares the auto-apply in two places, both in the build-tree
`build/usd/usdGenSchema/resources/plugInfo.json`:

- per-type: `"UsdGenMaskAPI": { "apiSchemaAutoApplyTo": ["UsdGenOperator"], ... }`
- Info-level block: `"AutoApplyAPISchemas": { "UsdGenMaskAPI": { "apiSchemaAutoApplyTo": ["UsdGenOperator"] } }`

`UsdGenMaskAPI` declares exactly one property: `float usdGen:mask:opacity = 1.0`
(`libs/usdGenSchema/schema.usda:115-125`; also in `build/usd/usdGenSchema/resources/generatedSchema.usda:145-148`).
`UsdGenNoise`, `UsdGenScatter`, `UsdGenLength` are concrete types deriving from
`UsdGenOperator`; `UsdGenGroom`/`UsdGenGuideSet` derive from `UsdGeomImageable` and
must NOT receive the API (control).

Note: the usdGen schema is fully codeless — `libusdGenSchema.so`
(`libs/usdGenSchema/usdGenSchema.cpp`) contains only the `UsdGenDescription`
compute-extent registration; all types come from `plugInfo.json` `Info.Types`.

## 2. Method + exact commands

Probe source: `docs/prework/probes/PW-5/probe.cpp` (build with `Makefile` next to it).

The probe:

1. registers the build-tree schema resources explicitly (`PlugRegistry::RegisterPlugins`);
   `PXR_PLUGINPATH_NAME` also carries them,
2. resolves the six usdGen `TfType`s (`TfType::FindByName`) to trigger plugin
   discovery + load **before** the `UsdSchemaRegistry` singleton is first built,
3. prints `UsdSchemaRegistry::GetAutoApplyAPISchemas()` (the raw API→base-type map,
   populated from the plugInfo `AutoApplyAPISchemas` block + per-type
   `apiSchemaAutoApplyTo` metadata; `pxr/usd/usd/schemaRegistry.cpp`
   `CollectAddtionalAutoApplyAPISchemasFromPlugins` +
   `_GetTypeToAutoAppliedAPISchemaNames`, which calls
   `schemaInfo->type.GetAllDerivedTypes(&applyToTypes)`),
4. prints `FindSchemaInfo(t)` for each type (kind + TfType),
5. prints `FindConcretePrimDefinition(...)->GetAppliedAPISchemas()` and
   `->GetPropertyNames()` for `UsdGenNoise`, `UsdGenScatter`, `UsdGenLength`
   (expect the mask property **without** any explicit API apply) and
   `UsdGenGroom` (expect it **absent** — control), plus the abstract base
   `FindAbstractPrimDefinition("UsdGenOperator")` and the API's own declared
   properties via `FindAppliedAPIPrimDefinition("UsdGenMaskAPI")`,
6. as a stage-level cross-check, creates an in-memory `UsdStage`, defines one prim
   of each type, and prints `UsdPrim::GetAppliedSchemas()` (no API is explicitly
   applied anywhere).

The roadmap suggested "plain Python"; C++ was used instead because
`FindConcretePrimDefinition` / `GetPropertyNames` / `GetAppliedAPISchemas` are the
exact C++ entry points the Python `usd.schema` wrappers call — the registry state is
identical in both. No GPU/EGL involved.

Build and run (also `make run` from the probe dir):

```
cd <usdgen-src>/docs/prework/probes/PW-5
PXR=$USD
g++ -std=c++17 -O1 -w probe.cpp -o probe \
  -I $PXR/include -I /usr/include/python3.12 -L $PXR/lib \
  -lusd_usd -lusd_usdGeom -lusd_tf -lusd_sdf -lusd_gf -lusd_arch \
  -lusd_kind -lusd_vt -lusd_trace -lusd_work -lusd_js -lusd_plug \
  -lusd_ar -lusd_hio -lusd_usdImaging -lusd_hd -lusd_hdsi -lusd_python \
  -Wl,-rpath,$PXR/lib
LD_LIBRARY_PATH=$PXR/lib \
PXR_PLUGINPATH_NAME="<usdgen-src>/build/usd/usdGenSchema/resources:\
<usdgen-src>/build/usd/usdGenImaging/resources:$PXR/plugin/usd" \
./probe 2>&1 | tee run1.txt
```

Host: headless aarch64 Linux, g++ 13.3, C++17, OpenUSD 26.08
(`$USD`, flat-namespace
`pxrInternal_v0_26_8__pxrReserved__` build).

## 3. Raw evidence

Full output (`docs/prework/probes/PW-5/run1.txt`), verbatim:

```
== plugin path ==
PXR_PLUGINPATH_NAME=<usdgen-src>/build/usd/usdGenSchema/resources:<usdgen-src>/build/usd/usdGenImaging/resources:$USD/plugin/usd

== TfType resolution (triggers plugin discovery + load) ==
  UsdGenNoise : UsdGenNoise
  UsdGenScatter : UsdGenScatter
  UsdGenLength : UsdGenLength
  UsdGenGroom : UsdGenGroom
  UsdGenOperator : UsdGenOperator
  UsdGenMaskAPI : UsdGenMaskAPI
  UsdGenNoise IsA(UsdGenOperator) = yes
  UsdGenScatter IsA(UsdGenOperator) = yes
  UsdGenLength IsA(UsdGenOperator) = yes
  UsdGenGroom IsA(UsdGenOperator) = no

== UsdSchemaRegistry::GetAutoApplyAPISchemas() ==
  UsdGenMaskAPI -> [UsdGenOperator,UsdGenOperator]

== FindSchemaInfo (registration evidence) ==
  UsdGenNoise : identifier=UsdGenNoise kind=concreteTyped TfType=UsdGenNoise
  UsdGenScatter : identifier=UsdGenScatter kind=concreteTyped TfType=UsdGenScatter
  UsdGenLength : identifier=UsdGenLength kind=concreteTyped TfType=UsdGenLength
  UsdGenGroom : identifier=UsdGenGroom kind=concreteTyped TfType=UsdGenGroom
  UsdGenOperator : identifier=UsdGenOperator kind=abstractTyped TfType=UsdGenOperator
  UsdGenMaskAPI : identifier=UsdGenMaskAPI kind=singleApplyAPI TfType=UsdGenMaskAPI

== prim definition property sets ==
UsdGenNoise   ; appliedAPISchemas=[UsdGenMaskAPI]  (6 props): usdGen:algorithmVersion,usdGen:enabled,usdGen:frequency,usdGen:magnitude,usdGen:mask:opacity,usdGen:seed
UsdGenScatter ; appliedAPISchemas=[UsdGenMaskAPI]  (6 props): usdGen:algorithmVersion,usdGen:density,usdGen:enabled,usdGen:mask:opacity,usdGen:mode,usdGen:seed
UsdGenLength  ; appliedAPISchemas=[UsdGenMaskAPI]  (4 props): usdGen:algorithmVersion,usdGen:enabled,usdGen:length,usdGen:mask:opacity
UsdGenGroom   ; appliedAPISchemas=[]  (6 props): proxyPrim,purpose,usdGen:schemaVersion,usdGen:sessionId,usdGen:surface,visibility
UsdGenOperator (abstract); appliedAPISchemas=[UsdGenMaskAPI]  (3 props): usdGen:algorithmVersion,usdGen:enabled,usdGen:mask:opacity
UsdGenMaskAPI  (applied API def); appliedAPISchemas=[UsdGenMaskAPI]  (1 props): usdGen:mask:opacity

== stage-level check: applied schemas on a live prim ==
  UsdGenNoise   applied schemas: [UsdGenMaskAPI]
  UsdGenScatter applied schemas: [UsdGenMaskAPI]
  UsdGenLength  applied schemas: [UsdGenMaskAPI]
  UsdGenGroom   applied schemas: []

== verdict: does the prim definition already carry 'usdGen:mask:opacity'? ==
  UsdGenNoise   contains mask prop : YES
  UsdGenScatter contains mask prop : YES
  UsdGenLength  contains mask prop : YES
  UsdGenGroom   contains mask prop : no (expected)

== summary ==
PASS: auto-apply propagated to every codeless UsdGenOperator-derived type; control (UsdGenGroom) correctly excluded.
```

Reading:

- All six usdGen types register from `plugInfo.json` with the correct kinds
  (`concreteTyped` / `abstractTyped` / `singleApplyAPI`) and the TfType hierarchy is
  intact (`UsdGenNoise/Scatter/Length` `IsA(UsdGenOperator)` = yes, `UsdGenGroom` = no).
- `GetAutoApplyAPISchemas()` contains the mapping `UsdGenMaskAPI -> UsdGenOperator`
  (twice — see Risks; the registry dedupes during composition, so the property
  appears exactly once in every property set below).
- `UsdGenMaskAPI`'s own prim definition is non-empty
  (`usdGen:mask:opacity`), so the task's "if the API's property set is empty, print
  the API's declared properties instead" fallback was **not** needed — but the
  declared set is printed anyway (row `UsdGenMaskAPI (applied API def)`).
- **Every codeless `UsdGenOperator` subtype's composed prim definition carries
  `usdGen:mask:opacity` and lists `UsdGenMaskAPI` in
  `GetAppliedAPISchemas()` without any explicit apply** — the derived-type
  propagation (`GetAllDerivedTypes`, `schemaRegistry.cpp` in the "Collect all the
  types to apply the API schema to" block) fires for plugInfo-declared codeless
  types on OpenUSD 26.08.
- The control `UsdGenGroom` (derives `UsdGeomImageable`, not `UsdGenOperator`) has
  `appliedAPISchemas=[]` and no `usdGen:mask:opacity` — propagation respects the
  type hierarchy.
- Stage-level cross-check agrees: on a live in-memory stage,
  `UsdPrim::GetAppliedSchemas()` reports `[UsdGenMaskAPI]` for the three operator
  prims and `[]` for `UsdGenGroom`.

## 4. Decision

DECISION: SI-8 (PW-5) **PASSES** — auto-apply of `UsdGenMaskAPI` **does** propagate to
codeless derived types on OpenUSD 26.08: `UsdGenNoise`, `UsdGenScatter` and
`UsdGenLength` each carry `usdGen:mask:opacity` in
`FindConcretePrimDefinition(...).GetPropertyNames()` (and `UsdGenMaskAPI` in
`GetAppliedAPISchemas()` / the stage prim's `GetAppliedSchemas()`) with no explicit
API application, while the non-operator control `UsdGenGroom` correctly does not.
Therefore the tool does **NOT** need to keep applying `UsdGenMaskAPI` explicitly on
every operator it authors; the ADR §2.1 explicit-apply fallback is cleared at the
M0-prework level. The gate still **binds at M1** via `testUsdGenAutoApply`
(§5.2 row SI-8), which should encode this measurement.

## 5. Risks / follow-ups

- **Duplicate registration in the auto-apply map.** `GetAutoApplyAPISchemas()`
  shows `UsdGenMaskAPI -> [UsdGenOperator,UsdGenOperator]` because *both* the
  per-type `apiSchemaAutoApplyTo` (inside `Info.Types.UsdGenMaskAPI`) and the
  Info-level `AutoApplyAPISchemas` block in `plugInfo.json` are registered
  (`CollectAddtionalAutoApplyAPISchemasFromPlugins` appends to an existing entry,
  `schemaRegistry.cpp:902-912`). Composition dedupes
  (`seenSchemaFamilyVersions` in `_ComposeAPISchemasIntoPrimDefinition`), so the
  property appears exactly once — harmless, but redundant metadata. Follow-up
  (M1, not this pre-work): single-source it, or keep both as belt-and-braces and
  make `testUsdGenAutoApply` robust to the duplicate.
- **Scope of this measurement.** Proven at the *schema-registry* level
  (composed prim definitions) and the *stage* level (applied schemas on live
  prims). Not proven: attribute *value* resolution of `usdGen:mask:opacity` on a
  stage prim, or Hydra/scene-index dirtying when it changes — that is the M1
  `testUsdGenAutoApply` gate ("confirmed on shipped code"). If that later gate
  fails, the ADR §2.1 explicit-apply fallback reinstates.
- **Version fragility.** The propagation path is OpenUSD-internal behavior
  (`_GetTypeToAutoAppliedAPISchemaNames` / `GetAllDerivedTypes`), not a public API
  contract. Measured on OpenUSD 26.08 (aarch64, flat-namespace build); re-run
  this probe on any OpenUSD upgrade before relying on auto-apply.
- **Python-variant not exercised.** The roadmap's method line names plain Python;
  the C++ probe hits the same registry entry points Python wraps. Trivial to add a
  one-line Python check (`usd.schema` prim definition property names) to
  `testUsdGenAutoApply` if strict method compliance is wanted.
- **New operator subtypes.** Every `UsdGenOperator` subtype added at M1+ is covered
  automatically by the `GetAllDerivedTypes` expansion (no per-type declaration
  needed); `testUsdGenAutoApply` should iterate the full concrete-operator type set
  rather than the three types pinned here.
