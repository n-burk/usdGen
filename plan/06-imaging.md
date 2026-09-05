# Imaging: scene indices, adapters and publication

Date: 2026-09-04. Status: plan v1 (draft, pending review).

This document specifies `usdGenImaging`: the library that connects the usdGen evaluator to Hydra 2.0.
It fixes the four plugin registrations, the UsdImaging adapters that carry every `usdGen:*` property
into data sources, the renderer-level `UsdGenGroomSceneIndex` that owns the published generation, the
exact contract of every prim usdGen synthesizes, the invalidation discipline that keeps Storm on its
fast paths, how time and render context reach the evaluator, what changes under hdPrman and
`usdrecord`, the two extent channels, the diagnostic surface, and the tests and gates that prove all
of it. Everything here is stage-free by construction (S8): the C ABI is an accelerator for interactive
tooling, never a correctness requirement.

Reads with: `01-architecture.md` (the invariants I1–I8, ADR §9 R1), `02-schema.md` §2 and §6 (the
single normative property registry and the property→locator table, R7),
`03-execution-engine.md` §5–§6 (`UsdGenGraphDesc`, chunks, the dirty router, the commit, the
generation handoff), `05-static-curves-and-deformation.md` §5.7 (frozen curves, the always-tiles
rule, the overlay case), `07-look-maps-expressions.md` §2–§3 (the three material terminals and the
glslfx), `08-tools.md` §1.4 (contract C4, the C ABI — the single source this document repeats) and
§2 (the brush loop), `09-performance-and-benchmarks.md` §0.2 (the only frame ledger, R41) and §5
(the single gate registry, R40), `10-build-dependencies-testing.md` §1.2, §3.5, §4.3, §4.4, §5.1
and §7.2 (targets, the single env-var registry, the schema plugInfo, the plugInfo entries usdGen
ships, test tiers, the six `TfDebug` codes, R35),
`11-roadmap.md` (milestones M0–M8).

---

## 0. Overview and evidence

### 0.1 What this document decides

usdGen publishes hair as ordinary Hydra `basisCurves` prims from a **renderer-level filtering scene
index** that runs after the entire UsdImaging chain and before every Storm and hdPrman plugin (S1,
S2). It reads its operator graph from typed data sources published by its own UsdImaging **prim
adapters** (S10), reads deformed surfaces through a **private** ext-computation pruning wrapper (S3),
evaluates on the commit thread only (S17–S19), publishes an immutable generation with one atomic
store, and emits the narrowest dirty locators that Storm will act on (S30, ADR §5.2).

Five facts carry the design. Each was measured, not assumed:

| # | Fact | Evidence |
|---|---|---|
| F1 | A renderer-level `HdSceneIndexPlugin` at phase 0 / `InsertionOrderAtEnd` lands strictly after the whole UsdImaging chain (RigExec, UsdSkel, flattening, native-instance propagation) and before every Storm/hdPrman plugin. | S1; `research/G-chain-order-probe.md` §3 (MEASURED, 24-node chain dump) |
| F2 | Without a UsdImaging adapter, `usdGen:*` attributes and relationships are **invisible** to Hydra and produce **no notice at all** — including `usdGen:input` retargets. | S10; `research/G-stage-free-parameter-and-time-transport.md` §1, §5 (MEASURED twice); `research/G-evaluation-scheduling-and-batching.md` §4 finding S5 |
| F3 | Storm's upload unit is the prim, and `points.size()` must equal `Σ curveVertexCounts` exactly or the prim renders fallback red with only a `TF_WARN`. | S28; `research/G-storm-throughput-and-prim-granularity.md` §1.3–1.4 (MEASURED) |
| F4 | A frame change delivers exactly **2** `PrimsDirtied` calls regardless of scene size; the `/` + `sceneGlobals/currentFrame` dirty is the last one when nothing batches. | S18; `research/G-evaluation-scheduling-and-batching.md` §4, §9 (MEASURED for N = 1…5 000 scalps) |
| F5 | `GetPrim` must be threadsafe; observer callbacks need not be. 8 readers × 20 publishes over an atomic snapshot gave 16 734 reads and **0 torn reads**. | `pxr/imaging/hd/sceneIndex.h:98,110`; `pxr/imaging/hd/sceneIndexObserver.h:123,132,143,151`; `research/G-evaluation-scheduling-and-batching.md` §5, §7 (MEASURED) |

### 0.2 Settled decisions this document implements

S1–S7 (placement, the private pruning wrapper, post-flattening spaces, the bare-`primvars` rule, hdGp
rejected, the metadata-only UsdImaging plugin), S8–S16 (stage-free transport, codeless schemas, the
adapter, ramps, the REST adapter, asset resolution, lazy dependency registration, the process-global
registry, OpenExec deferred), S17–S20 (never cook in `GetPrim` or in every `_PrimsDirtied`; deferred
commit + atomic publish + diffed dirties; `asyncAllow`/`asyncPoll`), S27–S34 (32–256 curve prims per
description, exact-size arrays, the curve contract, invalidation discipline, the measured Storm
numbers, motion profiles, instancers and native-instance grooms). ADR §1 records the amendments:
**chunk ≠ tile** (S23/S27), the `after: ["hd:sceneGlobals"]` tag is decorative (S1), the `GetPrim`
cook backstop is **withdrawn** (S18c), `hairId` is a uniform **float** (S29), and the Storm material
override ships default OFF behind gate L-1 (S36).

ADR §9 (the addendum) rules on this chapter directly, and those rulings win wherever an earlier ADR
section disagrees. In ruling order: **R1** (invariants are I1–I8; appendix A rows are `EV-001…` and
are the only citation handle for a measured number, §4.1, §5.1, §6.3), **R2** (test tiers and gate
families, §10), **R3** (class names, §2.1, §3.1), **R4** (`UsdGenHairPreviewPrimvar`, §5.1),
**R5** (the Python package is `usdgen`, so the usdview plugin registers
`usdgen.usdGenUsdview.UsdGenUsdviewContainer`, §1.4), **R6** (no prefix filter in the adapter
mapping, §2.2), **R12**/**R13** (`curveId` is `uint64`, `hairId` and the decimation salt, §4.1,
§6.4), **R15** (`geomSubset` resolution and where `UsdGenRestAPI` may be applied, §2.6, §3.10),
**R19** (the compute-extent function lives in `libusdGenSchema.so`, §8), **R20**
(`std::atomic_load`/`std::atomic_store` over a `std::shared_ptr` generation; `VtArray::IsUnique()`
does not exist, §3.4, §3.11), **R21** (tile arithmetic — **49 tiles** at 100 k — and the `tileTarget`
edit, §0.4, §4, §8), **R24** (`elementSize = 3` on the guide planes, §4.1), **R25** (the router key
is the full locator path, §2.2, §3.3), **R28** (SI-6's traversal bound, §3.6), **R29** (the
`visibility` overlay; no in-place path in v1, §3.4.1, §5.1), **R31** (`08-tools.md` §1.4 is the C
ABI, §3.8), **R32** (trigger (c) always applies, §3.9), **R34** (all-renderers ordering under
hdPrman, §3.5, §7), **R35** (the env-var registry at `10-build-dependencies-testing.md` §3.5 and the
six `TfDebug` codes at its §7.2, §9), **R37** (the `usdGen` core link rule, §8), **R38** (v1 = the
M0–M7 set, §5.3, §11), **R39** (no T4 gate is a milestone exit, §10), **R40** (the gate registry,
§10.1), **R41** (`09-…` §0.2 is the only frame ledger, §8, §10.1), **R42** (number tags: MEASURED /
`DERIVED from EV-nnn` / UNMEASURED / ASSUMPTION) and **R45** (every "see `NN-…md` §X" points at a
section that exists and says what is claimed).

### 0.3 Position in the chain

```
UsdImagingStageSceneIndex          <- our prim adapters run here (§2)
  … RigExec / UsdSkel resolving / material bindings resolving …
  UsdImagingNiPrototypePropagatingSceneIndex  (+ flattening)
  UsdImagingSelectionSceneIndex
HdMergingSceneIndex                (usdImagingGL/engine.cpp:1544)
 [all,0 AtStart] engine app callback: HdsiSceneGlobalsSceneIndex,
                 HdsiDomeLightCameraVisibilitySceneIndex,
                 HdsiSceneMaterialPruningSceneIndex     (engine.cpp:148-166, 183-202)
 [all,0 AtEnd ]  UsdGenGroomSceneIndex                  <-- usdGen (§3)
 [GL ,0..1000]   Storm's 13 plugins  |  [RIS,0..1000] hdPrman's 26 plugins
HdSceneIndexAdapterSceneDelegate -> render delegate
```

The engine's own app scene indices are appended by one callback (`_Append`,
`pxr/usdImaging/usdImagingGL/engine.cpp:148-166`) registered at phase 0 with
`InsertionOrderAtStart` (`_Register`, `:183-202`; ADR §9 R43 accepts `engine.cpp:148-201` for this
callback), so phase 0 / `InsertionOrderAtEnd` puts usdGen immediately after
`HdsiSceneGlobalsSceneIndex` and before everything renderer-specific. That ordering
is structural (F1), not a consequence of the JSON `after` tag: no plugin in stock 26.08 carries an
`hd:sceneGlobals` tag, and unknown `after` tags are ignored
(`pxr/imaging/hd/sceneIndexPluginRegistry.cpp:606-615`; ADR §1 S1). Keep the tag as documentation;
never rely on it. The two renderer-specific counts are exact, not approximate: 13 entries whose
`bases` is `HdSceneIndexPlugin` in `pxr/imaging/hdSt/plugInfo.json` and 26 in
`third_party/renderman/plugin/hdPrman/plugInfo.json` (counted in the 26.08 tree).

### 0.4 Vocabulary

| Term | Meaning |
|---|---|
| **chunk** | 512 curves, curve-aligned; the engine's unit of dirtiness and parallelism (S23 as amended, ADR §1) |
| **tile** | one published `basisCurves` prim = `chunksPerTile` chunks, with `chunksPerTile = max(1, ceil(nChunks/tileTarget))` and `nTiles = min(nChunks, clamp(ceil(nChunks/chunksPerTile), 32, 256))` (ADR §1 as ADR §9 R21 fixes the arithmetic; invariant I5). Worked: 100 k curves, chunk 512, `usdGen:tileTarget` 64 → 196 chunks, 4 chunks per tile, **49 tiles**; 64 tiles is the 1 M-curve figure |
| **generation** | an immutable `UsdGenGeneration`: all tiles, guides and instancers for one frame of one session, published by one atomic store (`std::atomic_store` over a `std::shared_ptr<const UsdGenGeneration>`, §3.4; ADR §9 R20) |
| **session** | one `(stage \| usdGen:sessionId, groom root)` evaluator state, owned by `UsdGenImagingRegistry`; several scene indices may attach to one session (ADR §4.5) |
| **publication** | the per-prim payload inside a generation (`UsdGenTilePublication` and friends) |

---

## 1. Registrations (exactly four)

ADR §5.1 fixes four registrations. All four live in the `usdGenImaging` and `usdGenUsdview` build
targets (`10-build-dependencies-testing.md` §1.2).

| # | Registered type | Base | Purpose | Settled by |
|---|---|---|---|---|
| 1 | `UsdGenGroomSceneIndexPlugin` | `HdSceneIndexPlugin` | the value path: evaluate and publish curves | S1, S2 |
| 2 | `UsdGenMetadataSceneIndexPlugin` | `UsdImagingSceneIndexPlugin` | returns its input; exists only for the two instancing hooks | S7 |
| 3 | five prim adapters + one API-schema adapter | `UsdImagingSceneIndexPrimAdapter`, `UsdImagingAPISchemaAdapter` | parameter transport | S10, S12, ADR §2.3 |
| 4 | `usdgen.usdGenUsdview.UsdGenUsdviewContainer` | `pxr.Usdviewq.plugin.PluginContainer` | the tool plugin | S43, ADR §9 R5 |

Both the plugInfo JSON entry **and** the C++ `RegisterSceneIndexForRenderer` call ship for (1): under
the default `Hybrid` ordering policy a C++ registration whose plugin id has no JSON entry is silently
dropped (`pxr/imaging/hd/sceneIndexPluginRegistry.cpp:862-878`;
`research/G-hdprman-and-usdrecord-render-time-chain.md` §2.2), and `loadWithRenderer` is mandatory
(`pxr/imaging/hd/sceneIndexPlugin.h:35-39`).

### 1.1 `usdGenImaging/resources/plugInfo.json` (generated; verbatim)

`LibraryPath` is substituted from `$<TARGET_FILE_NAME:usdGenImaging>` and the file is generated into
`<build>/usd/usdGenImaging/resources` so one relative hop works in the build tree and the install
tree — the usdRig pattern (S44; `usdRig/plugin/rigExecImaging/resources/plugInfo.json.in`,
`usdRig/CMakeLists.txt:492-518`).

```json
{
    "Plugins": [
        {
            "Info": {
                "Types": {
                    "UsdGenGroomSceneIndexPlugin": {
                        "bases": ["HdSceneIndexPlugin"],
                        "displayName": "usdGen groom resolution",
                        "loadWithRenderer": "",
                        "priority": 0,
                        "tags": ["usdGen:groom"],
                        "ordering": {
                            "after": ["hd:sceneGlobals"],
                            "before": ["hdGp:proceduralResolution",
                                       "hdPrman:motionBlur"]
                        }
                    },
                    "UsdGenMetadataSceneIndexPlugin": {
                        "bases": ["UsdImagingSceneIndexPlugin"],
                        "displayName": "usdGen instancing metadata"
                    },
                    "UsdGenOperatorAdapter": {
                        "bases": ["UsdImagingSceneIndexPrimAdapter"],
                        "primTypeName": "UsdGenOperator",
                        "includeDerivedPrimTypes": true
                    },
                    "UsdGenMapAdapter": {
                        "bases": ["UsdImagingSceneIndexPrimAdapter"],
                        "primTypeName": "UsdGenMap",
                        "includeDerivedPrimTypes": true
                    },
                    "UsdGenGroomAdapter": {
                        "bases": ["UsdImagingSceneIndexPrimAdapter"],
                        "primTypeName": "UsdGenGroom"
                    },
                    "UsdGenDescriptionAdapter": {
                        "bases": ["UsdImagingSceneIndexPrimAdapter"],
                        "primTypeName": "UsdGenDescription"
                    },
                    "UsdGenGuideSetAdapter": {
                        "bases": ["UsdImagingSceneIndexPrimAdapter"],
                        "primTypeName": "UsdGenGuideSet"
                    },
                    "UsdGenRestAPIAdapter": {
                        "bases": ["UsdImagingAPISchemaAdapter"],
                        "apiSchemaName": "UsdGenRestAPI"
                    }
                }
            },
            "LibraryPath": "../../@USDGEN_IMAGING_LIBRARY_FILENAME@",
            "Name": "usdGenImaging",
            "ResourcePath": "resources",
            "Root": "..",
            "Type": "library"
        }
    ]
}
```

`priority` and `displayName` are required fields on a Hydra plugin entry; `priority` is not used for
scene index plugins (`pxr/imaging/hdSt/plugInfo.json:14-18`, which says so in a comment). Insertion
phase and order are **not** JSON keys — they come from the C++ call in §1.2, which the registry then
turns into manufactured `phase0` tags composed with the JSON `tags`/`ordering`
(`pxr/imaging/hd/sceneIndexPluginRegistry.cpp:914-1005`).

`includeDerivedPrimTypes` is a bool and propagates the adapter down the schema hierarchy through
`PlugRegistry::GetDirectlyDerivedTypes` for every derived type that has no adapter of its own
(`pxr/usdImaging/usdImaging/adapterRegistry.cpp:132-243`). It works for codeless types because
`UsdSchemaRegistry` registers them from plugInfo metadata (`research/A4-openusd-hdgp-adapters.md`
§2.1; `research/G-stage-free-parameter-and-time-transport.md` §1, MEASURED).

**Five prim entries, not one.** `primTypeName` is a single string
(`pxr/usdImaging/usdImaging/adapterRegistry.cpp:104-131`), so one adapter cannot claim five type
names from one plugInfo entry. `UsdGenGroom`, `UsdGenDescription` and `UsdGenGuideSet` derive from
`UsdGeomImageable`, not from `UsdGenOperator`, so `includeDerivedPrimTypes` on the operator base does
not reach them (ADR §2.3; `design/judge-evidence.md` §2.1 — this was the most consequential shared
defect in all three proposals). The five entries name five one-line `final` subclasses of a single
implementation, `UsdGenPrimAdapterBase` (§2.1), so there is exactly one body of adapter logic.

### 1.2 C++ registration and `_IsEnabled`

```cpp
// usdGenImaging/groomSceneIndexPlugin.h
class UsdGenGroomSceneIndexPlugin final : public HdSceneIndexPlugin
{
public:
    UsdGenGroomSceneIndexPlugin();
protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(
        const std::string &renderInstanceId,
        const HdSceneIndexBaseRefPtr &inputScene,
        const HdContainerDataSourceHandle &inputArgs) override;
    bool _IsEnabled(const HdContainerDataSourceHandle &inputArgs) const override;
};
```

Signatures verified at `pxr/imaging/hd/sceneIndexPlugin.h:120-134`: the three-argument
`_AppendSceneIndex` overload wins when both are overridden, and `_IsEnabled` is `const`.

```cpp
// usdGenImaging/groomSceneIndexPlugin.cpp
TF_DEFINE_ENV_SETTING(USDGEN_ENABLE, true,
                      "Enable the usdGen groom scene index (kill switch).");

TF_REGISTRY_FUNCTION(TfType)
{
    HdSceneIndexPluginRegistry::Define<UsdGenGroomSceneIndexPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        HdSceneIndexPluginRegistryTokens->allRenderers.GetString(),  // ""
        TfToken("UsdGenGroomSceneIndexPlugin"),
        /* inputArgs      = */ nullptr,
        /* insertionPhase = */ 0,
        HdSceneIndexPluginRegistry::InsertionOrderAtEnd);
}

bool
UsdGenGroomSceneIndexPlugin::_IsEnabled(
    const HdContainerDataSourceHandle &inputArgs) const
{
    return TfGetEnvSetting(USDGEN_ENABLE);
}

