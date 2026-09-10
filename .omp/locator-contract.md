# Locator-contract ruling + migration packet: attribute/relationship collision

Date: 2026-09-09. Status: DESIGN-CONTRACT verdict (read-only investigation; no code edited).
Question: what locator layout does the PLAN mandate for `float usdGen:length` vs
`rel usdGen:length:source` (and siblings), and what minimally-compliant migration makes
`UsdImagingDataSourceMapped` happy while preserving the 02 §6 / R25 router contract?

## 1. Verdict: the plan MANDATES the colliding layout (with citations)

The collision is not an implementation accident. Three normative plan statements compose it:

- `plan/02-schema.md:145` (§0.7 "Grouped parameters"): "`UsdGenPrimAdapterBase` splits each schema
  property name on `:` and hands `UsdImagingDataSourceMapped` an `HdDataSourceLocator` of that many
  elements … `usdGen:mask:ramp:knots` → `usdGen/mask/ramp/knots`." The rule is total over the
  property name: `usdGen:length` → `usdGen/length`, `usdGen:length:source` → `usdGen/length/source`.
- `plan/06-imaging.md:563-573` (§2.2 rule 4): "`_LocatorFor` strips the leading `usdGen:` and splits
  the remainder on `:`, producing `clump/size`, `frozen/mode`, `sculpt/deltas`" and the
  `PropertyMappings` constructor prepends the `usdGen` prefix. No carve-out for relationships.
- `plan/02-schema.md:1781` (§6.1 graph-structural rows) quotes the collided locator verbatim as a
  router prefix: "`usdGen/terminal`, `usdGen/guides`, … `usdGen/clump/centers`, `usdGen/length/source`,
  `usdGen/direction/source`, …". The router table therefore EXPECTS `usdGen/length/source` to exist.

So the mandated layout puts attribute `length` at `usdGen/length` and relationship `length:source`
at `usdGen/length/source` — an ancestor/descendant pair — on the same mapped container.

### 1.1 Why that pair is illegal in OpenUSD 26.08 (exact semantics)

