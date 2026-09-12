# G — CUDA group publication staging filter

## Scope and source boundary

This note specifies the first renderer-private bridge for CUDA device
generations. It does not change the public Groom publisher yet.

`UsdGenGroomSceneIndex` currently owns cooking and synthetic publication. Its
visible result is queried from an immutable snapshot in `GetPrim` and
`GetChildPrimPaths`; its `Synchronize`/`asyncPoll` boundary drains queued
publication notices. The current namespace is
`<description>/__usdGenRender/tile_NNNN`. The group bridge changes the CUDA
route so Groom publishes only a renderer-neutral group candidate/control on
that render-path scope. It does not expose candidate member BasisCurves or
their material bindings upstream of acceptance.

The candidate contains immutable value presentation for each intended member,
the external provider, token topology, local range plus matrix, ticket, and
generation. The private HdSt group API validates these values and converts an
accepted member bundle to an rprim- and registry-qualified cached-ready proxy.
No raw geometry, topology indices, or draw counts cross the candidate/control
boundary.

## Placement

The shipping renderer registration must build this order, from upstream to
downstream:

```
UsdImaging / renderer input
  -> UsdGenGroomSceneIndex
  -> HdStBasisCurvesGpuGroupStagingSceneIndex   (new, per render index)
  -> HdsiUnboundMaterialPruningSceneIndex
  -> remaining HdSt chain / RenderIndex
```

This is not merely a conceptual order. Groom is registered for all renderers
at phase 0. The registry resolves all-renderer phase-0 entries before
GL-specific phase-0 entries, so register the shipping staging plugin for GL at
phase **0**, `InsertionOrderAtEnd`: it follows Groom's all-renderer phase-0
entry and precedes Storm's phase-1 render-pass pruning and phase-900
unbound-material pruning. Do not move it to an arbitrary late phase.

The shipping `plugInfo` ordering needs its own tag, for example
`hdSt:gpuCurveGroupStaging`, with `after` constraints on `usdGen:groom`,
`hdSt:phase0`, and `hd:sceneGlobals`, and `before` constraints on
`hdSt:phase1` and `hdGp:proceduralResolution`. It must not self-tag as
`hdSt:phase0`. Verify the resolved order under both Hybrid and
JsonMetadataOnly policies; the latter cannot rely on C++ insertion order.

For the first native integration it is acceptable to wrap explicitly:

```
staging = HdStBasisCurvesGpuGroupStagingSceneIndex::New(groom);
terminal = HdsiUnboundMaterialPruningSceneIndex::New(staging, pruningArgs);
```

The native test must still prove this explicit order. Shipping requires the
registered GL-specific phase-0 placement and the real renderer chain-order
assertion.

## State and ordering contract

The filter owns, per render index:

- `accepted`: last complete tile map, immutable scalar presentation, and only
  cached-ready registry-scoped proxies;
- `pending`: one candidate/control snapshot and mailbox, never visible as
  BasisCurves downstream;
- a renderer-owned registry identity with weak lease; and
- serialized ticket/generation admission state.

On a new candidate, replace/cancel only the prior pending candidate. Keep the
entire accepted map visible. During the next ordinary HdSt Sync/Commit, the
controller requests Curves, Hull, and Points for every pending member,
including members that will be invisible. The post-Commit callback may only
make the group Ready/Rejected result and post it to the mailbox. It must not
send scene-index notices, call into Groom, or wait.

At an external poll boundary, reject mailbox entries unless all of group path,
ticket, generation, mailbox lifetime, and registry identity liveness still
match the pending state. Admit one terminal result at most once. A Ready result
must have a provider for every pending member and each proxy must be a
factory-created cached-ready proxy. Then atomically replace `accepted` with
the whole snapshot and send one coherent add/remove/dirty publication set.
Only after that next normal Sync can each rprim receive matching scalar
presentation and already-ready GPU BAR bundle.

Failure, a false/throwing `Ready`, stale ticket, canceled mailbox, incomplete
membership, or an invalid/expired registry identity discards `pending` and
retains `accepted` unchanged. No per-rprim partial install is permitted.

Scope/groom removal is different: cancel pending work **and immediately remove
the accepted tile map**. Removal of a member that belongs only to a pending
candidate is deferred with that candidate: it is never visible and causes the
candidate to reject or be superseded, not a visible remove notice.

## Poll boundaries

For a direct `HdEngine` native path, call the staging filter's nonblocking
`Poll()` immediately before `HdEngine::Execute`; that is the sole place it may
admit mailbox results and emit notices. Do not poll from an rprim `Sync`, a
post-Commit callback, or `GetPrim`.

For UsdImaging, deliver `HdSystemMessageTokens->asyncPoll` through the existing
scene-index chain. The staging filter consumes that message and performs the
same nonblocking `Poll()` outside `SyncAll`; `UsdGenGroomSceneIndex` already
uses `asyncPoll` as its render-thread publication flush. `asyncAllow` enables
the asynchronous route but is not an admission callback.

## Required integration tests

1. Build the actual native chain and assert node order: Groom upstream of
   staging, staging upstream of unbound-material pruning. Start with an
   accepted multi-member group whose material prim is visible downstream.
2. Stage a changed multi-member candidate (geometry, matrix, visibility,
   material) and force one provider Ready failure. Poll then render: the exact
   old framebuffer, old material binding, and old tile membership remain; the
   candidate material was never exposed for pruning.
3. Recover with every member Ready. One pre-Execute poll emits the full
   replacement; the following Sync renders the new scalar snapshot and all
   matching proxy bundles. Assert every member changed together, not merely a
   pixel count.
4. Exercise zero-member completion/removal, one member, and a 32+ member
   group; include an invisible member so all-members preparation is verified.
5. Exercise stale ticket, cancellation, throwing Ready, upstream scope/groom
   removal before poll, and registry invalidation before mailbox admission.
   Each retains old accepted output except scope/groom removal, which removes
   it immediately. Verify one terminal admission/notice set per ticket.

This test plan intentionally does not claim a completed shipping filter or
all-tile renderer atomicity; it defines the acceptance boundary those changes
must satisfy.