HdSceneIndexBaseRefPtr
UsdGenGroomSceneIndexPlugin::_AppendSceneIndex(
    const std::string &renderInstanceId,
    const HdSceneIndexBaseRefPtr &inputScene,
    const HdContainerDataSourceHandle &inputArgs)
{
    return UsdGenGroomSceneIndex::New(inputScene, inputArgs, renderInstanceId);
}
```

`allRenderers` is the empty token and `rendererDisplayName` is `"__rendererDisplayName"`
(`pxr/imaging/hd/sceneIndexPluginRegistry.h:27-31`). The registry overlays the display name onto
`inputArgs` per append (`sceneIndexPluginRegistry.cpp:1434-1457`). usdGen reads it **for diagnostics
only**: the display name is not intent, and the interactive/render context is explicit (ADR §2.3,
§6.4 below). A disabled plugin returns its input scene
(`pxr/imaging/hd/sceneIndexPluginRegistry.cpp:1196-1199`), so `USDGEN_ENABLE=0` removes usdGen from
every chain with no other change.

`renderInstanceId` is the per-engine string `"UsdImagingGLEngine_<renderer>_<ptr>"`. usdGen keeps it
for the diagnostics HUD and for `UsdGenImagingRegistry` bookkeeping when several engines exist in one
process; it is never a session key (session keys are the stage or `usdGen:sessionId`, ADR §4.5).

### 1.3 The metadata plugin (S7)

```cpp
// usdGenImaging/metadataSceneIndexPlugin.h
class UsdGenMetadataSceneIndexPlugin final : public UsdImagingSceneIndexPlugin
{
public:
    HdSceneIndexBaseRefPtr AppendSceneIndex(
        const HdSceneIndexBaseRefPtr &inputScene) override { return inputScene; }

    TfTokenVector InstanceDataSourceNames() override;             // { "usdGen" }
    TfTokenVector ProxyPathTranslationDataSourceNames() override; // { "usdGen" }
};
```

Both base virtuals are **non-const** (`pxr/usdImaging/usdImaging/sceneIndexPlugin.h:83,92`). Writing
`const override` is a hard compile error; `design/judge-evidence.md` §2.4 caught exactly that in one
proposal.

The plugInfo entry alone does not make the plugin exist. A `UsdImagingSceneIndexPlugin` subclass is
discovered through its own registry function, exactly as the header's example shows
(`pxr/usdImaging/usdImaging/sceneIndexPlugin.h:45-48`):

```cpp
// usdGenImaging/metadataSceneIndexPlugin.cpp
TF_REGISTRY_FUNCTION(TfType)
{
    TfType::Define<UsdGenMetadataSceneIndexPlugin,
                   TfType::Bases<UsdImagingSceneIndexPlugin>>();
}

TF_REGISTRY_FUNCTION(UsdImagingSceneIndexPlugin)
{
    UsdImagingSceneIndexPlugin::Define<UsdGenMetadataSceneIndexPlugin>();
}
```

`UsdImagingCreateSceneIndices` collects both hooks by iterating
`UsdImagingSceneIndexPlugin::GetAllSceneIndexPlugins()`
(`pxr/usdImaging/usdImaging/sceneIndices.cpp:159-164`, `:180-188`). Without the registry function the
plugin is never in that list, both hooks are silently absent, and grooms on native instances
aggregate wrongly with no diagnostic (§2.8 case 1). `testUsdGenInstanceKeys` asserts `usdGen` is in
the aggregation key (§10.2).

The plugin adds nothing to the chain (`AppendSceneIndex` returns its input), so its position in the
UsdImaging plugin slot — which is ordered by `std::set<TfType>` on private pointers and is therefore
unspecified (`research/A2-usdrig-imaging.md` §2.1) — is irrelevant. It exists for two hooks:

* `InstanceDataSourceNames()` adds `usdGen` to the native-instance aggregation key
  (`pxr/usdImaging/usdImaging/sceneIndices.cpp:159-164`), so two scalp instances whose grooms differ
  only in `usdGen:*` values do **not** aggregate into one prototype. Without it the propagated
  prototype name is keyed on `NoPrimvars___usdUpAxis<hash>` and the difference is silently lost
  (`research/G-stage-free-parameter-and-time-transport.md` §4, MEASURED).
* `ProxyPathTranslationDataSourceNames()` makes a `usdGen:surface` relationship that targets an
  instance proxy (`/World/HeadA/ScalpMesh`) translate to the prototype path
  (`sceneIndices.cpp:180-188`, `:292-296`).

usdRig defers both (`usdRig/docs/imaging-datasource-redesign.md:209-217`); usdGen ships them from M0
(`research/G-instancing-cards-archives-and-native-instances.md` decision 7).

### 1.4 The usdview plugin

The file is `lib/python/usdgen/plugInfo.json` (`10-build-dependencies-testing.md` §4.4), and both the
type name and `Name` are **package-qualified**: the Python package is `usdgen` (all lowercase,
ADR §9 R5) and Plug loads a python plugin by executing `import <Name>`
(`pxr/base/plug/plugin.cpp:218-222`), so an unqualified `usdGenUsdview` would import a module that
does not exist. `08-tools.md` §1.1 gives the registered type as
`usdgen.usdGenUsdview.UsdGenUsdviewContainer`; `lib/python` must be on `PYTHONPATH` and
`lib/python/usdgen` on `PXR_PLUGINPATH_NAME` (`10-build-dependencies-testing.md` §3.6).

```json
{
    "Plugins": [
        {
            "Info": {
                "Types": {
                    "usdgen.usdGenUsdview.UsdGenUsdviewContainer": {
                        "bases": ["pxr.Usdviewq.plugin.PluginContainer"],
                        "displayName": "usdGen grooming"
                    }
                }
            },
            "LibraryPath": "",
            "Name": "usdgen.usdGenUsdview",
            "ResourcePath": ".",
            "Root": ".",
            "Type": "python"
        }
    ]
}
```

Its behaviour — state in a dataclass built in `__init__`, lazy Qt imports, the signal's frame not the
property, `UpdateViewport()` after every edit — is `08-tools.md` §1.2–1.3. Its only imaging
obligations are to call `UsdGenImaging_Activate` and `SetTime`, to call `Commit` **only** after a
live-override change (§3.8, §3.9 trigger (a)), and to set `_allowAsync` during `registerPlugins` for
progressive generation (S20; `research/G-evaluation-scheduling-and-batching.md` §8). It never calls
`Commit()` for a stage edit and it registers **no** `Usd.Notice` listener: a stage edit commits on
trigger (c) inside `_PrimsDirtied`, with or without the plugin attached (ADR §9 R32; §3.9).

### 1.5 What is deliberately not registered

A second `UsdImagingSceneIndexPlugin` on the **value** path (rejected: pointer-ordered, observed
upstream of UsdSkel, flips with `PXR_PLUGINPATH_NAME` — `research/G-chain-order-probe.md` §2,
MEASURED). An hdGp generative procedural (rejected: gated off by default, uncached `GetChildPrim`,
non-chainable, downstream of Storm's velocity motion — S6). A usdRig registry hook (rejected: usdGen
must not require a usdRig change — S1). A `HdSiExtComputationPrimvarPruningSceneIndex` spliced into
the shared chain (rejected: S3 makes it private; see §3.5).

---

## 2. The UsdImaging adapters

Without these, half the schema does not exist as far as Hydra is concerned (F2). This is the single
fact that makes the adapter mandatory rather than an optimisation.

### 2.1 Classes

```cpp
// usdGenImaging/primAdapter.h
class UsdGenPrimAdapterBase : public UsdImagingSceneIndexPrimAdapter
{
public:
    TfTokenVector GetImagingSubprims(const UsdPrim &) override;      // { TfToken() }
    TfToken       GetImagingSubprimType(const UsdPrim &,
                                        const TfToken &subprim) override;
    HdContainerDataSourceHandle GetImagingSubprimData(
        const UsdPrim &, const TfToken &subprim,
        const UsdImagingDataSourceStageGlobals &) override;
    HdDataSourceLocatorSet InvalidateImagingSubprim(
        const UsdPrim &, const TfToken &subprim,
        const TfTokenVector &properties,
        UsdImagingPropertyInvalidationType) override;

protected:
    /// Built once per schema type name, from UsdPrimDefinition. Never from a
    /// hand-written list.
    static const UsdImagingDataSourceMapped::PropertyMappings &
        _Mappings(const TfToken &schemaTypeName);
};