`_FindOrCreateChild` (`/home/burkard/work/OpenUSD/pxr/usdImaging/usdImaging/dataSourceMapped.cpp`,
"Adding data source locator when there was already an ascendant locator added for a Usd attribute
with name '%s'") `TF_CODING_ERROR`s and returns `nullptr` when a new mapping's intermediate element
walks through an already-registered LEAF. Whichever of (`length`, `length/source`) registers second
fails: leaf-then-descendant hits the ascendant guard; descendant-then-leaf inserts a duplicate
`hdNames` entry over an existing container node, corrupting `Get`/invalidation. The current
`LocatorForProperty` (`libs/usdGenImaging/usdGenImaging/primAdapter.cpp:213-243`) implements exactly
the mandated rule (comment at `:216-219` cites 02 §0.7), and `Mappings()` (`:165-211`) feeds both
sides of every collision into one `PropertyMappings`, so construction emits the coding error, the
container is unusable, and testUsdGenStormSurgery publishes generation 0.

### 1.2 Verified collision inventory (schema-declared, all from `libs/usdGenSchema/schema.usda`)

Attribute leaf `usdGen:X` (or `usdGen:N:X`) + attribute/relationship `usdGen:X:*` on the SAME prim
type, hence the same mapped container:

| Type | Leaf | Descendant(s) | schema.usda lines |
|---|---|---|---|
| `UsdGenGrow` | `float usdGen:length` | `rel usdGen:length:source` | 202, 211 |
| `UsdGenGuideInterpolate` | *(inherits length block)* | `rel usdGen:length:source` | 236 |
| `UsdGenLength` | `float usdGen:length:value` | `rel usdGen:length:source` | 346, 351 |
| `UsdGenDirection` | `vector3f usdGen:direction` | `rel usdGen:direction:source`, `float2[] usdGen:direction:knots` (+interp) | 387, 395–397 |
| `UsdGenWidth` | `float usdGen:width` | `float2[] usdGen:width:knots` (+interp) | 367–369 |
| `UsdGenScale` | `float usdGen:scale` | `float2[] usdGen:scale:knots` (+interp) | 446–447 |
| `UsdGenBend`-family (`radius`) | `float usdGen:radius` | `float2[] usdGen:radius:knots` (+interp) | 467–469 |
| `UsdGenBend`-family (`angle`) | `float usdGen:angle` | `float2[] usdGen:angle:knots` (+interp) | 489–491 |
| `UsdGenNoise` | `float usdGen:noise:magnitude` | `float2[] usdGen:noise:magnitude:knots` (+interp) | 320–323 |

Non-collisions that must NOT move (siblings under a shared container are legal): `clump/size` vs
`clump/centers` (283–285), `displace/amount` vs `displace/map` (519–520), `mask/amount` vs
`mask/source` (`plan/02-schema.md:870-871`). Any fix must be scoped to true ancestor/descendant
pairs, not to shared-prefix siblings.

### 1.3 What R25 does and does not say

- R25 (`design/adr-v1.md:383`): "The dirty-router key is the **full locator path** below `usdGen/`
  (never 'first two elements'); the table is generated per property from 02 §6."
- `plan/03-execution-engine.md:1048-1060` (§5.2 hop 1) + `plan/02-schema.md:1764-1769` (§6 intro):
  prefixes are 1–4 elements deep, matched longest-first; truncating to two would route a clump-size
  slider as a structural recompile. `plan/01-architecture.md:727-729` repeats the "full path, never
  first two elements" keying.
- R25 constrains the ROUTER KEY (full path, longest-prefix match), never the LOCATOR SHAPE. The
  shape mandate comes from 02 §0.7 + 06 §2.2 rule 4 alone — and that shape is unimplementable, so
  the shape mandate (not R25) is what must give. The withdrawn `rel/…` branch-prefix ruling failed
  for the opposite reason: it moved every relationship to a new top-level branch, churning ALL
  relationship router keys and contradicting the verbatim 02 §6.1 prefixes, with no plan text behind it.

### 1.4 Gate-text audit (`plan/09-performance-and-benchmarks.md` §5.2) — zero amendments needed

- SI-1 (`:568`): "`points.size() == Σ curveVertexCounts`, hard assert" — no locator names quoted.
- SI-2 (`:569`): "exactly `primvars/points/primvarValue` + `extent/*` on the dirty tiles, nothing
  else" — tile-output locators only; operator-container shape unmentioned.
- SI-5 (`:572`): chain order only — unaffected.
- SI-7 (`:574`): "every `usdGen:*` property of all five registered types **and** `usdGen/rest/points`
  … appears **and dirties**" — names the SET, never the SHAPE. Any deterministic injective mapping
  keeps SI-7 green as written.
- 06 §2.5 `:643-649` and 02 §6.6 rule 1 (02 `:1874-1879`) restate SI-7 as "appears and dirties" —
  likewise shape-agnostic.

CONCLUSION on gates: the registry wording is inviolable and inviolate — no gate text needs an
amendment under the recommended migration. The ONE plan text that needs an amendment is
`02-schema.md` §6.1 `:1781` (the `usdGen/length/source`, `usdGen/direction/source` prefixes) plus
the shape rule at 02 §0.7 `:145` / 06 §2.2 rule 4 `:563-573`; both are flagged explicitly in §3.

## 2. Recommended migration: relationship-leaf suffix (ranked first)

Rule (one deterministic sentence covering all attr/rel pairs incl. future nesting):
**attribute locators are unchanged from the 02 §0.7 rule; a relationship whose 02 §0.7 locator
would be an ancestor-or-descendant of any attribute locator on the same prim type gets `-rel`
appended to its FINAL element only** (`usdGen:length:source` → `usdGen/length/source-rel`;
`usdGen:direction:source` → `usdGen/direction/source-rel`). Relationships with no collision keep
their 02 §0.7 locator (`usdGen/input`, `usdGen/mask/source`, `usdGen/clump/centers` — siblings are
legal). Ramp-knot attributes (`width/knots`, `noise/magnitude/knots`, …) are ATTRIBUTES and keep
their locators; the collision they participate in is resolved from the relationship side where one
exists, and where the pair is attr/attr (`width` vs `width/knots`) the LEAF-attr keeps its name and
the KNOT-attr keeps its (longer) name — that pair is equally illegal and is resolved the same way:
the descendant attribute keeps its full path, the ancestor attribute keeps its path, and the tie is
broken because the ancestor in every verified attr/attr case is a plain scalar that the evaluator
reads via the router prefix table, NOT via mapped-container `Get` at that exact leaf…

CORRECTION — that last sentence is wrong, and the attr/attr pairs need care: `width` (leaf,
`usdGen/width`) vs `width/knots` (descendant, `usdGen/width/knots`) collide in the container
regardless of who reads them. Suffixing only relationships does NOT fix attr/attr pairs. The
minimum-entropy rule that fixes BOTH with one function:

**FINAL RULE: within one prim type's mapping set, if a property's 02 §0.7 locator is a strict
ancestor of another property's 02 §0.7 locator, the ANCESTOR property's leaf takes a `-value`
suffix when it is an attribute (`usdGen/width` → `usdGen/width-value`) and a `-rel` suffix when it
is a relationship (`usdGen/length/source`… no — `length:source` is the DESCENDANT there).**

Simplest consistent form — suffix the SHORTER (ancestor) side, keyed by kind:

- ancestor attribute + descendant anything → ancestor becomes `<leaf>-value`
  (`width` → `usdGen/width-value`; `noise:magnitude` → `usdGen/noise/magnitude-value`;
  `direction` → `usdGen/direction-value`; `scale`, `angle`, `radius` likewise).
  Cost: attribute router keys churn for exactly the 7 collided leaves.
- ancestor attribute + descendant RELATIONSHIP (`length` vs `length:source` on Grow/GuideInterp;
  `length:value` vs `length:source` on Length — here `length:value` is NOT an ancestor of
  `length/source`; they are SIBLINGS under `length/`, legal, no change) → for Grow/GuideInterp the
  ancestor `length` is an attribute → `usdGen/length-value`, rel keeps `usdGen/length/source`.

Wait — that keeps `usdGen/length/source` verbatim (02 §6.1 `:1781` UNCHANGED for the rel rows) and
moves only attribute leaves. Re-rank against the assignment's criteria:

1. Zero gate-text amendments: satisfied (gates quote no operator-container names, §1.4).
2. Zero router-key churn for attributes: VIOLATED for the 7 collided leaves — but the alternative
   (suffix the descendant) churns the 02 §6.1-quoted rel prefixes instead. Attribute-leaf suffixing
   is still better: the churned keys are all VALUE-class sliders (02 §6.5) routed by exact-leaf
   match, while rel keys are STRUCTURAL graph edges (02 §6.1) quoted verbatim in the normative
   table. Fewer normative-table rows change (zero rel rows change; only §6.5's catch-all "every
   remaining leaf" needs no edit at all since it names no exact leaf for these).
3. Single pure function in `LocatorForProperty`: satisfied — but the function needs the sibling set
   (collision is a property OF THE TYPE's property set, not of one name). Signature change required
   (see pseudocode).

RANKING vs alternatives:
- (A, recommended) Ancestor-attribute `-value` suffix: 0 gate amendments; 0 rel-key churn; 7
  value-leaf keys churn; 1 pure function + per-type pre-pass. Attribute dirties stay exact-leaf
  (SI-2-adjacent precision preserved: `usdGen/width-value` dirties alone).
- (B) Descendant-rel `-rel` suffix: fixes attr/rel pairs only; leaves attr/attr pairs
  (`width`/`width/knots`) STILL ILLEGAL. Rejected — does not close the defect.
- (C) `rel/` branch prefix (withdrawn): fixes everything but churns ALL 17 relationship keys,
  contradicts every 02 §6.1 rel row, zero plan evidence. Rejected — do not reintroduce.

Under (A) the amended plan text is minimal: 02 §0.7 `:145` gains one sentence ("…except that a
locator which would be a strict ancestor of another mapped locator on the same prim type takes a
`-value`/`-rel` suffix on its final element; the router table in §6 lists the suffixed forms");
06 §2.2 rule 4 `:563-573` gains the same sentence; 02 §6.5's catch-all already covers the suffixed
value leaves with NO edit; 02 §6.1 `:1781` needs NO edit (rel rows untouched). Only attr/attr
ancestor leaves move, and those appear in §6.4/§6.5 rows that must list the suffixed spellings
(`usdGen/width-value`, `usdGen/noise/magnitude-value`, `usdGen/direction-value`,
`usdGen/scale-value`, `usdGen/angle-value`, `usdGen/radius-value`, Grow/GuideInterp
`usdGen/length-value`).

## 3. Migration packet

### 3.1 Exact function pseudocode (replaces `primAdapter.cpp:213-243`)

```cpp
// Returns the RELATIVE (container-local) locator for one property. `siblings`
// is the full GetPropertyNames() set of the same prim type (06 §2.2: the
// mapping is built per schema type name, so the set is in hand in Mappings()).
// Pure: same (property, isRel, siblings) -> same locator, no I/O.
static HdDataSourceLocator LocatorForProperty(
    const TfToken &property, bool isRelationship,
    const TfTokenVector &siblings)
{
    // 1. Base rule unchanged (02 §0.7): strip "usdGen:", split rest on ':'.
    std::vector<std::string> els = Split02(property);   // e.g. {length,source}
    // 2. Ancestor check against every sibling's BASE locator.
    for (const TfToken &sib : siblings) {
        if (sib == property) continue;
        std::vector<std::string> sels = Split02(sib);
        // property is a STRICT ancestor of sib  <=>  els == prefix of sels.
        if (els.size() < sels.size() &&
            std::equal(els.begin(), els.end(), sels.begin())) {
            els.back() += isRelationship ? "-rel" : "-value";
            break;  // one suffix suffices: no two suffixed forms collide
                    // (suffix only ever extends the ancestor side).
        }
    }
    return HdDataSourceLocator(els-as-tokens);
}
```

Notes: descendant-side properties NEVER change (longest-prefix router matching, R25, is
preserved); only strict-ancestor properties move. `IsSingleTarget` (`primAdapter.cpp:245-252`)
is untouched — factory choice is orthogonal to locator shape. The per-type sibling set is
`def->GetPropertyNames()` already iterated in `Mappings()` (`:170-172`), so no new USD calls.

### 3.2 Every consumer file to touch

| File | Change |
|---|---|
| `libs/usdGenImaging/usdGenImaging/primAdapter.cpp` (`:151-243`) + `primAdapter.h` (`:63-64`) | New `LocatorForProperty` signature + ancestor pass in `Mappings()` (build sibling vector first, then map). ONLY locator-construction site. |
| `libs/usdGenImaging/usdGenImaging/usdGenDirtyRouter.cpp` (`:32-44` `_ParamLocator`) | Routes via `LocatorForProperty` already — update the call to the new signature (pass `isRelationship` for the stripped name; sibling set = the node's compiled param-name set, or simpler: ask the adapter's cached `Mappings(type)` for the authoritative locator instead of recomputing). NO key-matching logic changes (longest-prefix + `Intersects`, `:189-206`, is shape-agnostic). |
| `libs/usdGenImaging/usdGenImaging/usdGenGraphDescBuilder.cpp` (`:149` strip `usdGen:`) | NO CHANGE (engine param names are pre-locator stripped names; unaffected by shape). Verify only. |
| `libs/usdGen/usdGen/compiler.cpp` (`ClassifyParamBits`, `TypeClassification`) | NO CHANGE (classifies stripped param tokens, never locators). Verify only. |
| `libs/usdGenImaging/usdGenImaging/restApiSchemaAdapter.cpp` + `usdGenRestApiDataSource.cpp` | NO CHANGE (emits fixed `usdGen/rest/*` locators; no attr/rel pairs under `rest/`). Verify only. |
| `libs/usdGenImaging/usdGenImaging/usdGenImagingSession.*` | NO CHANGE — audited (`usdGenImagingSession.cpp:1-272`): session store hashes/keys on stage/groom-root only; no router-key hashing anywhere in the file. |
| `plan/02-schema.md` §0.7 `:145`, §6.4/§6.5 value/capture rows for the 7 suffixed leaves | One-sentence suffix rule + respelled leaf rows (explicit amendment, §2). |
| `plan/06-imaging.md` §2.2 rule 4 `:563-573`, §2.3 table `:584-592` | Same one-sentence rule + suffixed example locators. |

Router/R25 regeneration: there is NO standalone generator script in-repo (`bin/`, `python/` contain
no `gen_router`/`gen_hair_stages` — R25's "generated … table" is realized as the runtime
`TypeClassification` cache, `compiler.cpp:235-251`, plus the longest-prefix table built in
`UsdGenDirtyRouter::Rebuild` from `node.paramRouting`). Regeneration step = REBUILD (recompile);
no manual-sync diff exists to maintain. The per-type op `TopologyParameters()`/`ValueParameters()`
lists (e.g. `libs/usdGen/usdGen/ops/noise.cpp:91-113`, `length.cpp:97-118`) use stripped names and
need NO edits — but any op whose collided leaf is in `ValueParameters()` keeps working ONLY because
`_ParamLocator` and the adapter share the one function (§3.1): add a debug assert that every
`paramRouting` name resolves to a mapped absolute locator.

### 3.3 Test files needing expectation updates (specific assertions)

1. `tests/testUsdGenAdapter.cpp` (SI-7; currently green-in-wave-6 claim is STALE — its
   `ExpectLocator` at `:73-77` round-trips through `LocatorForProperty`, so it is self-consistent
   under ANY rule and cannot catch the shape; `present == propsChecked` at `:189` is the assertion
   that FAILS today because the mapped container rejects the collision). Updates: (a) pin EXPLICIT
   expected locators for every Table-1.2 pair (e.g. `usdGen/length-value` present,
   `usdGen/length/source` present on Grow; `usdGen/width-value` + `usdGen/width/knots` on Width) —
   no more pure round-trip; (b) assert `errorMark.IsClean()` (`:261`) stays clean specifically while
   pulling a collided prim (the coding-error regression); (c) assert dirty-exactness (`:176-183`)
   for a collided leaf edit (`usdGen:width` → exactly `usdGen/width-value`).
2. `tests/testUsdGenRestAdapter.cpp` — CURRENTLY AN M1 STUB (`:1-3`, returns 77). Not an update but a
   fill-in, required by this packet because SI-7 names it (`09 §5.2:574`): serve a Mesh with
   `UsdGenRestAPI`, assert `usdGen/rest/points` reads `Default()` (not time-varying) and refusal
   (`nullptr` + warn) on a `GeomSubset` (R15; `restApiSchemaAdapter.cpp:17-43`,
   `usdGenRestApiDataSource.cpp:70-76`). Unchanged by the locator fix — listed so the packet's
   verification set is complete.
3. `tests/testUsdGenInvalidation.cpp` (SI-2 engine half) — NO locator assertions exist (engine
   stripped names: `noise:magnitude` at `:113`, `segments` at `:205-214`); NO EDITS. Keep as the
   shape-independent router-behavior guard. (`tests/testUsdGenChainOrder.cpp` likewise untouched —
   chain order is locator-shape-independent.)

### 3.4 Verification command list (explicit targets; run from `build-schema/`)

```
ninja testUsdGenAdapter testUsdGenRestAdapter testUsdGenChainOrder testUsdGenInvalidation testUsdGenStormSurgery
ctest -R 'testUsdGenAdapter|testUsdGenRestAdapter|testUsdGenChainOrder|testUsdGenInvalidation|testUsdGenStormSurgery' --output-on-failure
```

Pass criteria: Adapter `present == propsChecked` with `errorMark` clean (mapped container builds —
the generation-0 root cause gone); RestAdapter real assertions green; ChainOrder/Invalidation
unchanged-green (no shape dependence); StormSurgery publishes generation > 0 and monotonic stamps.

### 3.5 Risk note: what this migration could break in M3+

- Instancing `primOrigin` (T-INST-1, `09 §5.4:613`): pick round-trip resolves through `primOrigin`,
  not the `usdGen` container — unaffected, BUT `UsdGenInstance` params rebase `usdGen:surface`
  inside propagated prototypes; if a future instanced type declares its own attr/rel pair, the
  ancestor pass runs per TYPE (`Mappings` is cached per schema type name, `primAdapter.cpp:153`),
  so prototype-scoped mappings stay correct. Watch item only.
- REST-API mesh locators: `usdGen/rest/*` shares the `usdGen` container prefix on MESH prims via a
  SEPARATE adapter (`restApiSchemaAdapter.cpp`); today no `rest:*` schema property collides with it.
  If the schema ever declares `usdGen:rest:*` operator properties, the ancestor pass (which sees
  only the prim type's own `GetPropertyNames()`) will NOT see the API adapter's fixed locators —
  reserve `rest` as a no-declare namespace or extend the sibling set. Flagged, no action today.
- Any downstream consumer that string-matches `usdGen/width` / `usdGen/length` exactly (debug HUD,
  `usdgen` Python surfaces, goldens) must accept the `-value` spellings; grep for quoted
  `usdGen/(width|length|direction|scale|angle|radius|noise/magnitude)"` outside §6 tables before
  landing.
- Overlaid upstream prims (S5/S30, 06 §3.4.1/§5.1): the suffix rule does not change overlay
  discipline (`ComputeDirtyLocators` + bare `primvars`), but re-run SI-2's overlaid-prims variant
  at M2 (`09 §5.2` M2 row) since invalidation SETS shift shape.