class UsdGenOperatorAdapter    final : public UsdGenPrimAdapterBase {};
class UsdGenMapAdapter         final : public UsdGenPrimAdapterBase {};
class UsdGenGroomAdapter       final : public UsdGenPrimAdapterBase {};
class UsdGenDescriptionAdapter final : public UsdGenPrimAdapterBase {};
class UsdGenGuideSetAdapter    final : public UsdGenPrimAdapterBase {};
```

`UsdImagingSceneIndexPrimAdapter` (`pxr/usdImaging/usdImaging/sceneIndexPrimAdapter.h:27`) is the
right base for a Hydra-2.0-only plugin: it makes the four scene-index virtuals the whole interface
and supplies `final` no-ops for the Hydra 1.0 pure virtuals. Each subclass gets its own
`TF_REGISTRY_FUNCTION(TfType)` with
`SetFactory<UsdImagingPrimAdapterFactory<UsdGenOperatorAdapter>>()`.

`GetImagingSubprims` returns `{TfToken()}` — one Hydra prim per USD prim, no subprims. usdGen does
**not** use an adapter subprim for its instancers: a property path cannot own children
(`pxr/usd/sdf/path.cpp:841`), so prototype hiding by prefix would not apply
(`research/G-instancing-cards-archives-and-native-instances.md` key facts).

`GetImagingSubprimType` returns `TfToken()` — the empty token — for **all five** registered types.
There is nothing to defer to: `UsdImagingSceneIndexPrimAdapter` declares all four scene-index
virtuals pure (`pxr/usdImaging/usdImaging/sceneIndexPrimAdapter.h:42-56`; the only inherited bodies
are the `final` Hydra-1.0 no-ops at `:81-112`), so "the base class's type" would name no token.
Operators and maps are data-only prims and must not become rprims. `UsdGenGroom`,
`UsdGenDescription` and `UsdGenGuideSet` derive from `UsdGeomImageable` but are not gprims: they
carry `visibility`, `purpose`, `primvars`, `primOrigin` and `__usdPrimInfo` through
`UsdImagingDataSourcePrim` (`pxr/usdImaging/usdImaging/dataSourcePrim.cpp:701-731`). `xform` and
`extent` are served **only** for `UsdGenDescription`, which is a `UsdGeomBoundable` and therefore
also a `UsdGeomXformable` (`dataSourcePrim.cpp:718-727` gates the `xform` container on
`IsA<UsdGeomXformable>` and `extent` on `IsA<UsdGeomBoundable>`; `pxr/usd/usdGeom/xformable.h:241`,
`pxr/usd/usdGeom/boundable.h:65`, `pxr/usd/usdGeom/imageable.h:57`). `UsdGenGroom` and
`UsdGenGuideSet` are Imageable but **not** Xformable, so they get no `xform` container at all — a
groom is a scope, and its tiles carry the surface's matrix (§4.1). None of this needs a Hydra prim
type. A typed usdGen prim was MEASURED arriving with `primType == ""`
(`research/G-stage-free-parameter-and-time-transport.md` §1, probe2; §2.7 below).
`<Description>/__usdGenRender/*` is the only place usdGen produces renderable prim types, and those
prims are synthesized by the scene index (§4), never by an adapter.

`GetImagingSubprimData` returns an overlay of `UsdImagingDataSourcePrim::New(path, prim, globals)` —
which brings `visibility`, `purpose`, `primvars`, `primOrigin`, `__usdPrimInfo` and, for Xformables
and Boundables respectively, `xform` and `extent` for free
(`pxr/usdImaging/usdImaging/dataSourcePrim.cpp:701-731`) — with a single `usdGen` container built by
`UsdImagingDataSourceMapped`.

### 2.2 Mappings from `UsdPrimDefinition`

```cpp
const UsdImagingDataSourceMapped::PropertyMappings &
UsdGenPrimAdapterBase::_Mappings(const TfToken &schemaTypeName)
{
    static TfStaticData<std::unordered_map<
        TfToken,
        std::unique_ptr<UsdImagingDataSourceMapped::PropertyMappings>,
        TfHash>> cache;
    static std::mutex mutex;

    std::lock_guard<std::mutex> lock(mutex);
    auto &slot = (*cache)[schemaTypeName];
    if (slot) { return *slot; }

    std::vector<UsdImagingDataSourceMapped::PropertyMapping> mappings;
    if (const UsdPrimDefinition *def =                     // null: not a CONCRETE type
            UsdSchemaRegistry::GetInstance().FindConcretePrimDefinition(schemaTypeName)) {
        for (const TfToken &name : def->GetPropertyNames()) {      // primDefinition.h:38
            if (_IsPrimBuiltin(name)) continue;   // served by UsdImagingDataSourcePrim
            const HdDataSourceLocator loc = _LocatorFor(name);     // RELATIVE locator:
                                                                   // usdGen:clump:size -> clump/size
                                                                   // usdGen:frozen:mode ->
                                                                   //   frozen/mode
            switch (def->GetSpecType(name)) {                      // primDefinition.h:290
            case SdfSpecTypeAttribute:
                mappings.push_back(_AttributeMapping(def, name, loc));
                break;
            case SdfSpecTypeRelationship:
                mappings.push_back(UsdImagingDataSourceMapped::RelationshipMapping{
                    {name, loc},
                    _IsSingleTarget(name)   // { usdGen:terminal, usdGen:frozen:curves }
                        ? UsdImagingDataSourceMapped::GetPathFromRelationshipDataSourceFactory()
                        : UsdImagingDataSourceMapped::GetPathArrayFromRelationshipDataSourceFactory()});
                break;
            default: break;
            }
        }
    }
    slot = std::make_unique<UsdImagingDataSourceMapped::PropertyMappings>(
               mappings, HdDataSourceLocator(_tokens->usdGen));   // prefix; makes loc absolute
    return *slot;
}
```

The `PropertyMappings` object is built **once per schema type name** and owned by the cache.
`_Mappings` is called from `GetImagingSubprimData` and from `InvalidateImagingSubprim` (§2.7) — once
per prim per pull — so returning a fresh heap allocation would both leak and defeat the cache.
`FindConcretePrimDefinition` returns `nullptr` for any name that is not a **concrete** registered
type (`pxr/usd/usd/schemaRegistry.h:508`) — an abstract base such as `UsdGenOperator` or `UsdGenMap`
(ADR §2.1), or a type whose plugin failed to load — so the null branch caches an empty mapping set
instead of dereferencing.

Four rules, each forced by evidence:

1. **Generic, never hand-written.** `UsdImagingDataSourceMapped::Get` posts a `TF_CODING_ERROR` for a
   property the prim does not have (`pxr/usdImaging/usdImaging/dataSourceMapped.cpp:166-212`,
   MEASURED at `research/G-stage-free-parameter-and-time-transport.md` §2). A superset list would
   spam coding errors on every operator type that lacks an attribute.
2. **`UsdPrimDefinition` is the source of truth**, and it works for codeless types with zero C++:
   `GetPropertyNames()` / `GetSpecType()` / `GetAttributeDefinition(name).GetTypeName()`
   (`pxr/usd/usd/primDefinition.h:38,290,277`; MEASURED, same report §1).
   `GetSchemaAttributeSpec()` at `:301-307` is marked **deprecated** in 26.08 ("Use
   GetAttributeDefinition instead") and returns a handle that can be null — do not use it. Because `UsdGenMaskAPI` is auto-applied to `UsdGenOperator` via
   plugInfo `AutoApplyAPISchemas` (ADR §2.1), its `usdGen:mask:*` properties are in that list on
   every operator without the author remembering to apply it. Gate SI-8 proves auto-apply works on a
   codeless type.
3. **The filter is a skip list, never a name-prefix test.** Every schema property usdGen declares is
   `usdGen:`-namespaced (ADR §9 R6; `02-schema.md` §2 is the normative registry, R7) — the freeze
   block is `usdGen:frozen:curves|mode|epoch|tier` and the sculpt block is
   `usdGen:sculpt:weight|space|curveIds|cvOffsets|deltas|epoch|lockedCurves|rootPrims|rootUVs`, not
   the un-namespaced shorthand ADR §2.3 wrote them with. A `TfStringStartsWith(name, "usdGen:")`
   test would therefore happen to work today, and that is exactly why it is banned: it would
   silently drop any property a later schema revision adds outside the namespace, and the failure
   mode is F2/S10 — invisible to Hydra with **no notice at all**, the class of defect
   `design/judge-evidence.md` §2.1 calls "the most consequential shared defect". So `_IsPrimBuiltin`
   skips only what `UsdImagingDataSourcePrim` already serves — `visibility`, `purpose`, `proxyPrim`
   (the `UsdGeomImageable` builtins), `xformOpOrder` and any `xformOp:*`, `extent`, and any
   `primvars:*` — and **every remaining name in `GetPropertyNames()` is mapped**, with no prefix
   filter (R6).
4. **`_LocatorFor` yields a relative locator; `PropertyMappings` makes it absolute.** `_LocatorFor`
   strips the leading `usdGen:` and splits the remainder on `:`, producing `clump/size`,
   `frozen/mode`, `sculpt/deltas`. The `PropertyMappings` constructor prepends the `usdGen` prefix —
   `const HdDataSourceLocator locator = dataSourcePrefix.Append(attrMapping->hdLocator);`,
   commented "Making locator absolute"
   (`pxr/usdImaging/usdImaging/dataSourceMapped.cpp:328-345`) — and stores
   `usdGen/clump/size`, `usdGen/frozen/mode`, `usdGen/sculpt/deltas`. Passing an already-absolute
   locator as the mapping's own `hdLocator` would land every property at `usdGen/usdGen/...` and
   nothing in the container would ever be found or invalidated. The stored absolute locator is
   exactly the key the dirty router hashes on (§3.3; the property→locator table is
   `02-schema.md` §6, and the router's compile-time table is generated from it, R25).

### 2.3 The `usdGen` container

One container per usdGen prim, published under the prim-level name `usdGen`. Measured shape from the
probe (`research/G-stage-free-parameter-and-time-transport.md` §2, probe1 §4): container names
`clumpRadius inputs map ramp surface`, nested `ramp/{knots,values}`, and
`Invalidate({usdGen:ramp:knots, usdGen:surface})` returning `usdGen/ramp/knots` and `usdGen/surface`.

| Property class (`02-schema.md` §3) | Data source | Locator | Time-varying? |
|---|---|---|---|
| scalar `float`/`int`/`bool`/`token`/`string` | `UsdImagingDataSourceAttribute<T>` | `usdGen/<a>/<b>` | only if `.timeSamples` or `.spline` authored |
| scalar with `.spline` (animated parameter) | same | same | **yes** — one dirty per `SetTime` (MEASURED) |
| ramp knots `float2[] <p>:knots` + `token <p>:interpolation` | typed DS | `usdGen/<p>/knots` | never |
| colour ramp `float[] <p>:positions` + `color3f[] <p>:colors` | typed DS | `usdGen/<p>/positions` | never |
| whole-spline ramp `float <p>:spline` | custom factory → `HdRetainedTypedSampledDataSource<TsSpline>` | `usdGen/<p>/spline` | **never** (must not flag) |
| `asset` | `UsdImagingDataSourceAssetPathAttribute` | `usdGen/<p>` | no; flags asset-path dependent |
| `asset[]` | typed DS | `usdGen/<p>` | no; **no** reload tracking |
| relationship, **exactly one** target — `usdGen:terminal` and `usdGen:frozen:curves` (`02-schema.md` §2.3, §2.9: "Exactly one target") | `GetPathFromRelationshipDataSourceFactory()` | `usdGen/terminal`, `usdGen/frozen/curves` | never |
| relationship, n targets, or "at most one" (`usdGen:input`, `usdGen:surface`, `usdGen:clump:centers`, `usdGen:mask:source`, `usdGen:mask:region`) | `GetPathArrayFromRelationshipDataSourceFactory()` → `VtArray<SdfPath>` of forwarded targets | `usdGen/input` | never |

`usdGen:input` is an **array** relationship, and that is not negotiable. S26 says "explicit
`usdGen:input` relationship(**s**)"; `UsdGenMap`'s `CombineMap` and the blend operators take several
(ADR §2.1); and the Merkle structural digest hashes `sorted(input paths)` (ADR §4.2.1), so a
truncated set would change the digest silently.
`GetPathFromRelationshipDataSourceFactory()` returns only the **first** forwarded target (MEASURED,
`research/G-stage-free-parameter-and-time-transport.md` §2), so using it here would lose every input
but one on every multi-input node. `_IsSingleTarget` is therefore a compile-time set containing
exactly `usdGen:terminal` and `usdGen:frozen:curves` — the two relationships `02-schema.md` declares
as "exactly one target". An "at most one" relationship such as `usdGen:mask:source` keeps the array
factory, because an empty array is how the evaluator sees "no map bound". The decision is per
**property name**, never per prim: `_Mappings` is
cached per schema type name (§2.2, §2.7) and `usdGen:input` is declared once on the abstract
`UsdGenOperator`, so one arity decision necessarily covers every operator.

`UsdAttribute::Get<SdfAssetPath>` resolves for free, and UsdImaging's asset-path data source adds
UDIM handling (`pxr/usdImaging/usdImaging/dataSourceAttribute.cpp:34-100`), so map loading downstream
needs no resolver context and no stage (S13). `FlagAsAssetPathDependent` is specialised for scalar
`SdfAssetPath` only (`dataSourceAttribute.h:203-212`), which is why the schema uses one `asset` per
map or a child prim per map, never `asset[]` (S13).

### 2.4 Ramps and `TsSpline` transport (S11, ADR §1)

A `.spline` on a `float` attribute is *only* a time-varying scalar to Hydra: `UsdStage` deems every
spline possibly time-varying without analysing it (`pxr/usd/usd/stage.cpp:9751-9758`),
`GetTimeSamplesInInterval` returns nothing, and `FlagAsTimeVarying` fires — so **every `SetTime`
dirties the parameter and forces a recook** (MEASURED, probe3 D/E). Therefore:

* A `.spline` on a *ramp* property is a **compile error** in `UsdGenGraphDesc` construction, reported
  as a hard diagnostic (§9).
* A `.spline` on a plain scalar parameter means "animated" and is honoured.
* The optional whole-spline ramp transport uses an `AttributeMapping::factory` that reads
  `UsdAttribute::GetSpline()` (`pxr/usd/usd/attribute.h:563`) and returns
  `HdRetainedTypedSampledDataSource<TsSpline>` **without** calling `FlagAsTimeVarying`. MEASURED end
  to end: the terminal scene index returns the `TsSpline` intact and evaluates it at an arbitrary
  parameter (probe6). `TsSpline` supports only `double/float/GfHalf/GfTimeCode`
  (`pxr/base/ts/types.h:32-37`), which is why a *colour* ramp is positions + colors and can never be
  a spline (ADR §1 S11).

The evaluator reinterprets the spline's parameter axis as normalized root-to-tip `u` and bakes it to
a 257-entry LUT at capture (`03-execution-engine.md` §8.3).

### 2.5 Time-varying and asset registration are lazy (S14)

`FlagAsTimeVarying` / `FlagAsAssetPathDependent` are called in the **data source constructor**
(`pxr/usdImaging/usdImaging/dataSourceAttribute.h:205-243`). Before a deep pull, no `SetTime` produces
any notice; after it, both prims dirty correctly (MEASURED, probe2 vs probe3). A graph that caches
values and stops pulling stops receiving time dirties for what it stopped pulling.

**Rule (closes the judges' shared S14 gap, `design/judge-evidence.md` §2.1 and §4.1):**
`UsdGenGraphDesc` construction pulls **every mapped locator of every node**, regardless of the node's
current mode. A `Clump` with `noise:amount == 0` still pulls `usdGen/clump/noise/frequency`. The pull
is one `Get` per locator per topology generation — at 200 operator prims × ~20 properties that is
4 000 `Get` calls, each a typed data source construction (UNMEASURED; gate SI-7 measures it and
asserts that every declared property of every registered type — all of them `usdGen:`-namespaced
(R6), `usdGen:frozen:*` and `usdGen:sculpt:*` included, plus `usdGen/rest/points` from the
API-schema adapter — both **appears** and **dirties**).
Nodes additionally declare `__dependencies` only for the surface→points edge (§4.1); parameter
invalidation comes from the adapter, not from dependency forwarding.

### 2.6 The `UsdGenRestAPI` adapter (S12)

```cpp
class UsdGenRestAPIAdapter final : public UsdImagingAPISchemaAdapter
{
public:
    HdContainerDataSourceHandle GetImagingSubprimData(
        const UsdPrim &, const TfToken &subprim, const TfToken &appliedInstanceName,
        const UsdImagingDataSourceStageGlobals &) override;
    HdDataSourceLocatorSet InvalidateImagingSubprim(
        const UsdPrim &, const TfToken &subprim, const TfToken &appliedInstanceName,
        const TfTokenVector &properties, UsdImagingPropertyInvalidationType) override;
};
```

It publishes `usdGen/rest/points` from an `AttributeMapping::factory` whose data source calls
`attr.Get<VtVec3fArray>(&r, UsdTimeCode::Default())`, and it **never** flags time-varying. MEASURED
(probe1 §5): at stage time 24 the deformed points read `[(0,0,5)…]` and the rest points read
`[(0,0,0)…]`, with only the deformed source appearing in the stage globals' time-varying list — so
the rest channel costs no per-frame dirty. An authored `primvars:rest` is honoured when present and
passes through the whole chain untouched (probe1 §1e, the Houdini convention). Capture-on-first-cook
is rejected: it is wrong whenever the first drawn frame is not the rest frame.

The same adapter serves rest **curve** points for frozen curves (S42,
`05-static-curves-and-deformation.md` §1.1 and §3.1):
the frozen-curve contract carries `primvars:rest` on the `BasisCurves` prim, and
`usdGen/rest/points` reads it at `Default()` when it is authored as a plain attribute.

It is registered by `apiSchemaName`, so it runs only on prims that actually apply `UsdGenRestAPI`
(`pxr/usdImaging/usdImaging/adapterRegistry.cpp:297-323`). **`UsdGenRestAPI` may be applied only to
the parent `Mesh`, never to a `GeomSubset`** (ADR §9 R15): the rest channel is a whole-mesh points
array and a subset carries only face indices into that mesh. Applying it to a `geomSubset` prim is
the sibling case of hard diagnostic 2 (§9) and is reported once per groom per compile;
`testUsdGenSurfaceResolver` covers it (§10.2).

### 2.7 Dirty locators the adapter emits

```cpp
HdDataSourceLocatorSet
UsdGenPrimAdapterBase::InvalidateImagingSubprim(
    const UsdPrim &prim, const TfToken &subprim,
    const TfTokenVector &properties, UsdImagingPropertyInvalidationType t)
{
    HdDataSourceLocatorSet result =
        UsdImagingDataSourceMapped::Invalidate(
            properties, _Mappings(prim.GetPrimTypeInfo().GetSchemaTypeName()));
    result.insert(UsdImagingDataSourcePrim::Invalidate(prim, subprim, properties, t));
    return result;
}
```

`UsdImagingDataSourceMapped::Invalidate` gives 1:1 nested-locator invalidation for free; the union
with `UsdImagingDataSourcePrim::Invalidate` keeps `visibility`, `purpose`, `extent` and `primvars:*`
working on the three Imageable types
(`pxr/usdImaging/usdImaging/dataSourcePrim.cpp:864-911`). That function is type-blind: it emits an
`xform` locator for any transform-affecting property name whatever the prim type
(`IsTransformationAffectedByAttrNamed`, `dataSourcePrim.cpp:880-885`), so on `UsdGenGroom` and
`UsdGenGuideSet` — Imageable but not Xformable (§2.1) — it can dirty a container that is never
published. That is harmless (Hydra ignores a dirty for an absent locator) and is the reason the
union is taken unconditionally rather than switched per type. The full property→locator table for every
schema type lives in `02-schema.md` §6; the router's compile-time table (§3.3) is generated from it,
so the two cannot drift.

Two announcement behaviours inherited from the stage scene index and relied on by §3:

* `DefinePrim("/…/Frizz", "UsdGenNoise")` arrives as `PrimsAdded` with `primType == ""`; retyping an
  existing prim arrives as a re-`PrimsAdded` at the same path (MEASURED, probe2). Both are structural
  and route to `UsdGenDirtyStructural`.
* A prim-level metadata change resyncs only when the field is a plugin field
  (`pxr/usdImaging/usdImaging/stageSceneIndex.cpp:552-566`) — which `apiSchemas` is, so applying
  `UsdGenMaskAPI` by hand is a resync.

### 2.8 The metadata plugin, and why it returns its input

The `usdGen` container the adapters publish survives the entire downstream chain — draw mode, native
instance aggregation and flattening, selection, render-settings flattening — and reaches the
propagated prototype copy as well (MEASURED, probe5 in
`research/G-stage-free-parameter-and-time-transport.md` §4). Nothing needs to be *appended* to the
UsdImaging chain to make that work, which is why `AppendSceneIndex` returns `inputScene` unchanged:
appending a real scene index there would put usdGen on the UsdImaging plugin slot, whose order is a
`std::set<TfType>` over private pointers and was MEASURED to flip with `PXR_PLUGINPATH_NAME`
(`research/G-chain-order-probe.md` §2) — the exact hazard S1 exists to avoid. The plugin exists only
for the two metadata hooks of §1.3 (aggregation key, proxy-path translation), and it carries no
state: `AppendSceneIndex` receives **no** input args at all
(`pxr/usdImaging/usdImaging/sceneIndexPlugin.h:54-56`), so it has no channel through which to
receive any.

---

## 3. `UsdGenGroomSceneIndex`

### 3.1 Class layout

```cpp
// usdGenImaging/groomSceneIndex.h
class UsdGenGroomSceneIndex final : public HdSingleInputFilteringSceneIndexBase
{
public:
    static UsdGenGroomSceneIndexRefPtr New(
        const HdSceneIndexBaseRefPtr &inputScene,
        const HdContainerDataSourceHandle &inputArgs,
        const std::string &renderInstanceId);

    HdSceneIndexPrim GetPrim(const SdfPath &) const override;        // atomic load only
    SdfPathVector    GetChildPrimPaths(const SdfPath &) const override;

    // Application entry points (reached from the C ABI, §3.8)
    void SetTime(double frame);
    void SetContext(UsdGenContext);            // interactive | render
    void Commit(UsdGenCommitReason);

protected:
    void _PrimsAdded  (const HdSceneIndexBase &,
                       const HdSceneIndexObserver::AddedPrimEntries &) override;
    void _PrimsRemoved(const HdSceneIndexBase &,
                       const HdSceneIndexObserver::RemovedPrimEntries &) override;
    void _PrimsDirtied(const HdSceneIndexBase &,
                       const HdSceneIndexObserver::DirtiedPrimEntries &) override;
    void _PrimsRenamed(const HdSceneIndexBase &,
                       const HdSceneIndexObserver::RenamedPrimEntries &) override;
    void _SystemMessage(const TfToken &messageType,
                        const HdDataSourceBaseHandle &args) override;

private:
    HdSceneIndexBaseRefPtr        _pruned;      // private ExtComputationPrimvarPruning (§3.5)
    UsdGenSessionHandle           _session;     // weak; re-attaches after a renderer switch
    UsdGenDirtyRouter             _router;
    UsdGenPublishedIndex          _published;   // path -> tile | guide | cvPoints | instancer
    UsdGenLiveOverrideStore       _liveOverrides;  // brush loop, §3.11
    UsdGenSurfaceResolver         _surfaces;       // usdGen:surface -> hydra path, §3.10
    std::atomic<bool>             _populated;   // §3.6
    std::string                   _renderInstanceId;
};
```

`HdSingleInputFilteringSceneIndexBase` declares `_PrimsAdded/_PrimsRemoved/_PrimsDirtied` pure and
`_PrimsRenamed` virtual with a base implementation that converts renames to removals
(`pxr/imaging/hd/filteringSceneIndex.h:127-148`).

### 3.2 Observer handling

All four handlers obey the same shape, which is S18's and usdRig's:

```
1. route the entries into _pending (UsdGenPendingDirty)   -- table lookups, never a cook (I7, S17)
2. forward the input entries downstream UNCHANGED, immediately
3. if a commit trigger fired (§3.9), commit and emit our own notices after the forward
```

* `_PrimsAdded` — routes `UsdGenGroom` roots into the session's known-roots set, marks
  `_populated = true`, and routes a re-add of an existing surface path as a full resync of that
  surface (the adapter delegate treats a re-`PrimsAdded` as all-dirty,
  `pxr/imaging/hd/sceneIndexAdapterSceneDelegate.cpp:244-264`;
  `research/A2-usdrig-imaging.md` §9).
* `_PrimsRemoved` — drops bookkeeping by prefix and marks the affected sessions structural. It must
  tolerate `-REM /` followed by an immediate full re-add: `SetStage(nullptr)` sends the removal while
  the stage is still live and downstream indices re-pull during the notice
  (`pxr/usdImaging/usdImaging/stageSceneIndex.cpp:388-396`, MEASURED probe2 L). **Caches are never
  freed inside a notice** (S15); the session is marked detached and reclaimed on the next commit.
* `_PrimsDirtied` — the router entry point (§3.3), plus the two commit triggers (b) and (c).
* `_PrimsRenamed` — forwarded to the base implementation, which converts to removed + added; usdGen
  treats the result exactly as a resync. Hydra has no `PrimsChildrenReordered` notice, which is one
  reason implicit sibling wiring was rejected (ADR §2.1).

`_SystemMessage` handles `asyncAllow` and `asyncPoll` (`pxr/imaging/hd/systemMessages.h:17-30`).
`SystemMessage` walks inputs first, then self (`pxr/imaging/hd/sceneIndex.cpp:181-195`), so the
message reaches usdGen wherever it sits. Notices may only be *sent* during the poll, on the app
thread; background work only stages results (`systemMessages.h:22-27`). Progressive generation is M8
(`11-roadmap.md`).

### 3.3 The dirty router entry point

```cpp
void
UsdGenGroomSceneIndex::_PrimsDirtied(
    const HdSceneIndexBase &sender,
    const HdSceneIndexObserver::DirtiedPrimEntries &entries)
{
    UsdGenPendingDirty pending;
    _router.Route(entries, &pending);            // O(entries), one hash lookup each; NEVER cooks
    const bool frameDirty   = _RouteSceneGlobals(entries, &pending);
    const bool surfaceDirty = pending.HasSurfacePoints();   // read BEFORE the move
    const bool syncCommit   = pending.NeedsSyncCommit();

    _SendPrimsDirtied(entries);                  // forward unchanged, first

    if (!_session) return;         // no groom root seen yet, or detached (§3.2)
    _session->AccumulateDirty(std::move(pending));

    // End of the batch that carried the dirty. Trigger (c) ALWAYS applies, with or
    // without an app driver (ADR §9 R32); trigger (b) only when no (a) is attached.
    if (frameDirty && !_session->HasAppDriver()) {
        Commit(UsdGenCommitReason::Frame);       // trigger (b)
        if (surfaceDirty) _session->MarkDirty(); // batched inversion, §3.9
    } else if (syncCommit) {
        Commit(UsdGenCommitReason::Edit);        // trigger (c)
    }
}
```

The commit is taken **at the end of the `_PrimsDirtied` batch that carried the dirty**, after the
entries have been forwarded — one cook per edit batch, which is what gate SI-3 counts (ADR §9 R32).
There is no "flag only" path for an operator, map or surface-topology dirty: an application that
drives time through the C ABI still gets its stage edits committed here, because a stage edit
produces no `SetTime` and no `currentFrame` notice at all (MEASURED, scenario S6 in
`research/G-evaluation-scheduling-and-batching.md` §4).

Routing cost is MEASURED and small: `HdDataSourceLocatorSet::Intersects` at 0.013 µs, ~0.2 µs per
notice entry end to end, 1.13 ms/frame at 5 000 dirty scalps
(`research/G-storm-throughput-and-prim-granularity.md` §1.8;
`research/G-evaluation-scheduling-and-batching.md` §9, both MEASURED). The router's structure — a
compile-time `(primPath, locatorPrefix) → (node, UsdGenDirtyBits, surfaceId)` table with no search —
is `03-execution-engine.md` §5.1.

Three routing cases are imaging-specific and belong here:

| Incoming locator on a bound surface | Bits raised | Why |
|---|---|---|
| `primvars/points` or `primvars/points/primvarValue` | `UsdGenDirtySurfacePoints` | the ordinary deform path |
| `extComputationPrimvars` (any leaf) or `extComputation/inputValues/*` on `<mesh>/skinningPoints*Computation` | `UsdGenDirtySurfacePoints` | under **Storm**, UsdSkel blocks `primvars/points` and publishes computed primvars; the private wrapper rewrites values but usdGen still receives the raw locator (`research/A2-usdrig-imaging.md` §9; `research/G-chain-order-probe.md` §4a, MEASURED) |
| `HdDataSourceLocatorSet::UniversalSet()` or a re-`PrimsAdded` | `UsdGenDirtySurfaceTopo` | resync: re-read type, topology, points (`research/A2-usdrig-imaging.md` §9) |

### 3.4 `GetPrim` and `GetChildPrimPaths`

```cpp
HdSceneIndexPrim
UsdGenGroomSceneIndex::GetPrim(const SdfPath &primPath) const
{
    if (_session) {
        if (const std::shared_ptr<const UsdGenGeneration> gen =
                _session->Generation()) {                       // std::atomic_load, lock-free
            if (const UsdGenPublishedPrim *p = _published.Find(gen, primPath)) {
                return p->Build();      // retained data sources over the generation's arrays
            }
        }
    }
    return _GetInputSceneIndex()->GetPrim(primPath);
}
```

`GetPrim` does exactly one thing beyond the lookup: an atomic load of the published generation. It
never cooks, never locks, never touches the stage, never blocks. That is the settled rule (S17, S19,
invariant I7) and it is what the tearing test proves safe (F5).

The generation is a `std::shared_ptr<const UsdGenGeneration>`, never a `TfRefPtr`: ADR §9 R20 fixes
the handoff on the idiom the zero-torn-reads probe measured, and a `TfRefPtr` has no atomic load
(`03-execution-engine.md` §6.1–6.2). `Generation()` is the **free** `std::atomic_load(&_current)`
and the commit thread publishes with `std::atomic_store(&_current, next)` — the `shared_ptr`
overloads R20 names, the form usdRig's `RigExecSnapshotStore` ships, and the only form available
under the project's `set(CMAKE_CXX_STANDARD 17)` (`10-build-dependencies-testing.md` §3.1). The
C++20 caveat is recorded once, in `03-execution-engine.md` §6.1: those free overloads are deprecated
there, so a later C++20 move swaps the member for `std::atomic<UsdGenGenerationConstPtr>` and
changes nothing else about the contract (`research/G-evaluation-scheduling-and-batching.md` §5,
caveat).

The withdrawn backstop matters here. S18(c) originally allowed a cook on the first `GetPrim` of a
generated prim; ADR §1 withdraws it, because a reader thread cannot emit notices and a cook that
emits none is useless — Storm only re-pulls what was dirtied
(`design/judge-delivery.md` §5.1). Trigger (c) in §3.9 replaces it.

`GetChildPrimPaths` replaces a `UsdGenDescription`'s child list with `__usdGenRender` (§3.4.1) and
serves the generation's prim set below it. Two rules copied from usdRig's synthesized-guide code
(`research/A2-usdrig-imaging.md` §5): **authored prims win** — a synthesized name is served only where
the input has no prim at that path — and **announcement history is kept even while unobserved**, so a
late observer receives correct removals.

#### 3.4.1 What the index hides

`GetChildPrimPaths(<Description>)` returns `__usdGenRender` and nothing else: `Ops/`, `Guides/`,
`Maps/`, `Prototypes/` and `Frozen/` are **pruned from the render** (ADR §2.2;
`05-static-curves-and-deformation.md` §5.7). Pruning is cheap and invisible because those prims are
inside a scope usdGen owns.

A C3 source prim that lives **outside** the groom — an imported curve set, a freeze written to a
sidecar, a simulation cache — cannot be pruned that way: it would vanish from the outliner and from
every other consumer of the stage. The index therefore **overlays `visibility/visibility = false`**
on it (`pxr/imaging/hd/visibilitySchema.h:35-39`), one reversible locator that shows in the property
panel, so the curves never draw twice while the description publishes tiles for them (ADR §9 R29).

Every overlay the index makes on an upstream prim — this `visibility` overlay, the CV-display child
of §4.2, any primvar the index adds — obeys S5: it emits
`HdContainerDataSourceEditor::ComputeDirtyLocators(...)` **and** the bare `primvars` locator (§5.1,
R29). Forgetting the bare locator freezes the resolved upstream prim, which is a silent
wrong-geometry bug.

### 3.5 The private pruning wrapper (S3)

```cpp
_pruned = HdSiExtComputationPrimvarPruningSceneIndex::New(_GetInputSceneIndex());
```

Constructed once in the constructor, held only by usdGen, **never spliced into the shared chain**.
Everything the surface reader pulls goes through `_pruned`; everything usdGen forwards downstream
comes from `_GetInputSceneIndex()` unchanged.

Why it is mandatory rather than optional:

* Under **Storm**, a UsdSkel-skinned scalp's `primvars/points` and `primvars/normals` are **blocked**
  (`Get` returns nullptr); the points exist only as `extComputationPrimvars:points`, and Storm has no
  pruning plugin of its own (grep of `hdSt/`, `hdx/`, `usdImagingGL/` is empty)
  (`research/G-chain-order-probe.md` §4a; `research/G-hdprman-and-usdrecord-render-time-chain.md` §5,
  both MEASURED/source-verified). The wrapper restores real animated values headlessly
  (MEASURED, same report §4b).
* Under **hdPrman** the wrapper is mandatory too, and unconditionally so.
  `HdPrman_ExtComputationPrimvarPruningSceneIndexPlugin` is a **renderer-specific** phase-0 entry
  (`third_party/renderman/plugin/hdPrman/plugInfo.json:168-180`) while usdGen is an
  **all-renderers** phase-0 entry (§1.2), and all-renderers entries sort first within a phase:
  `adjustSpecificRendererEntry` renames the renderer-specific `phaseN` tag to `phaseN_` and adds
  `after: phaseN` (`pxr/imaging/hd/sceneIndexPluginRegistry.cpp:966-1005`; ADR §9 R34). usdGen
  therefore runs **before** hdPrman's pruning and sees the same blocked `primvars/points` it sees
  under Storm (`research/G-hdprman-and-usdrecord-render-time-chain.md` §2.2, §5). Double wrapping is
  harmless: the class is a pass-through when nothing is computed
  (`pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.cpp:695-730`), so hdPrman's own wrapper
  becomes the no-op. Owning the wrapper privately is what lets ADR §5.1 keep phase 0.

The wrapper has **no cache**: each pull re-runs the CPU skinning callback
(`extComputationPrimvarPruningSceneIndex.cpp:243-…`). `UsdGenSurfaceReader` — the per-commit cache
behind `UsdGenSurfaceResolver` (§3.10) — therefore caches per `(surfacePath, surfaceGeneration)` and
pulls once per commit, never once per tile. That cost is what gate **SI-9** measures on a
production-density skinned scalp (T1, M2; ADR §9 R40, §10.1): the assertion is one pull per
`(surfacePath, surfaceGeneration)` and a recorded cost inside the S-2 deform budget.

### 3.6 Initial population (ADR §4.4, gate SI-6)

Two paths, and both must populate identically.

1. **Notice path (usdview, usdrecord, hdPrman through the engine).** The renderer-level index is
   constructed before the UsdImaging chain is inserted into the engine's merging index, and
   `HdMergingSceneIndex::InsertInputScenes` walks the new input and sends `PrimsAdded` for every prim
   (`pxr/imaging/hd/mergingSceneIndex.cpp:215-244`). So the groom roots arrive as notices;
   `_populated` becomes true on the first `_PrimsAdded`. The replay is **conditional**:
   `InsertInputScenes` returns early when the merging index has no observer yet
   (`pxr/imaging/hd/mergingSceneIndex.cpp:211-213`, immediately before that gathering). In
   `UsdImagingGLEngine` the precondition always holds — `_CreateSceneIndicesAndRenderer` creates the
   renderer, and with it the render index's observer, at
   `pxr/usdImaging/usdImagingGL/engine.cpp:1528-1529` → `:1790-1793`, **before** the insert at
   `:1544`. A host that inserts its inputs before creating a renderer gets no replay and is served by
   path 2 instead; gate SI-6 covers both.
2. **Pre-populated path (a host that hands us an already-populated input; the legacy
   `HdRenderIndex::New` route).** No `PrimsAdded` replay arrives. On the first
   `GetChildPrimPaths`/`GetPrim` with `_populated == false`, usdGen performs **one bounded traversal**
   of the input, scanning only for prims whose type is `UsdGenGroom`, then sets `_populated`. The
   traversal reads `primType` only, never a data source, and stops descending below a groom root.
   It must complete in **≤ 5 ms on the 11 005-prim stage** (ASSUMPTION until SI-6 measures it,
   ADR §9 R28). Gate SI-6 records the number and asserts both paths yield the same prim set.

The traversal is deliberately *not* the general mechanism. A renderer-level filtering scene index
that traverses on construction pays it on every renderer switch, and the notice path is the one every
in-tree application takes.

### 3.7 Sessions and the registry (S15, ADR §4.5)

```cpp
class UsdGenImagingRegistry
{
public:
    static UsdGenImagingRegistry &GetInstance();             // TfSingleton

    UsdGenSessionHandle Attach(const UsdGenSessionKey &);    // (weak stage | sessionId, groom root)
    void                Detach(const UsdGenSessionKey &);
    UsdGenSessionHandle Find  (const UsdGenSessionKey &) const;

    void     SetTime(double frame);                          // -> Commit on every session
    void     SetContext(UsdGenContext);
    void     Commit(UsdGenCommitReason);
    int64_t  Generation() const noexcept;
    void     ReloadMaps();                                   // S13: bumps the texture counter
private:
    mutable std::mutex _mutex;                               // released before any re-entrant call
    std::unordered_map<UsdGenSessionKey, std::weak_ptr<UsdGenSession>> _sessions;
};
```

The registry is process-global because nothing the engine owns survives a renderer switch:
`UsdImagingGLEngine::SetRendererPlugin` calls `_DestroyHydraObjects()`, which nulls the whole chain
including `_usdImagingSceneIndex`, and rebuilds it (`pxr/usdImaging/usdImagingGL/engine.cpp:403-451`,
`:1449-1546`). Scene indices hold weak handles and re-attach on construction (S15).

**Several scene indices, one session** (ADR §4.5). usdview switching renderers, or a process running
Storm and an hdPrman preflight, produces two `UsdGenGroomSceneIndex` instances over one session. The
session owns one current frame, one context, one generation counter; each attached index republishes
the same generation. The **prim set is a session property and never differs per index** — required by
S27/S28, and the reason `usdGen:renderDensityScale` decimates by stable id rather than changing the
tile count (ADR §2.3). A renderer switch re-attaches and continues the generation counter; it does
not reset the prim set.

Session keys, in the priority ADR §4.5 fixes — "(weak stage | `usdGen:sessionId`, groom root)":

1. The weak `UsdStage*` handed in by `UsdGenImaging_Activate(stageCacheId, …)` through
   `UsdUtilsStageCache::Get().Find(...)` — the usdRig interactive pattern
   (`usdRig/libs/rigExecImaging/registry.cpp:1160-1195`). Identity is the stage **object**, never its
   root layer: two stages commonly share a root layer and differ only by session layer
   (`usdRig/libs/rigExecImaging/registry.cpp:1070-1083`).
2. `usdGen:sessionId` (a `uniform string` on `UsdGenGroom`, ADR §2.3) — the **fallback**, used only
   when no stage is known. That is what ADR §2.3 means by "stage-free registry key when the C API is
   absent": the batch route, `usdrecord` and hdPrman, where the scene index has no stage at all.

The stage wins whenever both are available, and that order is the point. Two stages that reference
the same asset layer — the same groom opened twice, or a shot stage beside an asset-check stage —
carry the *same* authored `usdGen:sessionId`. Keying on the id first would collapse them into one
session with one current frame, one context and one generation: the same class of bug §8 names for
the extent callback, where "an ungated lookup answers a query about stage A frame 12 with stage B's
frame 30 pose".

If neither key is present, the index keys on the groom root path alone within the `renderInstanceId`,
and logs one `USDGEN_COMMIT` debug line saying so (§9; `USDGEN_SESSION` was retired — the six shipped
codes are declared in `10-build-dependencies-testing.md` §7.2, ADR §9 R35).

### 3.8 The C ABI

**This listing is reproduced verbatim from `08-tools.md` §1.4, which is the single source for
contract C4** (ADR §9 R31). The two were compared entry point by entry point and are **identical** in
every name, signature, return type and order; the only textual difference is the section pointers
inside the comments, which name each document's own sections. `08-tools.md` §1.4 governs; nothing
here is an independent statement of the ABI, so neither listing is edited without the other.

```c
/* usdGenImaging/cApi.h — USDGEN_IMAGING_C_API. Control and scalars only; no array
   ever crosses per element. Every entry point is extern "C", returns int status
   (0 = success, non-zero = error) except the two const char * accessors, and
   never throws across the boundary (ADR §9.4 R31).
   UsdGenImaging_GetLastError() returns the message for the calling thread. */

int         UsdGenImaging_Activate(long long stageCacheId, const char *groomRootPath,
                                   double frame);
int         UsdGenImaging_Deactivate(void);
int         UsdGenImaging_SetTime(double frame);
int         UsdGenImaging_Commit(void);   /* R32 trigger (a): after SetTime and after  */
                                          /* live-override changes ONLY. Never for a   */
                                          /* stage edit — trigger (c) owns those (3.9) */
int         UsdGenImaging_SetContext(const char *context); /* "interactive" | "render" */
long long   UsdGenImaging_GetGeneration(void);             /* publication handshake    */
long long   UsdGenImaging_GetTopologyGeneration(const char *descriptionPath);
                     /* bumps ONLY on a topology change (R31): a recapture, a new      */
                     /* curve set, a density or ceiling change. Never on a deform      */
                     /* frame and never on a live-override republish                   */

int         UsdGenImaging_BeginLiveOverride(const char *primPath);
int         UsdGenImaging_SetLiveOverrideIndexed(const char *primPath, const int *cvIndices,
                                                 int count, const float *xyz);
int         UsdGenImaging_ClearLiveOverride(const char *primPath);

int         UsdGenImaging_PickCV(const char *primPath, const float viewProj[16],
                                 int w, int h, float x, float y, float radiusPx,
                                 int *outCurve, int *outCv, float *outDistPx);
int         UsdGenImaging_Footprint(const char *primPath, const float viewProj[16],
                                    int w, int h, float x, float y, float radiusPx,
                                    int *outIdx, int maxOut, int *outCount);
int         UsdGenImaging_ClosestSurfacePoint(const char *surfacePath, const float p[3],
                                              int *outFace, float outUV[2], float outP[3]);
int         UsdGenImaging_BuildMirrorMap(const char *descriptionPath, int axis);

int         UsdGenImaging_SetInteractiveLOD(const char *descriptionPath, int maxCurves);
int         UsdGenImaging_SetMaskVisualisation(const char *opPath);  /* "" clears */
int         UsdGenImaging_ReloadMaps(void);
const char *UsdGenImaging_GetStatsJson(void);
const char *UsdGenImaging_GetLastError(void);
```

`SetInteractiveLOD(descPath, int maxCurves)` **supersedes** the earlier
`SetLodDensity(descPath, float density)`: the interactive ceiling is a curve count and is
session-only — there is no authored property (ADR §9 R8, R36) — and the engine converts it to a
`keepFraction` and applies the R13 hash rule, so ids, sculpt deltas and clump ids survive it (§6.4).
`GetTopologyGeneration`, `BuildMirrorMap`, `SetLiveOverrideIndexed` and `SetMaskVisualisation` are
R31 additions; `Footprint` is R31's name for the CV footprint query.

Every entry point is an accelerator. Disabling the C API degrades interactivity, never correctness:
`usdrecord` and hdPrman are driven entirely by authored data sources (S8, S39). Arrays never cross
here — they cross through the `_usdGen` pxr_boost module in O(1), 0.13–0.18 µs at 1 M CVs (MEASURED,
`research/G-tool-loop-array-transport-and-cv-picking.md` §1.2). CV picking is CPU in C++: **166 µs at
100 k CVs, 1.66 ms at 1 M** (MEASURED, same report §2.4), ≈8× cheaper than a Hydra pick, and
`UsdImagingGLEngine` exposes no point index anyway (`engine.cpp:1286-1288`, verified).

### 3.9 The three commit triggers, threads and notices (ADR §4.3 as ADR §9 R32 amends it)

| Trigger | When | Thread |
|---|---|---|
| (a) `UsdGenImaging_SetTime()` / `UsdGenImaging_Commit()` | the usdview plugin, from `currentFrameChanged` (**the signal's frame**, not the property); and an explicit `Commit()` after a live-override change. **Never for a stage edit.** Batch drivers wrapping `usdrecord` use the same entry points | app thread |
| (b) `/` + `sceneGlobals/currentFrame` dirty | stock hosts: usdview without the plugin, `usdrecord`, hdPrman — **only when no (a) is attached** | notice thread |
| (c) operator-parameter / map / surface-topology dirty | **always applies**, with or without an app driver: commits synchronously at the end of the `_PrimsDirtied` batch that carried it, so an edit batch costs exactly one cook (SI-3) | notice thread |

Trigger (c) is unconditional (ADR §9 R32). The app therefore never calls `Commit()` for a stage
edit, and a tool's release sequence is: close the live override without republishing → author the
edit in one `Sdf.ChangeBlock` → request a repaint → Hydra's `ApplyPendingUpdates` delivers the
dirty → the index cooks and publishes (`08-tools.md` §2.4). No `Usd.Notice` listener ordering rule
exists anywhere in this design.

Trigger (b) is sound because the frame
notice is **uniquely identifiable** (prim path `/`, locator `sceneGlobals/currentFrame`,
`HdSceneGlobalsSchema::GetDefaultPrimPath()` at `pxr/imaging/hd/sceneGlobalsSchema.h:105`) and is the
last notice of every un-batched frame (F4); the batched case is handled immediately below. It can
never be coalesced with the geometry notices because `HdsiSceneGlobalsSceneIndex` sits downstream of
the post-merging batching index.

**The batched-inversion case.** If a host ever wraps the post-merging index in
`MergingSceneIndexNoticeBatchBegin/End`, the `currentFrame` dirty arrives **before** the geometry
dirties, and a commit on trigger (b) alone would cook the previous frame's surface
(`research/G-evaluation-scheduling-and-batching.md` §6, M3a). usdGen does not read `GetPrim` to
recover: the read-through-consistency backstop is withdrawn (ADR §1 S18c; §3.4). Instead trigger (b)
commits **and leaves the session dirty whenever a surface-points dirty was accumulated in the same
notice pass after the frame dirty** — the `surfaceDirty` branch in §3.3. The batch flush then
satisfies trigger (c) and re-commits with the correct geometry. One extra cook per batched frame is
the deliberate price of keeping the commit logic independent of notice order, which §5.4 requires.

Committing at the end of a `_PrimsDirtied` batch is correct on both counts that disqualified the
alternatives: it runs on the notice thread (one thread, not four worker threads as a `GetPrim` cook
does) and it can emit notices. The new time is already visible inside the first notice handler —
`_stageGlobals._time` is assigned before `_SendPrimsDirtied`
(`pxr/usdImaging/usdImaging/stageSceneIndex.cpp:909-916`, MEASURED S10b) — so a synchronous cook
there reads correct post-`SetTime` data. Measured cook counts for 10 interactive events
(`research/G-evaluation-scheduling-and-batching.md` §5): eager cooking in every `_PrimsDirtied` =
**29** cooks (20 batched); cooking in `GetPrim` = 10 cooks but on **4 distinct worker threads** and
**0** downstream notices; end-of-batch commit = **10** cooks, one thread, 10 dirties. Gate **SI-3**'s
registry threshold is exactly that: 10 commits over the 10 events, every commit and notice on the app
or notice thread, and zero cooks inside `GetPrim` (`09-performance-and-benchmarks.md` §5.2).
`testUsdGenScheduling` additionally runs the same script wrapped in
`MergingSceneIndexNoticeBatchBegin/End` and asserts **identical published points** in both runs, with
at most one extra commit per batched frame from the inversion above; `09-…` §5.2's SI-3 row carries
that batched clause as well.

**Notice discipline.** All notices are emitted from the commit path on the thread that committed,
after the input entries have been forwarded, in the order `PrimsRemoved`, `PrimsAdded`, `PrimsDirtied`
(usdRig's proven order, `research/A2-usdrig-imaging.md` §3.4). Observer callbacks are not required to
be threadsafe (F5), so the commit thread does the emitting and the TBB workers never touch an
observer. A commit that is superseded mid-run returns the previous generation and **leaves the
session dirty**; it never half-publishes (ADR §4.2).

### 3.10 Locating the surface (`UsdGenSurfaceResolver`)

`usdGen:surface` is authored against the stage; the evaluator needs a path in the Hydra scene. One
class owns that translation (`design/proposal-performance.md` §6.4;
`design/proposal-risk.md` §5.4):

```cpp
// usdGenImaging/surfaceResolver.h
class UsdGenSurfaceResolver
{
public:
    struct Resolved {
        SdfPath         hydraPath;        // path in the input scene, post-propagation
        UsdGenSurfaceId id;               // stable id; part of the capture epoch
        bool            insidePrototype;
        VtIntArray      faceRestriction;  // empty unless the target was a GeomSubset
    };
    Resolved Resolve(const SdfPath &groomPrimPath,
                     const SdfPath &authoredTarget) const;
};
```

Four steps, in order:

1. If the groom prim has an ancestor whose `__usdPrimInfo.isNiPrototype` is true, take that
   ancestor's `niPrototypePath` together with the prim's `primOrigin/scenePath` (relative inside
   prototypes) and **rebase** the authored target onto the prototype root
   (`pxr/usdImaging/usdImaging/usdPrimInfoSchema.h:46-48`). Propagated prototype names are
   discovered, never constructed (S34, §4.3). The rebase arithmetic is an ASSUMPTION on top of
   MEASURED discovery; gate **T-INST-2** is what tests it.
2. Otherwise use the authored target verbatim. Instance-proxy targets have already been rewritten to
   prototype paths by the metadata plugin's `ProxyPathTranslationDataSourceNames()` (§1.3, §2.8), so
   the resolver never guesses.
3. Pull the resolved path **through the private wrapper** `_pruned` (§3.5) and read its prim type.
   `mesh` is the ordinary case: `primvars/points` is real post-wrapper, and `usdGen/rest/points`
   (§2.6) supplies the rest pose.
4. If the type is `geomSubset` — the type `UsdImagingGeomSubsetAdapter` publishes
   (`pxr/usdImaging/usdImaging/geomSubsetAdapter.cpp:125-133` returns `HdPrimTypeTokens->geomSubset`,
   `pxr/imaging/hd/tokens.h:279`) — take the **parent `mesh`** as the surface and the subset's
   `indices` as `faceRestriction`. ADR §2.3 is explicit: a subset restricts scatter to its faces, but
   `skinprim` and Ptex face ids stay indices into the **parent mesh's** faces, so nothing downstream
   re-indexes. A change to the subset's `indices` raises `UsdGenDirtySurfaceTopo` — a recapture, not
   a deform.

Any other resolved type is hard diagnostic 2 in §9, emitted once per groom per compile; usdGen then
passes the input through unchanged rather than publishing wrong hair.

### 3.11 Live overrides during a drag (S40)

The brush loop must not touch the stage per move. The C++ side is one class,
**`UsdGenLiveOverrideStore`** — the `_liveOverrides` member of §3.1 and the name
`01-architecture.md` §5.1 lists among `usdGenImaging`'s contents
(`design/proposal-risk.md` §5.2; `design/proposal-performance.md` §6.5 calls the same object
`UsdGenLiveOverride` and that spelling is not used here):

```cpp
// usdGenImaging/liveOverride.h
class UsdGenLiveOverrideStore
{
public:
    void Begin(UsdGenNodeId terminal);
    void SetFull(const VtVec3fArray &points);                              // press / release
    void SetIndexed(const VtIntArray &cvIndices, const VtVec3fArray &pts); // per move
    void Clear();
    TfSpan<const UsdGenChunkId> TouchedChunks() const;
};
```

A live override is a **virtual node appended after the terminal**. It owns no buffer. In v1 every
published `VtArray` is copy-on-write and a move pays one detach (ADR §1 S24;
`03-execution-engine.md` §6.2); patching CVs into a retired generation's buffer is an **M7**
optimisation, permitted only when the session store holds the sole strong reference and the array is
still the same allocation — `retired.use_count() == 1 && retired->tiles[i].points.IsIdentical(candidate)`
(`pxr/base/vt/array.h:945-950`). **`VtArray::IsUnique()` does not exist in OpenUSD 26.08 and appears
nowhere in this design** (ADR §9 R20, which supersedes ADR §1 S24's wording; the private `_IsUnique`
is at `pxr/base/vt/array.h:1023-1026`). A published buffer that must be recycled instead wraps a
`UsdGenGenerationBuffer : Vt_ArrayForeignDataSource` (`pxr/base/vt/array.h:39-51`) and becomes
reusable only after its detached callback has fired (`03-execution-engine.md` §6.2).
`TouchedChunks()` feeds the chunk→tile hop
(`03-execution-engine.md` §5.2), so one move dirties the one or two affected tiles with the bare
`primvars/points/primvarValue` leaf plus their `extent` and nothing else (§5.1). The store lives on
the scene index the tool drives (`_liveOverrides`, §3.1), not on the session, so a renderer switch
drops it and the artist restarts the stroke; while it is live, the eviction score never evicts a
live-override target (ADR §4.1). `UsdGenImaging_BeginLiveOverride` and
`UsdGenImaging_ClearLiveOverride` (§3.8) are the C entry points; `08-tools.md` §2.4 owns the Python
side and the single stage write at release.

---

## 4. Published prims

Everything below `<Groom>/<Description>/__usdGenRender/` is Hydra-only and never authored (ADR §2.2):

```
<Groom>/<Description>/__usdGenRender/
    tile_0000 … tile_NNNN              basisCurves
    guides/<setName>                   basisCurves, purpose = guide
    guides/<setName>/cvs               points
    inst_<opName>                      instancer
    inst_<opName>/Prototypes/<n>       re-rooted prototypes with instancedBy
    material_storm                     only when USDGEN_STORM_MATERIAL_OVERRIDE=1
```

The prim set is allocated once at compile and never changes during interaction (S27, S28). Adding or
removing prims bumps the rprim index version and forces an O(all rprims) dirty-list gather
(`pxr/imaging/hd/dirtyList.cpp:226-275`), and any element-count change reallocates the whole
aggregated VBO (`pxr/imaging/hdSt/vboMemoryManager.cpp:605-620`, MEASURED). The count is computed
from the *maximum* density the description can reach:
`chunksPerTile = max(1, ceil(nChunks / tileTarget))` and
`nTiles = min(nChunks, clamp(ceil(nChunks / chunksPerTile), 32, 256))` from
`uniform int usdGen:tileTarget = 64`, clamped to 32–256 (ADR §9 R21; `02-schema.md` §2.3). The
`min(nChunks, …)` term is what keeps a small groom from being asked for 32 tiles it cannot fill —
an empty `basisCurves` has no valid `extent` and is never published (`02-schema.md` §3.2).

A `usdGen:tileTarget` edit is the one authored property that changes the published prim set. It
routes as `UsdGenDirtyTopology` on the description: tile prims are added or removed with real
`PrimsRemoved`/`PrimsAdded`, and no digest changes (R21). It is not a per-frame or per-drag edit,
and the tool exposes it outside the interaction loop.

### 4.1 Tile contract C2 (frozen at end of M1)

Every `tile_NNNN` carries exactly this. Deviating from it is a contract break, not a bug fix.

| Data source (locator) | Type / value | Why |
|---|---|---|
| prim type | `basisCurves` | `pxr/imaging/hd/tokens.h:283` |
| `basisCurves/topology/curveVertexCounts` | `VtIntArray` | S29 |
| `basisCurves/topology/type` | `cubic` | S29 |
| `basisCurves/topology/basis` | the value of `uniform token usdGen:curve:basis` — `bspline` (default) or `catmullRom`; `centripetalCatmullRom` and `bezier` are rejected at compile with a named diagnostic, and hdPrman raises `TF_CODING_ERROR` and skips the prim for a basis it does not know (`hdPrman/basisCurves.cpp:98-110`) | S29, `02-schema.md` §2.3, §7 |
| `basisCurves/topology/wrap` | `pinned` | S29 |
| `primvars/points` | `VtVec3fArray`, `vertex`, role `point`; `points.size() == Σ curveVertexCounts` **asserted** | F3, S28 |
| `primvars/widths` | `VtFloatArray`, `vertex` or `constant`; **never `varying`** — varying costs a MEASURED 6.48 ms CPU expansion per 100 k curves (`research/G-storm-throughput-and-prim-granularity.md` key facts) | S29 |
| `primvars/hairT` | `VtFloatArray`, `vertex`, root 0 → tip 1 | S35; the glslfx declares it `float` (`prototypes/storm-hair-look/usdGenHairPreview.glslfx:180-184`) |
| `primvars/hairId` | `VtFloatArray`, **`uniform`**, `UsdGenHash32(curveId, 0) / 2^32` in [0,1), where `curveId` is the `uint64` of `primvars:usdGen:curveId` (ADR §9 R12) | ADR §1 S29 — the shipped glslfx declares `hairId` as `"type": "float"` (`prototypes/storm-hair-look/usdGenHairPreview.glslfx:190-194`); an int primvar does not bind and the shader silently reads 0.0 |
| `primvars/st` | `VtVec2fArray`, `uniform`, role `textureCoordinate`; root UV on the emitting surface | S29, S36 |
| `primvars/displayColor` | `VtVec3fArray`, role `color`; **`uniform`** when `usdGen:look:bakeMode = "perCurve"` (the default), **`vertex`** when `"perCV"` (`07-look-maps-expressions.md` §1.3); absent when `usdGen:look:bakeTarget = "none"` | S29; `uniform` is one `GfVec3f` per curve, so at the canonical 49-tile split (≈ 2 041 curves per tile at 100 k / chunk 512 / `tileTarget` 64, ADR §9 R21) the array is ≈ **24 KB** per tile against ≈ 196 KB of `points` on the same tile — **DERIVED** arithmetic, not a measurement. The measured half is the reason colour is `uniform` and lives in its own element-primvar BAR: `DirtyPrimvar` re-uploads every non-points primvar (`research/G-storm-throughput-and-prim-granularity.md` §1.5, MEASURED analysis; §5.1 below) |
| `primvars/<bakePrimvar>` | `VtVec3fArray`, role `color`, same interpolation rule as `displayColor`; **optional** — present only when `usdGen:look:bakeTarget = "primvar"` names something other than `displayColor` (`07-look-maps-expressions.md` §1.3–§1.4) | ADR §9 R8 |
| `primvars/clumpId_<level>` | `VtIntArray`, `uniform`, when a `UsdGenClump` ran | ADR §2.3 (emitted primvars) |
| `primvars/guideIndex`, `primvars/guideWeight` | `VtIntArray` / `VtFloatArray`, `uniform`, **`elementSize = 3`** (`HdPrimvarSchemaTokens->elementSize`, `pxr/imaging/hd/primvarSchema.h:42`), when `UsdGenGuideInterpolate` ran. `int[3]`/`float[3]` are not USD type names | ADR §2.3, ADR §9 R24 |
| `primvars/hairTangent` | **absent by default** (variant A); `VtVec3fArray`, `vertex`, object space when gate S-8 selects variant B | ADR §5.4 |
| `primvars/normals` | **never published** | S29 ("no `normals`"); a `normals` primvar makes `_SupportsUserNormals` true and forces Storm's RIBBON + **ORIENTED** shader key regardless of refineLevel (`pxr/imaging/hdSt/basisCurves.cpp:303-350`, `hdSt/basisCurvesShaderKey.h:41-52`; `research/A5-storm-curves-shading.md` key facts), which destroys ADR §5.4 variant A's premise that the tangent comes from Storm's own `inData.Neye` |
| `primvars/minScreenSpaceWidths` | `constant` `1.0` | S29; claimed as a Storm builtin so it survives primvar filtering (`pxr/imaging/hdSt/basisCurves.cpp:1371-1385`) |
| `primvars/velocities`, `primvars/accelerations` | `HdBlockDataSource` — blocked, except in motion profile P1 | S29, S32; blocking works because an overlay turns a block into null (`pxr/imaging/hd/overlayContainerDataSource.cpp:94-97`) |
| `extent/min`, `extent/max` | `GfVec3d`, recomputed **every deforming frame** | without an authored extent the bbox is `[FLT_MAX,-FLT_MAX]` and the prim is never culled (`pxr/imaging/hdSt/primUtils.cpp:887-891`, MEASURED) |
| `xform/matrix` + `xform/resetXformStack = true` | the surface's world matrix; points are surface-local | S4 |
| `displayStyle/refineLevel` | `2` | S29; per-prim refineLevel wins over usdview's complexity because the override index is an underlay (`research/A5-storm-curves-shading.md` §10) |
| `purpose/purpose` | inherited by hand from the description | S4 — usdGen runs after flattening, so nothing is inherited automatically |
| `visibility/visibility` | inherited by hand from the description | S4 |
| `materialBindings` | one entry under `HdMaterialBindingsSchemaTokens->allPurpose`, whose **value is the empty token** — never the literal string `"allPurpose"`, which is the separate `_allPurposeToken` — binding to the description's `Material` (three terminals, S36); to `material_storm` only when gate L-1 flips the default | ADR §5.6; `pxr/imaging/hd/materialBindingsSchema.h:36-39` |
| `primOrigin/scenePath` | the path selected by `uniform token usdGen:pickTarget` (`description` \| `terminal`, default `description`; `02-schema.md` §2.3) — **absolute** outside prototypes, **relative** inside them. Written by the imaging layer, never by a kernel | S29; a synthesized prim with no `primOrigin` picks as an empty path (MEASURED, `research/G-instancing-cards-archives-and-native-instances.md` §6) |
| `__dependencies` | **one** entry per tile: `dependedOnPrimPath` = the surface, `dependedOnDataSourceLocator` = `primvars/points`, `affectedDataSourceLocator` = `primvars/points` | S30; `HdDependencyForwardingSceneIndex` fan-out is a MEASURED **0.17 / 0.38 / 0.90 / 1.30 µs** per affected prim at 32 / 1 k / 10 k / 100 k dependents (**EV-031**, `appendix-A-evidence-ledger.md` §2.4; `research/G-storm-throughput-and-prim-granularity.md` §1.8) — so 130 ms for one mesh dirty at 100 k dependents (**DERIVED from EV-031**). One edge per tile, never per curve, is what keeps usdGen off that curve |

Field names verified at `pxr/imaging/hd/dependencySchema.h:35-38`. `primOrigin`'s single field is
`scenePath` (`pxr/imaging/hd/primOriginSchema.h:37`).

**The exactness assert is not optional.** A `points` array longer than `Σ curveVertexCounts` is
replaced wholesale by the fallback colour with only a `TF_WARN`; with `curveIndices` it works but
costs 3.46 ms instead of 0.22 ms (MEASURED, `research/G-storm-throughput-and-prim-granularity.md`
§1.4). Padding and array reuse are therefore off the table, and the density scrub keeps element
counts fixed by parking culled strands at a degenerate point with zero width, committing the real
count on release (S28). Gate SI-1 asserts the equality on every tile; gate **S-7** measures the
parked-CV drag against a real count change (§10.1).

**Velocities.** Emitting a `velocities` primvar that is inconsistent with `points` at the same sample
time makes `HdsiVelocityMotionResolvingSceneIndex` silently replace the deformation samples with
`{start, end}` (`pxr/imaging/hdsi/velocityMotionResolvingSceneIndex.cpp:176-190`). usdGen therefore
blocks `velocities`/`accelerations` whenever it owns `points`, and unblocks them only in profile P1,
which the artist opts into per description via `usdGen:motion:mode = velocities` (S32).

### 4.2 Guides and the CV points child

`guides/<setName>` is a `basisCurves` prim per `UsdGenGuideSet` with `purpose/purpose = guide` and
`primvars:usdGen:role = "guide"`. It gets its own prim set rather than sharing tiles because a
different material or refineLevel fragments the draw batch anyway (S30;
`pxr/imaging/hdSt/commandBuffer.cpp:164-177`). The usdview plugin forces
`viewSettings.displayGuide = True` so guides are visible, exactly as usdRig does for its rig guides
(S43).

`guides/<setName>/cvs` is a synthesized `points` prim carrying `points`, constant `widths` and
`uniform displayColor`. Selection state is a **colour**, not a Hydra selection:
`HdSelectionSchema` carries only `fullySelected`/`nestedInstanceIndices`, and the engine exposes no
point-selection API (S40; `research/G-tool-loop-array-transport-and-cv-picking.md` §3). The child
prim exists only while a brush that needs CV display is active; it is added and removed with real
`PrimsAdded`/`PrimsRemoved`, which is acceptable because it happens on a tool-mode change, not during
a drag.

### 4.3 Instancers and re-rooted prototypes (S33, S34)

`inst_<opName>` is a real `instancer` prim emitted by `UsdGenInstance` (cards, archives, spheres,
clump-instanced geometry). Contract:

| Data source | Content | Evidence |
|---|---|---|
| `instancerTopology/prototypes` | `VtArray<SdfPath>`, each a **namespace child** of the instancer | `pxr/imaging/hd/instancerTopologySchema.h:36-41` |
| `instancerTopology/instanceIndices` | `HdIntArrayVectorSchema` — a *vector of* `VtIntArray`, one per prototype (the `hydra_prim_schemas.dox` claim of `HdInstanceIndicesSchema` containers is stale) | `pxr/imaging/hd/instancerTopologySchema.h:125-126`; MEASURED, `research/G-instancing-cards-archives-and-native-instances.md` key facts |
| `instancerTopology/mask` | `VtBoolArray`, empty = all true | same header |
| `primvars/hydra:instanceTranslations`, `…Rotations`, `…Scales` | `instance` interpolation | `pxr/imaging/hd/tokens.h:125-127` |
| arbitrary `instance` primvars | per-card tint, melanin, UV offset — Storm applies no filter to instance-rate primvars | `pxr/imaging/hdSt/primUtils.cpp:211-221` |
| prototype `instancedBy/paths` | **exactly one** path, hand-authored; more raises `TF_CODING_ERROR` | `pxr/imaging/hd/instancedBySchema.h:37`; `sceneIndexAdapterSceneDelegate.cpp:2684-2688` |
| `primOrigin/scenePath` on instancer **and** every prototype | absolute outside prototypes, relative inside | MEASURED (§4.1) |

Material variety across cards/archives is **multiple prototypes with partitioned `instanceIndices`**;
Storm has no per-instance material binding (`pxr/imaging/hdSt/basisCurves.cpp:609`,
`hdSt/mesh.cpp:3267,3338`). Continuous variation is `instance` primvars. A prim carrying
`instancedBy` is drawn only through its instancer, so no extra hiding step is needed.

For grooms on **natively instanced** scalps, hair is generated once per propagated prototype path
(`…/UsdNiInstancer/UsdNiPrototype/…`). Propagated prototype names are hashes
(`ForInstancer%zx` from `TfHash::Combine`,
`pxr/usdImaging/usdImaging/piPrototypePropagatingSceneIndex.cpp:406-433`): **discover them from
`__usdPrimInfo.piPropagatedPrototypes` / `.isNiPrototype` / `.niPrototypePath`
(`pxr/usdImaging/usdImaging/usdPrimInfoSchema.h:46-48`), never construct them** (S34).
`UsdGenSurfaceResolver` (§3.10) rebases an authored `usdGen:surface` target onto the prototype root
using the ancestor's `niPrototypePath` and the prim's `primOrigin`; that rebase arithmetic is an ASSUMPTION on
top of MEASURED discovery and is what gate **T-INST-2** exists to test. Gate **T-INST-1** asserts the
pick round-trip: `HdxPrimOriginInfo::ComputeInstancerContext()` returns `[(instancerPath, index)]`
with `primOrigin` and an **empty vector** without it (MEASURED, probe4 cases (a) vs (b)), and usdview
falls back to `Sdf.Path.emptyPath, -1` when it is empty
(`pxr/usdImaging/usdviewq/stageView.py:2331-2340`).

Interactive card edits dirty `primvars/hydra:instanceTranslations` (→ `DirtyPrimvar`, one BAR
re-upload), **never** `instancerTopology`, which forces an index rebuild for every prototype rprim.
The direction that matters is locator → bit:
`HdDirtyBitsTranslator::InstancerLocatorSetToDirtyBits` maps `instancerTopology` to
`DirtyInstanceIndex` (`pxr/imaging/hd/dirtyBitsTranslator.cpp:1148`, `:1182-1183`; the reverse
mapping `InstancerDirtyBitsToLocatorSet` at `:416-456` is the cross-check), and the rebuild itself is
`pxr/imaging/hdSt/primUtils.cpp:1174-1220`.

### 4.4 `material_storm` (gate L-1)

Source evidence says Storm already resolves `outputs:glslfx:surface` ahead of plain
`outputs:surface`: `HdStRenderDelegate`'s render-delegate info lists render contexts
`{glslfx, mtlx}` (`pxr/imaging/hdSt/renderDelegate.cpp:695-707`, the range ADR §9 R43 accepts) and
`HdsiMaterialRenderContextFilteringSceneIndex` selects "the first render context encountered in
`renderContextPriorityOrder`" (`pxr/imaging/hdsi/materialRenderContextFilteringSceneIndex.h:20-45`).
If gate L-1 (EGL harness, M1) confirms it, the per-delegate override is deleted and tiles bind the
one authored `Material` with three terminals (S36). If it fails,
`USDGEN_STORM_MATERIAL_OVERRIDE=1` becomes the default: usdGen synthesizes
`<Description>/__usdGenRender/material_storm` carrying only the glslfx network (plus `primOrigin`)
and binds tiles to it. The hedge is a deletable code path with a one-afternoon gate, not a permanent
fork. Details in `07-look-maps-expressions.md` §3.3.

---

## 5. Invalidation discipline (ADR §5.2)

### 5.1 The rules

| Case | Emit | Never emit |
|---|---|---|
| a tile usdGen owns, points changed | `primvars/points/primvarValue` (**bare leaf**) + `extent/min` + `extent/max` | the `ComputeDirtyLocators` container sentinel |
| a tile usdGen owns, width-writing node dirty | additionally `primvars/widths/primvarValue` | — |
| a tile usdGen owns, colour-only edit | `primvars/displayColor/primvarValue` alone | anything co-dirtied with `points` |
| a primvar appears for the first time | `primvars/<name>` once | a `…/primvarValue` dirty (it does not make a new primvar visible — MEASURED, `research/G-tool-loop-array-transport-and-cv-picking.md` §5) |
| usdGen **overlays an upstream prim** — `visibility/visibility = false` on a C3 source prim outside the groom, a CV-display child, any primvar the index adds (§3.4.1); and, in v2 only, the in-place points path behind gate **S-10** (`05-static-curves-and-deformation.md` §5.7) | `HdContainerDataSourceEditor::ComputeDirtyLocators(...)` **and the bare `primvars` locator** (S5, ADR §9 R29) | the bare leaf alone |
| the surface's `xform` dirtied | the tile's own `xform/matrix` | nothing — Hydra dirtiness is per prim, never hierarchical (S4) |
| a prim's owned-leaf set or type changed | `PrimsRemoved` + `PrimsAdded` | `UniversalSet` as a substitute for a resync |
| a card/hair transform edit at constant count | `primvars/hydra:instanceTranslations` | `instancerTopology` |

**Why the bare leaf.** `HdSceneIndexAdapterSceneDelegate::_PrimsDirtied` clears the cached primvar
descriptors for a prim whenever a dirtied locator starts with `primvars` and does *not* end in
`primvarValue`/`indexedPrimvarValue`/`indices`
(`pxr/imaging/hd/sceneIndexAdapterSceneDelegate.cpp:510-522`). `ComputeDirtyLocators` emits exactly
such a locator, at a MEASURED 0.51 µs/prim/frame of descriptor recompute. usdGen is the sole producer
of its tile prims and the adapter re-pulls `GetPrim` through a one-entry per-thread memo, so it never
holds a stale container — the bare leaf is safe and keeps the cache warm (saves 0.51 ms/frame at
1 000 prims: **DERIVED from EV-030**, whose MEASURED terms are 0.105 µs/prim for
`ComputeDirtyLocators` and 0.51 µs/prim/frame for the descriptor recompute it forces;
`appendix-A-evidence-ledger.md` §2.4, `research/G-storm-throughput-and-prim-granularity.md` §1.9).

The rule has one precondition worth naming, because it is the thing that would quietly invalidate it:
**no caching filter may sit between usdGen and the render index.** In stock 26.08 none does —
`UsdImagingGLEngine` appends `HdCachingSceneIndex` only when
`USDIMAGINGGL_ENGINE_ENABLE_CACHING_SCENE_INDEX` is set, and it defaults to **false**
(`pxr/usdImaging/usdImagingGL/engine.cpp:86-88`, `:249-255`, `:1782-1786`; verified). A host that
turns it on must be treated as the overlay case. `testUsdGenTileContract` asserts the env setting
is off and skips with an explanatory message when it is not.

The in-place overlay path — publishing deformed points onto a frozen source prim instead of onto
tiles — **does not exist in v1**: `usdGen:output (tiles|inPlace)` is dropped everywhere (ADR §9 R8)
and the optimisation sits behind new gate **S-10** (T2, M8: PrimsRemoved/Added versus an in-place
points dirty on a frozen prim; R29, R38, R40). The v1 overlay cases are the three named in the row
above.

**Why the bare `primvars` locator in the overlay case.** S5 is a *must*. usdRig upstream of
UsdSkelImaging freezes the skin: the resolved prim's captured `HdPrimvarsSchema`
(`pxr/usdImaging/usdSkelImaging/dataSourceResolvedPointsBasedPrim.h:245`) goes stale because the only
test that rebuilds it is `dirtyLocators.Contains(HdPrimvarsSchema::GetDefaultLocator())`
(`pxr/usdImaging/usdSkelImaging/dataSourceResolvedPointsBasedPrim.cpp:1178-1180`), and usdRig only
sends child locators. Promoting the dirty to the bare `primvars` locator unfreezes it exactly
(MEASURED, `research/G-chain-order-probe.md` §4c). Forgetting it is a silent wrong-geometry bug, so
the publisher **asserts** it whenever it writes into an overlay container, and
`testUsdGenSkelInterop` covers it. (S46: file the corresponding bug against usdRig.)

**Why never co-dirty `displayColor` with `points`.** `DirtyPrimvar` is one bit for *every* primvar
except points/normals/widths (`pxr/imaging/hd/changeTracker.cpp:957-978`). Setting it drops the prim
out of Storm's points fastpath, re-pulls all primvar descriptors, and re-uploads every non-points
primvar — `hairT`, `st`, `hairId`, `displayColor` and, in variant B, the tangent
(`pxr/imaging/hdSt/basisCurves.cpp:875-935`; MEASURED analysis in
`research/G-storm-throughput-and-prim-granularity.md` §1.5).

That last fact is exactly why ADR §5.4 makes `hairTangent` a **measured fork**. Variant A publishes
no tangent primvar at all, so a deforming frame changes only `points` and stays on the fastpath;
variant B (`UsdGenHairPreviewPrimvar`, ADR §9 R4) publishes a `hairTangent` vertex primvar and
therefore re-uploads every non-points primvar per deforming frame (≈ +1.5–2.5 ms at 100 k × 8 CV:
**DERIVED from EV-022**, the measured per-frame deform delta, scaled over the full-primvar path —
never measured as such; `appendix-A-evidence-ledger.md` §2.11). Gate **S-8** measures both variants
and decides which ships (§4.1, §10.1); if B wins, `09-performance-and-benchmarks.md` §0.2's ledger no
longer fits 16.6 ms (R41).

### 5.2 What Storm does with each locator

| Locator usdGen emits | Storm dirty bit | Consequence |
|---|---|---|
| `primvars/points` (or `.../primvarValue`) | `DirtyPoints` | points-only fastpath; nothing but the points buffer source is touched (`hdSt/basisCurves.cpp:930-998`) |
| `primvars/widths` | `DirtyWidths` | width buffer only |
| `primvars/<other>` | `DirtyPrimvar` | full vertex loop, all non-points primvars re-uploaded |
| `basisCurves/topology` | `DirtyTopology` | cubic index rebuild 2.6 ms warm / 5.2 ms cold per 500 k patches, whole-VBO relocation, deep validation of **all** batches (MEASURED, `research/G-storm-throughput-and-prim-granularity.md` §1.6) |
| `extent/min` and `extent/max` | `DirtyExtent` | frustum-cull bbox |
| `displayStyle/refineLevel` | `DirtyDisplayStyle` | geometric shader change → batch rebuild |
| `xform/matrix` | `DirtyTransform` | — |

Mapping verified at `pxr/imaging/hd/dirtyBitsTranslator.cpp:823-845` (the `primvars` locator is
decomposed so that `primvars/points` maps to `DirtyPoints` only) and `:638-645` (basisCurves
topology).

### 5.3 Resync versus dirty

A prim-type change is `PrimsRemoved` + `PrimsAdded` (`research/A2-usdrig-imaging.md` §5). Because
operator **mode** switches are `usdGen:mode` token edits rather than prim delete/create (ADR §2.1),
the common artist action — "make this Scatter follow the guides instead of scattering randomly",
`usdGen:mode` `random` → `atGuides`, both v1 (ADR §9 R38: `random` M1, `atGuides` M3) — is a value
edit that raises `UsdGenDirtyStructural` inside the engine and republishes tile *contents*, not a
stage resync.
That is deliberate: it keeps `UsdStage::RemovePrim` out of interactive paths, where an OpenExec system
attached to the stage raises a spurious `Tf.ErrorException`
(`pxr/exec/esfUsd/stageData.cpp:361` — the `if (!UsdPrimDefaultPredicate(resyncedPrim))` test;
`appendix-A-evidence-ledger.md` §3.10, S41/S46).

`usdGen:enabled` is **non-structural** for topology-preserving operators — the node becomes a memcpy
pass-through, no recompile, no recapture — and routes as `UsdGenDirtyTopology` (recapture downstream,
one topology publish, still no recompile) for generators, `UsdGenResample`, `UsdGenLength`,
`UsdGenFreeze` and `UsdGenInstance`, whose topology contribution vanishes (ADR §2.3 as ADR §9 R14
amends it; `02-schema.md` §6.2). Muting is the most common A/B action an artist performs; making it
a recompile would cost a 10–150 ms capture (ASSUMPTION, `design/judge-evidence.md` §2.3).

### 5.4 Notice batching (S30)

usdGen emits one `PrimsDirtied` call carrying one entry per dirty tile, not one call per tile. The
per-entry tax is ~0.2 µs and the plumbing is ≪ 1 ms even at 5 000 entries (MEASURED,
`research/G-evaluation-scheduling-and-batching.md` §9). usdGen does **not** wrap frames in
`MergingSceneIndexNoticeBatchBegin/End`: batching inverts the frame/geometry order (the
`currentFrame` dirty arrives *first*), and no in-tree application does it, so the commit logic must
not depend on either order. §3.9 says how trigger (b) survives a host that batches anyway, and gate
SI-3 runs the batched variant. `HdNoticeBatchingSceneIndex` does not delay *data*, only notices
(`research/G-evaluation-scheduling-and-batching.md` §7), which is precisely why `GetPrim` must serve the
*published* generation and not upstream values usdGen has not consumed yet.

---

## 6. Time and render context

### 6.1 `sceneGlobals/currentFrame` is the stock time source

`UsdImagingGLEngine::_SetSceneGlobalsCurrentFrame(params.frame)` runs on every `PrepareBatch`/`Render`
in both usdview and `usdrecord` (`pxr/usdImaging/usdImagingGL/engine.cpp:483-498`, `:2062-2073`;
`research/G-hdprman-and-usdrecord-render-time-chain.md` §8). Because
`HdsiSceneGlobalsSceneIndex` is inserted by the engine's phase-0 `AtStart` app callback and usdGen is
phase-0 `AtEnd`, usdGen reads the identical
`HdSceneGlobalsSchema::GetFromSceneIndex(_GetInputSceneIndex()).GetCurrentFrame()` in interactive and
batch — one code path. hdPrman reads the same value for `Ri:Frame`
(`hdPrman/renderSettings.cpp:278-291`).

Two behaviours to code against. `SetTime` with an unchanged value early-outs and produces no notice
(`pxr/usdImaging/usdImaging/stageSceneIndex.cpp:363-365`, MEASURED). And nobody in `usdImagingGL`,
`usdviewq` or `usdAppUtils` calls `HdsiSceneGlobalsSceneIndex::SetTimeCodesPerSecond` (grep empty) —
but the stage's `timeCodesPerSecond` still reaches the chain: `UsdImagingDataSourceStage` publishes
it in the `sceneGlobals` container at the absolute root
(`pxr/usdImaging/usdImaging/dataSourceStage.cpp:57-72`, served at
`stageSceneIndex.cpp:247-249`), the scene-globals index **overlays** rather than replaces that
container, and the velocity index reads the schema first and falls back to a hard-coded 24 only when
no opinion exists (`pxr/imaging/hdsi/velocityMotionResolvingSceneIndex.cpp:273-287`;
`appendix-A-evidence-ledger.md` §3.2, §6 K22). usdGen's P1 profile reads fps from the same
scene-globals value and documents 24 as the no-opinion fallback rather than assuming it.

### 6.2 App-driven time

Trigger (a) in §3.9. The usdview plugin listens to `currentFrameChanged` and uses **the signal's
frame**, because `RootDataModel.currentFrame`'s setter emits before assigning
(`pxr/usdImaging/usdviewq/rootDataModel.py:152-162`), and `AppController._setFrameIndex` assigns and
only then calls `_updateOnFrameChange()`, which renders
(`pxr/usdImaging/usdviewq/appController.py:3872-3878`). That gives a guaranteed pre-redraw commit
point. The same C entry point serves batch drivers that wrap `usdrecord`.

### 6.3 Motion sample requests

`primvars/points/primvarValue` on a tile is a `UsdGenSampledPointsDataSource`
(`HdTypedSampledDataSource<VtVec3fArray>`) implementing the three profiles (S32):

| Profile | `usdGen:motion:mode` | `GetContributingSampleTimesForInterval` | `GetValue(t)` |
|---|---|---|---|
| P0 single (default) | `single` | `false` | the retained offset-0 array |
| P1 velocities | `velocities` | `false`; additionally publishes `primvars/velocities` | retained offset-0; the downstream velocity index extrapolates |
| P2 samples | `samples` | `true` + ≥2 retained offsets | nearest/lerp of the two bracketing retained offsets, clamped outside |

Storm **never** requests samples: `hdSt` contains no `SamplePrimvar`, basisCurves reads `GetValue(0)`
directly (`pxr/imaging/hdSt/basisCurves.cpp:975-990`), and the delegate's fallback interval is `[0,0]`
(`pxr/imaging/hd/sceneIndexAdapterSceneDelegate.cpp:142-143`). So P2 costs the viewport nothing.

The retained per-offset cache is keyed `(graphGen, surfaceGen, absTime)` with lerp and clamp for
non-retained times (ADR §4.1). Keying by *absolute* time makes whole-frame offsets reuse the previous
frame's `+1` as this frame's `0` when scrubbing forward. Capacity is `usdGen:motion:sampleCount` (= `k`) **retained
offsets** per prim, on top of the offset-0 array that is already the published generation — so the
*extra* cost is `k` × 19.2 MB at 100 k curves × 16 CV (19.2 MB per point sample is MEASURED,
**EV-075**, `appendix-A-evidence-ledger.md` §2.9;
`research/G-motion-blur-sampling-strategy.md` §5): `k = 3` ≈ 58 MB per groom,
`k = 9` (prman `geosamples 9`) ≈ 173 MB — both **DERIVED from EV-075**, arithmetic only. The source
report writes the same budget as "`k+1` samples per prim" and then multiplies by `k`; `k` extra
offsets is the arithmetic that matches its own numbers. `usdGen:motion:sampleCount = 3` (2..16)
bounds it (ADR §2.3), with the caveat of §7 rule 1: the effective ceiling is 16 only for
RenderMan API ≥ 26 and 4 otherwise.

Only `points` and `xform/matrix` need samples. hdPrman collapses every other primvar to offset 0
(`hdPrman/renderParam.cpp:833-847`), so widths, colours, `st` and every baked primvar are
single-sample retained sources that return `false`.

Sub-frame clamping matters. A surface with samples at frames 1 and 24 answers
`GetContributingSampleTimesForInterval(-0.25, 0.25)` at `t=1` with `{-0.25, 0, 23}` — the far
bracketing sample is pulled in (MEASURED, `research/G-stage-free-parameter-and-time-transport.md` §3).
usdGen clamps to the requested window by default and forwards the surface's raw list only when
`usdGen:motion:forwardSurfaceSamples = true`.

### 6.4 Context and the density pair (ADR §2.3)

The session carries an explicit context, `interactive` or `render`. It is set by, in priority order:
`UsdGenImaging_SetContext("render")`, the `USDGEN_CONTEXT=render` environment variable (the batch
route), or a usdview toggle. **The renderer display name is not intent** — an artist previews with
hdPrman and renders with Storm — and `sceneGlobals` has no interactive flag: its token list is
`primaryCameraPrim activeRenderPassPrim activeRenderSettingsPrim startTimeCode endTimeCode
timeCodesPerSecond currentFrame sceneStateId` (`pxr/imaging/hd/sceneGlobalsSchema.h:37-46`, verified).

`float usdGen:densityScale = 1` applies **only** in the `interactive` context;
`float usdGen:renderDensityScale = 1` applies **only** in the `render` context. **The contexts are
exclusive and neither scale applies in the other**; both default to 1 (ADR §9 R13;
`02-schema.md` §2.3.1). Both decimate by stable id:

```
keep iff UsdGenHash32(curveId, kSaltDensity) < keepFraction · 2^32
keepFraction = clamp(scale_groom · scale_description, 0, 1)
```

with `kSaltDensity ≠ 0`, so the surviving set is never `{hairId < scale}` — `hairId` is
`UsdGenHash32(curveId, 0) / 2^32` and a shared salt would couple the two (ADR §9 R12). Curve ids,
sculpt deltas and clump ids therefore survive a density change, and the preview set is always a
subset of any larger scale's set. The session-only interactive ceiling
(`UsdGenImaging_SetInteractiveLOD`, §3.8) uses the same predicate with
`keepFraction = maxCurves / curvesAtScale1`. Because the prim set is a session property (§3.7) and
is sized from the maximum reachable density, changing the context changes tile *contents*, never the
tile count.

---

## 7. hdPrman and `usdrecord`

The same binary, the same registration and the same scene index serve both (S2). hdPrman paths below
are relative to `third_party/renderman/plugin/` in the OpenUSD tree; the plugin is **not built or
installed on this host** (`PXR_BUILD_PRMAN_PLUGIN` defaults OFF), so everything here is
source-verified, not run-verified (`research/ENVIRONMENT.md`;
`research/G-hdprman-and-usdrecord-render-time-chain.md` §0). What differs:

| | Storm | hdPrman |
|---|---|---|
| Chain construction | `engine.cpp:1776-1779` | the same call; the legacy `renderIndex.cpp:209-214` path is skipped because the engine passes a terminal scene index |
| Display names | `"GL"` | `"RenderMan RIS"` (`hdPrmanLoader/plugInfo.json:10`, the renderer plugin's own `displayName`); the full set `"RenderMan RIS"`, `"RenderMan XPU"`, `"RenderMan XPU - CPU"`, `"RenderMan XPU - GPU"` appears in hdPrman's `loadWithRenderer` arrays (`hdPrman/plugInfo.json:170`) |
| Skinned scalps | `primvars/points` **blocked**; usdGen's private wrapper restores them | `primvars/points` **blocked here too**. usdGen is an all-renderers phase-0 entry and the registry orders all-renderers entries ahead of renderer-specific ones in the same phase (`pxr/imaging/hd/sceneIndexPluginRegistry.cpp:966-1005`), so `HdPrman_ExtComputationPrimvarPruningSceneIndexPlugin` (`hdPrman/plugInfo.json:168-180`, tag `hdPrman:phase0`) runs *after* usdGen. The private wrapper does the real work under hdPrman as well; hdPrman's own wrapper downstream is then the harmless pass-through (`pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.cpp:695-730`) |
| Material | glslfx (or `material_storm` under L-1) | MaterialX `chiang_hair_bsdf` via `outputs:mtlx:surface`; `materialRenderContexts = {ri, mtlx}` (`hdPrmanLoader/rendererPlugin.cpp:222-238`) |
| Motion | never samples | `_MotionBlurTypedSampledDataSource` wraps every primvar; only `points` and `xform.matrix` blur |
| Pinned curves | native pinned index path | `pinned` → `nonperiodic`; `HdsiPinnedCurveExpandingSceneIndex` expands bspline by +2 CVs per end, per shutter sample |
| Dependency forwarding | `HdSt_DependencyForwardingSceneIndexPlugin`, phase 1000 AtEnd | `HdPrman_DependencyForwardingSceneIndexPlugin`, phase 1000 AtEnd — same class |

Rules the tile data source must satisfy under hdPrman
(`research/G-hdprman-and-usdrecord-render-time-chain.md` §3.5):

1. Return `true` with **2..k** times whose union covers the requested interval, or `false` when the
   groom is static. Excess samples are **truncated, not resampled** — the delegate simply
   `resize`s the list (`pxr/imaging/hd/sceneIndexAdapterSceneDelegate.cpp:2454-2457`). `k` is
   `HDPRMAN_MAX_TIME_SAMPLES`, which is **16 only for RenderMan API ≥ 26 and 4 otherwise**
   (`third_party/renderman/plugin/hdPrman/renderParam.h:66-70`, an `#if _PRMANAPI_VERSION_MAJOR_ >= 26`).
   So `usdGen:motion:sampleCount > 4` must emit one `TF_WARN` naming the resolved ceiling when the
   render delegate reports an older RenderMan, rather than let samples vanish silently. UNMEASURED
   here — hdPrman is not built on this host (`research/ENVIRONMENT.md`).
2. Guarantee a **constant CV count across the shutter**. hdPrman's ordinality check pulls
   `GetValue(t).GetArraySize()` at every returned time and drops to a single sample on a mismatch
   (`hdPrman/motionBlurSceneIndexPlugin.cpp:353-363`). Topology-changing operators evaluate their
   topology decision once per frame, never per shutter sample.
3. `GetValue(t)` is called at every returned time **twice** — once for the ordinality check, once for
   the real pull — and at arbitrary `t` when `ri:object:geosamples` or `blurScale` are authored. The
   per-offset cache (§6.3) is what makes that cheap; recomputing per call would be catastrophic.
4. Blur is disabled entirely until the render camera syncs (shutter defaults to `0,0`), so usdGen must
   never assume a non-empty interval.
5. Motion cost is `H_cached + k·tail + (k-1)·m`, not `k·(H+tail)`, because operators are classified
   `restSpace | deformedSpace` and only the deformed tail re-runs per offset (S25;
   `research/G-motion-blur-sampling-strategy.md` §4.3). Gate **R-1** asserts `k·tail`, not `k·chain`,
   evaluations for a 3-sample `usdrecord`.

hdGp is not a fallback under RenderMan: hdPrman ships no resolver of its own and the universal one is
gated by `HDGP_INCLUDE_DEFAULT_RESOLVER` (default false), which neither `usdrecord` nor usdview sets
(`pxr/imaging/hdGp/sceneIndexPlugin.cpp:25-27,60-66`). An hdGp-based hair system would render nothing
under `usdrecord -r "RenderMan RIS"` out of the box; an `HdSceneIndexPlugin` has no such gate (S6).

**Verification without RenderMan.** hdPrman is not built on this host
(`research/ENVIRONMENT.md`). The ordering can still be exercised: register synthetic `hdPrman:*` tags
and use `HdSceneIndexPluginRegistry::SetPluginOrderingPolicy` plus
`LoadAndGetSceneIndexPluginIds("RenderMan RIS", "")`
(`pxr/imaging/hd/sceneIndexPluginRegistry.cpp:1505-1559`). That is gate **SI-5**'s second half and
runs in tier 1.

---

## 8. Extent: two channels

Framing and culling are different mechanisms and both must work (`design/proposal-artist.md` §5.7).

**Channel 1 — Hydra `extent` on every published prim.** Recomputed with the points, every deforming
frame, as a min/max pass fused into the same interleave loop (**0.05 ms for 49 tiles**, the tile
count at 100 k / chunk 512 / `tileTarget` 64 — ADR §9 R21. The number is an **ASSUMPTION**, never
measured in isolation; gate **S-2** measures it as part of the deform frame, and
`09-performance-and-benchmarks.md` §0.2 carries it as one row of the only frame ledger). Safe to
author at the renderer level because
`UsdImagingExtentResolvingSceneIndex` runs far upstream (`usdImaging/sceneIndices.cpp:232-234`), so a
downstream overlay wins. Without it the prim is never frustum-culled (§4.1, MEASURED); gate **S-4**
is what proves the extent actually culls (§10.1).

**Channel 2 — a `UsdGeomComputeExtentFunction` for `UsdGenDescription`.** usdview's "frame selected"
goes through `UsdGeomBBoxCache`, which ignores Hydra entirely. `UsdGenDescription` derives from
`UsdGeomBoundable` (ADR §2.1), so it needs a registered extent function — and ADR §9 R19 fixes where
that function lives: **not** in `usdGenImaging`. `usdGenSchema` ships as a **library** plugin,
`libusdGenSchema.so`, holding the schema tokens and this registration and linking `usd`/`usdGeom`
plus the static `usdGenMath` archive that carries the extent kernel — no `hd`, no `usdImaging`, no
`usdGen` (`10-build-dependencies-testing.md` §1.2); the schema classes stay codeless (no generated
C++ classes). That keeps extents
working in `usdcat` and `usdrecord` without dragging `usdImaging`/`hd` in, and keeps `usdGen` core
free of `usd` (R19, R37).

```cpp
// usdGenSchema/extent.cpp -- inside PXR_NAMESPACE_OPEN_SCOPE, like usdGeom's own subscription
TF_REGISTRY_FUNCTION(UsdGeomBoundable)
{
    const TfType type = TfType::FindByName("UsdGenDescription");
    if (!type.IsUnknown()) {
        UsdGeomRegisterComputeExtentFunction(type, &UsdGenSchema_ComputeDescriptionExtent);
    }
}
```

`UsdGeomRegisterComputeExtentFunction` requires the type to derive from `UsdGeomBoundable`
(`pxr/usd/usdGeom/boundableComputeExtent.cpp:271-289`) and keys by `TfType`, not by a template
argument — which is what makes it usable from a **codeless** schema, exactly as usdRig does for its
codeless Boundables (`usdRig/libs/rigExecImaging/registry.cpp:1131-1155`).

`UsdGenSchema_ComputeDescriptionExtent` is the name `02-schema.md` §7.2 and
`10-build-dependencies-testing.md` §1.2 declare; its kernel body lives in `usdGenMath`, so
`usdGenSchema` links no Hydra (`10-build-dependencies-testing.md` §4.3).

The default implementation is stage-only: it unions the bound surfaces' extents with the
description's maximum reachable length and width. When `usdGenImaging` is loaded in the same process
it installs a better one through the single setter `libusdGenSchema.so` exports —
`void UsdGenSchema_SetExtentProvider(UsdGenSchemaExtentFn)`, called from `usdGenImaging`'s own
registry function (`10-build-dependencies-testing.md` §4.3) — so an interactive session gets the live
generation's bounds and a plain `usdcat` run still gets an answer. The dependency direction is
`usdGenImaging → usdGenSchema`, never the reverse. The provider reads the live generation **only if
it describes this stage at this time**, and otherwise falls back to the surface extent. That gate is not optional: the store is
a process-global singleton, so an ungated lookup answers a query about stage A frame 12 with
stage B's frame 30 pose — "plausible, wrong, and undetectable downstream"
(`usdRig/libs/rigExecImaging/registry.cpp:1070-1083`). Identity is the stage **object**, since two
stages commonly share a root layer and differ only by session layer.

Two plumbing requirements, both easy to get silently wrong:

* The schema plugin must declare `"implementsComputeExtent": true` on `UsdGenDescription`, or
  `_LoadPluginForType` returns early and the extent never arrives
  (`pxr/usd/usdGeom/boundableComputeExtent.cpp:155-165`).
* That flag is a promise that Plug can *load code*, and Plug loads the plugin that **declared the
  TfType** (`_LoadPluginForType` → `plugReg.GetPluginForType(type)`, `:155-165`). The schema
  plugInfo must therefore be a `"Type": "library"` variant whose `LibraryPath` names
  **`usdGenSchema`** — Plug treats a `resource` plugin as already loaded and never dlopens it. The
  checked-in, data-only `resource` copy keeps the flag **off**; only the generated copy sets it.
  usdRig's CMake does exactly this and explains why (`usdRig/CMakeLists.txt:409-462`);
  `10-build-dependencies-testing.md` §4.3 copies the recipe.

---

## 9. Diagnostics

| Channel | Content |
|---|---|
| Env kill switch | `USDGEN_ENABLE=0` — `_IsEnabled` returns false, the plugin returns its input, nothing else changes (§1.2) |
| Env | `USDGEN_CONTEXT`, `USDGEN_CHUNK_SIZE`, `USDGEN_THREAD_LIMIT`, `USDGEN_MEMORY_BUDGET_MB`, `USDGEN_STORM_MATERIAL_OVERRIDE`, `USDGEN_DIAGNOSTICS`, `USDGEN_IMAGING_DLL` (out-of-tree library override, the `RIGEXEC_IMAGING_DLL` pattern, S43). `10-build-dependencies-testing.md` §3.5 owns the single env-var registry (ADR §9 R35); any new variable is added there first |
| `TF_DEBUG` | Exactly six codes ship: `USDGEN_COMPILE`, `USDGEN_DIRTY`, `USDGEN_COMMIT`, `USDGEN_PUBLISH`, `USDGEN_CAPTURE`, `USDGEN_MEMORY` (`10-build-dependencies-testing.md` §7.2 declares them, `03-execution-engine.md` §9.2 and `09-performance-and-benchmarks.md` §6.2 print the same six; ADR §9 R35). `USDGEN_GRAPH` is a synonym of `USDGEN_COMPILE` and `USDGEN_SESSION`/`USDGEN_MAPS`/`USDGEN_EXPR`/`USDGEN_TIMING` are **retired** — no document may reintroduce one. Imaging writes `USDGEN_DIRTY` (routing), `USDGEN_COMMIT` (commit reason, session keying) and `USDGEN_PUBLISH` (generation number, tiles, notices) |
| Trace scopes | `TRACE_SCOPE` / `TRACE_FUNCTION` (`pxr/base/trace/trace.h:30,35`). `03-execution-engine.md` §9.2 is the declaration site and `09-performance-and-benchmarks.md` §6.2 prints the identical table; the ones this document raises are **`UsdGen::Route`** (`_PrimsDirtied` accumulation and the locator → node → chunk → tile hops; gates SI-2, SI-3), **`UsdGen::Interleave`** (SoA → AoS plus the per-tile extent; E-1, S-2) and **`UsdGen::Publish`** / **`UsdGen::Diff`** (generation swap, prev/next diff, notice emission; SI-2, SI-3, SI-4). Scope names are plain — the gate is named beside a scope, never inside it, so `UsdGen::SI-3::Commit` and the other gate-prefixed spellings are not used. `TfTrace` is **not** an OpenUSD 26.08 symbol (`10-build-dependencies-testing.md` §7.2) |
| `UsdGenStats` | `03-execution-engine.md` §9.2 is the normative declaration; this document does not redeclare it. The counters imaging writes are `commits`, `publishedTiles`, `noticeEntries` and `supersessions`, plus the ring of the last 120 commit durations split by phase — the numbers SI-3 and SI-9 read |
| `UsdGenNodeStats` | per-operator time, capture time, chunks touched — read by the tool's stack-profiler column (`08-tools.md` §5.3) and exposed through `UsdGenImaging_GetStatsJson()` for `testusdview` assertions |
| usdview HUD | one line, identical to the example `09-performance-and-benchmarks.md` §6.1 owns: `usdGen 0.9 ms  49/49 tiles  196/196 chunks  gen 1042  86 MB` |
| Hydra debug | `HD_SCENE_INDEX_PLUGIN_ORDERING=1` prints the resolved entry list (`sceneIndexPluginRegistry.cpp:1404-1419`); `HD_ENABLE_PERFLOG=1` is required for any Storm counter to be non-zero |

**Hard diagnostics, not silence** (`design/proposal-risk.md` §5.4). Three conditions are named errors
emitted once per groom per compile, never a silent pass-through:

1. `usdGen: surface <path> has computed points that could not be resolved` — `primvars/points` is null
   through the private wrapper and `extComputationPrimvars/points` exists. Something upstream produced
   a GPU-only computation with no CPU callback
   (`pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.cpp:383-398` warns and skips).
2. `usdGen: surface <path> resolved to prim type '<t>', expected 'mesh' or 'geomSubset'` — the
   resolver (§3.10) found a prim, but not a surface it can scatter on; usdGen passes the input
   through unchanged. `geomSubset` is legal and resolves to its parent mesh with a face restriction
   (ADR §2.3), so it must not be diagnosed.
3. `usdGen: <prim>.<property> authors a spline on a ramp property` — a `.spline` on a ramp is a
   compile error (§2.4), because accepting it would silently recook every frame.

Profiling on this host has to live with `perf_event_paranoid = 4` and yama: the in-process SIGPROF
sampler from `prototypes/data-plane-benchmark/sampler.h` is carried into `usdGenTestUtils`
(`research/G-data-plane-engine-prototype-benchmark.md`, host notes).

---

## 10. Testing

Tiers (S45, ADR §9 R2; definitions in `10-build-dependencies-testing.md` §5.1): **T1** headless
scene-index tests over the real `UsdImagingCreateSceneIndices` chain, sub-100 ms, no GL — the primary
regression suite; **T2** Storm correctness and GPU timing headlessly through the EGL
device-platform context (`prototypes/storm-hair-look/eglctx.h`, which renders on the GB10); **T3**
`testusdview` under the scratchpad Xvfb (CPU numbers only); **T4** workstation protocols, which are
release criteria and never milestone exits (R39). **T0** needs neither Hydra nor a stage
(`03-execution-engine.md`); nothing in this document is T0.

### 10.1 Gates this document is accountable for

`09-performance-and-benchmarks.md` §5 is **the single gate registry** (ADR §9 R40) — id, metric,
pass criterion, driver, tier, milestone and status — and `10-build-dependencies-testing.md` §6.5
restates it with the CTest name per id. This table cites both and states R40's values; every row
agrees with both siblings, including **SI-5** and **S-6** at milestone **M1**.

| Gate | Tier | Assertion | Threshold | Milestone |
|---|:--:|---|---|---|
| SI-1 exactness | T1 | `points.size() == Σ curveVertexCounts` on every published tile and guide prim | hard assert | M1 |
| SI-2 invalidation | T1 | a clump-amount edit dirties exactly `primvars/points/primvarValue` plus `extent/min` and `extent/max` on the dirty tiles and **nothing else** | exact locator set | M1, re-run M2 for overlaid prims |
| SI-3 cook count | T1 | 10 interactive events through the real chain (§3.9) | exactly 10 commits; every commit and notice on the app or notice thread (thread-id assert); **zero cooks inside `GetPrim`** — the registry wording verbatim | M1 |
| SI-4 no torn read | T1 | 8 reader threads × 100 publishes | 0 torn reads | M1 |
| SI-5 chain order | T1 | usdGen lands after UsdSkel/RigExec and before Storm's plugins; and, with synthetic `hdPrman:*` tags, before `hdPrman:motionBlur` | exact chain dump | M1 (first green in M0) |
| SI-6 initial population | T1 | notice path vs pre-populated path (§3.6) | identical prim sets; the bounded traversal ≤ **5 ms on the 11 005-prim stage** (ASSUMPTION until measured, R28) and the cost recorded | M1 |
| SI-7 adapter coverage | T1 | every declared property of every registered type's `UsdPrimDefinition` — all of them `usdGen:`-namespaced (R6), `usdGen:frozen:*` and `usdGen:sculpt:*` included — appears in the `usdGen` container **and** dirties when edited; plus `usdGen/rest/points` from the API-schema adapter | 100 % of `GetPropertyNames()` minus the Imageable builtins | M1 |
| SI-8 auto-applied API | T1 | `UsdGenMaskAPI` auto-applies to `UsdGenOperator` on a codeless type | mask properties present in `UsdPrimDefinition::GetPropertyNames()` | M0 pre-work, exits M1 |
| SI-9 pruning-wrapper cost | T1 | one commit's worth of surface pulls through the private `HdSiExtComputationPrimvarPruningSceneIndex` on a production-density skinned scalp (§3.5), asserting `UsdGenSurfaceReader` pulls once per `(surfacePath, surfaceGeneration)`, not once per tile, and that the wrapper resolves skinned points with `HD_ENABLE_SCENE_INDEX_EMULATION` off — driver `testUsdGenPruningCost` | cost recorded, and ≤ the S-2 deform budget | M2 |
| SI-10 sessions | T1 | two scene-index instances attached to one session (§3.7) report **identical generation, prim set and frame** for one commit; a renderer switch re-attaches and continues the generation counter — driver `testUsdGenSessions` | exact match on all three | M2 |
| S-2 deform frame | T2 | scene-index points publish across 49 tiles | ≤ 2.0 ms delta over static | M2 |
| S-3 one-tile edit | T2 | dirty 1 of 49 tiles | ≤ 1.2× static | M2 |
| S-4 culling | T2 | camera framing ¼ of the groom; 49 tiles with authored `extent` (§4.1, §8 channel 1) | `itemsDrawn` drops ≥ 3× | M2 |
| S-5 batches | T2 | 49 tiles, one material, one refineLevel | `drawBatches == 1`, `drawCalls == 1` | M1 |
| S-6 no relocation | T2 | 100 deform frames | `vboRelocated == 0` | M1 |
| S-7 density scrub | T2 | parked-CV drag (degenerate CV + zero width) vs a real count change (§4.1, S28) | parked ≤ 1.5× a points-only frame | M5 |
| S-8 `hairTangent` | T2 | variant A vs B frame time on a 100 k deforming groom; A compiles under `HDST_ENABLE_HGI_RESOURCE_GENERATION=1` | A wins or B becomes default | M0 pre-work, decides M1 |
| S-9 refineLevel | T2 | level 1 vs 2 frame time **and** the switch cost | tumble tier ships iff switch < 3 ms | M0 pre-work, decides M1 |
| L-1 render context | T2 | Storm resolves `outputs:glslfx:surface` before `outputs:surface` | pass ⇒ delete the override path | M0 pre-work, decides M1 |
| T-INST-1 pick round-trip | T1 | `HdxPrimOriginInfo::FromPickHit` on a synthesized instancer | non-empty instancer context; instance id round-trips | M6 |
| T-INST-2 prototype rebasing | T1 | groom inside a propagated native prototype | `usdGen:surface` resolves; `instancedBy` has exactly one path; `primOrigin` is relative | M6 |
| R-1 hdPrman parity | T4 | `usdrecord` with 3 motion samples | correct blur, `k·tail` evaluations | release (built M7) — no T4 gate is a milestone exit (R39) |

Thresholds and their derivations are `09-performance-and-benchmarks.md` §5, and the budget the Storm
gates are cut from is its §0.2 — the only frame ledger (R41). Two of the numbers the gates compare
against are not measurements and are tagged there: the Storm draw at 100 k × 8 CV / refineLevel 2 /
720p, **≈ 12.2 ms DERIVED** by linear interpolation between the MEASURED **EV-020** (5.01 ms at
40 k) and **EV-021** (23.93 ms at 200 k); and the usdGen deformed tail, **0.4–0.6 ms DERIVED** from
the styler pass **EV-014**–**EV-016** (0.158–0.545 ms single-thread) against the 8-thread five-node
chain **EV-001** (1.02 ms). Both provenances are `appendix-A-evidence-ledger.md` §2.11. Gate **S-1**
re-measures the first at 100 k in M1 for exactly that reason (ADR §7, R41).

### 10.2 Named tests (tier 1 unless the row says otherwise)

| Test | Proves |
|---|---|
| `testUsdGenChainOrder` | SI-5. The chain-order probe (`prototypes/chain-order/`) carried in verbatim as a **regression test**, not a one-off: it dumps the resolved chain with usdSkelImaging and rigExecImaging loaded and asserts usdGen reads post-deformation points. Make it loud if the order changes (`research/A2-usdrig-imaging.md` §9). |
| `testUsdGenAdapter` | SI-7. Walks every registered type's `UsdPrimDefinition`, asserts each declared property — all `usdGen:`-namespaced (R6), `usdGen:frozen:*` and `usdGen:sculpt:*` included — appears at its expected locator, edits each, and asserts exactly one dirty at that locator. Also asserts `usdGen:input` carries **all** its targets, not just the first (§2.3). |
| `testUsdGenAutoApply` | SI-8. `UsdGenMaskAPI`'s properties appear on every `UsdGenOperator` subtype of a codeless schema, and the explicit-apply path gives the identical `GetPropertyNames()` (§2.2 rule 2). |
| `testUsdGenRestAdapter` | §2.6. `usdGen/rest/points` reads at `Default()`, never flags time-varying, and is served for a `Mesh` but refused on a `GeomSubset` (R15). |
| `testUsdGenInvalidation` | SI-2. Parameter edit → exact locator set; primvar appearance → `primvars/<name>` once; colour edit → `displayColor` alone. |
| `testUsdGenScheduling` | SI-3. Counts commits and asserts the committing thread id for all three triggers — including trigger (c) **with** an app driver attached (R32) — then repeats the whole 10-event script under `MergingSceneIndexNoticeBatchBegin/End` and asserts identical published points (§3.9). |
| `testUsdGenSnapshotRace` | SI-4. 8 readers × 100 publishes, asserting `frame` and array contents come from one generation. |
| `testUsdGenPopulation` | SI-6. Both population paths against the same stage. |
| `testUsdGenSkelInterop` | S5. UsdSkel-skinned scalp + a usdGen overlay on frozen curves; asserts the bare `primvars` locator is emitted and the resolved skin does not freeze. |
| `testUsdGenTileContract` | SI-1 and C2. Asserts every data source, type, interpolation and role in §4.1 on a synthetic groom, including blocked velocities, `elementSize = 3` on `guideIndex`/`guideWeight`, the `__dependencies` edge count, the absence of any `normals` entry in `primvars`, and the all-purpose material binding under the **empty** token. Also asserts the §3.4.1 hiding rules: `<Description>` has exactly one child, `__usdGenRender`, and a C3 source prim outside the groom carries a `visibility = false` overlay whose dirty includes the bare `primvars` locator. |
| `testUsdGenInstancer` | T-INST-2, built on `prototypes/instancing/`: a groom inside a propagated native prototype, `usdGen:surface` rebasing, one `instancedBy` target, relative `primOrigin`. T-INST-1's pick round-trip has its own driver in the gate registry (`09-performance-and-benchmarks.md` §5.4). |
| `testUsdGenInstanceKeys` | §1.3. Asserts `usdGen` appears in the native-instance aggregation key and in the proxy-path-translation list — i.e. that `UsdGenMetadataSceneIndexPlugin`'s `TF_REGISTRY_FUNCTION(UsdImagingSceneIndexPlugin)` actually registered it. Two scalp instances differing only in `usdGen:*` must not aggregate. |
| `testUsdGenSurfaceResolver` | §3.10. `Mesh` target, `GeomSubset` target (parent mesh + face restriction, parent-mesh face ids), instance-proxy target, and a non-surface target that must raise diagnostic 2 exactly once. |
| `benchUsdGenStorm --cull` | **T2**, gate S-4. 49 tiles with authored `extent`, camera framing ¼ of the groom; asserts `itemsDrawn` drops ≥ 3× and that an unauthored extent culls nothing. |
| `benchUsdGenStorm --scrub` | **T2**, gate S-7. Parked-CV drag at fixed element count vs a real count change; asserts the parked frame is ≤ 1.5× a points-only frame and that `points.size() == Σ curveVertexCounts` holds throughout. |
| `testUsdGenMotionSamples` | The hdPrman contract of §7 without hdPrman: ordinality, `false` when static, the resolved sample ceiling (16 or 4, §7 rule 1), cache hits on the double `GetValue`. |
| `testUsdGenSessions` | SI-10. Two `UsdGenGroomSceneIndex` instances attached to one session (§3.7); asserts identical generation, prim set and frame for one commit, and that a renderer switch re-attaches and continues the generation counter. |
| `testUsdGenPruningCost` | SI-9. One commit's worth of surface pulls through the private pruning wrapper on a production-density skinned scalp; asserts one pull per `(surfacePath, surfaceGeneration)` and records the cost (§3.5). |

Every name above resolves in the CTest registry (`10-build-dependencies-testing.md` §5.6), so
`ctest -L T1` runs them and §6.5's gate → test map holds.

M1's stop condition stands (ADR §7): if precise `usdGen:*` invalidation cannot be demonstrated by
SI-2 and SI-7, fall back to `primvars:usdGen:*` — which *is* measured to dirty precisely
(`*DIRTY /World/Clump {'primvars/usdGen:rampKnots'}`, probe2 C) — and re-plan.

---

## 11. Out of scope

* **hdGp procedurals.** Rejected as the vehicle (S6). usdGen may later host its own resolver for
  third-party procedurals; that is v3.
* **An OpenExec/ExecUsd backend.** The control plane is not plugin-extensible in 26.08 (hard-coded
  adapter list, `pxr/usdImaging/usdExecImaging/adapterRegistry.cpp:29-52`). Curve arrays never go through
  OpenExec (S16). A future adapter behind the same operator interface is v3.
* **A generator scene index inserted at `overridesSceneIndexCallback`.** Letting UsdImaging do the
  prototype re-rooting is attractive (`research/G-instancing-cards-archives-and-native-instances.md`
  §3) but needs an application-controlled construction path that stock usdview does not offer.
  usdGen synthesizes instancers downstream instead (S33).
* **Automatic pickup of an overwritten map file.** Nothing invalidates on a file overwrite;
  `stage->Reload()` does not (MEASURED). `UsdGenImaging_ReloadMaps()` is the explicit action (S13).
* **Per-instance material binding.** Storm has none; material variety is multiple prototypes (S33).
* **CV highlight through Hydra selection.** Not reachable from usdview; selection state is a colour
  (S40).
* **The in-place `points` overlay on a frozen prim.** It does not exist in v1: `usdGen:output` is
  dropped everywhere (ADR §9 R8) and the optimisation is v2/M8 behind gate **S-10** (R29, R38, R40).
  The description always publishes tiles (`05-static-curves-and-deformation.md` §5.7).
* **Progressive/async generation.** The `asyncAllow`/`asyncPoll` hooks exist from M0 but the
  progressive generator lands in M8 (ADR §7).
* **Any renderer-specific scene index of usdGen's own.** One binary, one registration, all renderers
  (S2).

---

## 12. Sources

**Design.** `design/adr-v1.md` §1 (amendments), §2.1–2.3 (types and properties), §3 (contracts C1–C5),
§4.3–4.5 (commit triggers, initial population, sessions), §5.1–5.6 (registrations, invalidation, the
tile contract, `hairTangent`, refineLevel, material binding), §7 (milestones and gates), §8 (document
map), and **§9 (the addendum)** — R1–R6, R12–R13, R15, R19–R21, R24–R25, R28–R32, R34–R35, R37,
R38–R43 and R45, each cited where it applies (§0.2). `design/brief-v1.md` §2.1–2.9 (S1–S46).

**Proposals.** `design/proposal-performance.md` §6.1–6.6 (the imaging library — primary source;
§6.4 `UsdGenSurfaceResolver`, §6.5 the live-override store — shipped as `UsdGenLiveOverrideStore`,
`01-architecture.md` §5.1),
§5.4–5.5 (dirty propagation and the commit), §3.3 (registrations), §11.2 (gates).
`design/proposal-risk.md` §5.1 (plugInfo and `_IsEnabled`), §5.2 (classes), §5.3 (contract C2), §5.4
(surface resolution and the hard diagnostics), §5.5 (the publish path), §8.2 (the C ABI).
`design/proposal-artist.md` §5.2–5.3 (the scene index and `__usdGenRender`), §5.7 (the two extent
channels).

**Judges.** `design/judge-evidence.md` §0 (API verification table; the `hd:sceneGlobals` and
render-context findings), §2.1 (adapter coverage, auto-applied mask), §2.3 (S5 restored, `hairId` as
float), §2.4 (the non-const overrides). `design/judge-delivery.md` §4.1 (the `hairTangent`/fastpath
conflict), §5 (notice emission from the backstop, initial population, session identity).
`design/judge-artist.md` (legibility of the published layout).

**Research.** `research/A2-usdrig-imaging.md` §2, §3.4, §5, §6, §9. `research/A4-openusd-hdgp-adapters.md`
§2.1–2.5, §4.1–4.2, §5. `research/A5-storm-curves-shading.md` §10, key facts.
`research/G-chain-order-probe.md` §2, §3, §4a–4c, key facts. `research/G-stage-free-parameter-and-time-transport.md`
§1–§5, §7, key facts. `research/G-evaluation-scheduling-and-batching.md` §4–§9.
`research/G-storm-throughput-and-prim-granularity.md` §1.3–1.9, §1.11, §2, key facts.
`research/G-storm-hair-look-prototype.md` §5. `research/G-motion-blur-sampling-strategy.md` §4–§6.
`research/G-hdprman-and-usdrecord-render-time-chain.md` §2 (§2.2 the all-renderers-first tie-break),
§3, §5, §8, §9.
`research/G-instancing-cards-archives-and-native-instances.md` §5–§8, key facts.
`research/G-tool-loop-array-transport-and-cv-picking.md` §1.2, §3, §5.
`research/G-freeze-bake-undo-and-frozen-reentry.md` §2.2–2.3. `research/ENVIRONMENT.md` (with the
CORRECTIONS block).

**Sibling plan documents this one defers to.** `02-schema.md` §2 and §6 (the normative property
registry and the property→locator table), §7.1–§7.2 (the `usdGenSchema` library and
`UsdGenSchema_ComputeDescriptionExtent`); `03-execution-engine.md` §5.1–§5.2, §6.1–§6.3, §9.2 (the
router, the generation handoff, tile assembly, the six `TfDebug` codes and the trace scopes);
`08-tools.md` §1.1 (the `usdgen` package and the registered container type), §1.4 (contract C4);
`09-performance-and-benchmarks.md` §0.2 (the only frame ledger), §5 (the gate registry), §6.1–§6.2
(the HUD line and the trace-scope table); `10-build-dependencies-testing.md` §1.2, §3.1 (C++17),
§3.5 (the env-var registry), §4.3–§4.4 (the schema plugInfo rewrite and the shipped plugInfo
entries), §5.1, §5.6, §6.5, §7.2 (the six `TfDebug` codes);
`appendix-A-evidence-ledger.md` §2.4 (EV-030, EV-031), §2.9 (EV-075), §2.11 (the DERIVED and
ASSUMPTION provenances), §3.10 (`pxr/exec/esfUsd/stageData.cpp:361`).

**Prototypes carried in.** `prototypes/chain-order/` (becomes `testUsdGenChainOrder`),
`prototypes/stage-free-transport/` (probe1/2/3/5/6 become `testUsdGenAdapter` and
`testUsdGenRestAdapter`),
`prototypes/evaluation-scheduling/` (`models.cpp` becomes `testUsdGenScheduling`),
`prototypes/instancing/` (`testUsdGenInstancer`), `prototypes/storm-hair-look/` (`eglctx.h` and
`bench_hair.cpp` become the T2 harness; `usdGenHairPreview.glslfx` is contract C5).

**OpenUSD 26.08** (`/home/burkard/work/OpenUSD`, tag v26.08). Every file:line above was re-verified by
grep in this tree while writing.
