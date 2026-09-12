# OpenUSD renderer integration patches

These patches target **OpenUSD v26.08** (`ee47c679a`). They are an in-progress
renderer integration, not an upstream-supported API or a completed GPU hair
renderer. Apply them only in a private source checkout. The shared SDK must
not be overwritten during development.

`0001-hdst-post-commit-publication.patch` adds a generic
`HdStResourceRegistry::AddPostCommitCallback` hook. It has no usdGen/CUDA
dependency and introduces no application mutex. Registration uses the normal
parallel Sync phase; producers must quiesce before Commit. Callbacks run on
the commit context after pending computations are submitted and cleared.
Callbacks registered by a callback wait for the next commit. Exceptions are
isolated, recursive Commit is rejected, and uncommitted closures are released
before destructor garbage collection without being invoked.

This hook is **not a GPU fence or an aggregate success result**. A client
must establish transfer completion, check every required computation, and
publish an entire matching generation only on success. Failed candidates
must never overwrite live ranges. Native rprim attachment must use
`HdStUpdateDrawItemBAR` to invalidate cached draw commands; callback state
must independently protect rprim lifetime, including removal before Commit.
The caller must retain the registry throughout Commit. A closure must not
own the registry itself (which would create an ownership cycle), destroy it,
or draw/reenter Commit from the callback.

## Isolated development validation

The harness rebuilds only HdSt against the matching SDK. It is intentionally
restricted to the reference Linux/OpenGL, MaterialX-on, Ptex-off configuration.
It exports the matching MaterialX definition to its test consumers, because
that definition changes the registry's public-header layout. The production
integration will require a consistent full OpenUSD/client rebuild, not a
binary swap into an unrelated process.

From the usdGen repository, substitute absolute paths below:

```sh
git -C /path/to/OpenUSD worktree add --detach /path/to/private/OpenUSD v26.08
git -C /path/to/private/OpenUSD apply --check /path/to/usdGen/patches/openusd/0001-hdst-post-commit-publication.patch
git -C /path/to/private/OpenUSD apply /path/to/usdGen/patches/openusd/0001-hdst-post-commit-publication.patch
git -C /path/to/private/OpenUSD apply --check /path/to/usdGen/patches/openusd/0002-hdst-gpu-basis-curves.patch
git -C /path/to/private/OpenUSD apply /path/to/usdGen/patches/openusd/0002-hdst-gpu-basis-curves.patch
git -C /path/to/private/OpenUSD apply --check /path/to/usdGen/patches/openusd/0003-hdst-gpu-curve-groups.patch
git -C /path/to/private/OpenUSD apply /path/to/usdGen/patches/openusd/0003-hdst-gpu-curve-groups.patch
git -C /path/to/private/OpenUSD apply --check /path/to/usdGen/patches/openusd/0004-hdst-gpu-curve-group-controller.patch
git -C /path/to/private/OpenUSD apply /path/to/usdGen/patches/openusd/0004-hdst-gpu-curve-group-controller.patch
cmake -S tests/storm-extension -B /path/to/private/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSD_INSTALL_DIR=/path/to/OpenUSD_26_08 \
  -DUSDGEN_PATCHED_OPENUSD_SOURCE=/path/to/private/OpenUSD \
  -DUSDGEN_CUDA_BUILD_DIR=/path/to/usdGen/build-codex \
  -DUSDGEN_RESOURCE_BUILD_DIR=/path/to/usdGen/build-codex
cmake --build /path/to/private/build -j 6
ctest --test-dir /path/to/private/build --output-on-failure
```

`USDGEN_CUDA_BUILD_DIR` is optional; without it, the generic publication and
native Storm fixture tests are built. With it, the CUDA publication and
provider tests are added after building usdGen's CUDA targets on the same
source revision and toolchain. CTest selects the private HdSt library only
for these test processes, even if the calling shell points at the original
SDK. Do not globally prepend this development library to an application's
environment.

`USDGEN_RESOURCE_BUILD_DIR` defaults to `build-codex`, but it must name an
already-built matching usdGen tree. The isolated plugin-order test consumes its
schema and imaging resource trees to discover the Groom scene-index plugin;
therefore the private-HdSt harness alone is not a standalone all-tests setup.

The generic test uses real registry allocation and `HdStUpdateDrawItemBAR`,
checking failed replacement, aggregate-offset draw-batch invalidation,
parallel registration, callback exception isolation, deferred callbacks,
recursive-commit rejection, and cancellation on destruction. The CUDA test
uses the production transfer bridge and GPU Width evaluation, checks whole
eight-channel publication, failed and superseded candidates, and redraws
retained old/new snapshots after source retirement; it reads framebuffer
pixels only. The direct provider helper separately reads GL index/count
oracles. The native fixture covers retained-scene-index/native BasisCurves
rendering; the separate production CUDA fixture covers CUDA-generation
rendering and replacement retention. Neither is live groom `Publish` wiring.

Live groom `Publish` still refuses device generations, and multi-tile atomic
device publication is not wired. Persistent asynchronous interop, motion,
culling performance, full SDK rebuild, and remaining plan gates are open.

## Renderer-neutral group API (patch 0003)

`0003-hdst-gpu-curve-groups.patch` is relative to `0001` and `0002`. It adds
only a private HdSt API for immutable renderer-neutral group candidates,
validated post-Commit ready providers, a registry-lifetime identity, and a
nonblocking mailbox. It registers its header/source in HdSt CMake.

The API neither observes scene indices nor installs scene data, callbacks, or
draw-item BARs. In particular, it does **not** implement the per-render-index
staging filter, renderer controller, frontend acknowledgement, or all-tile
publication policy. Those are a later integration layer and remain open.

Checkpoint (2026-09-11): core CUDA curve-index tests passed in 10 repeated
runs plus memcheck; nonbenchmark T0/T1 session/publication tests also passed.
Private Storm helper coverage validates dispatch count-word copying, malformed
count rejection, and conservative GPU-item visibility.
Native provider ingress was exercised with rejection handling. The isolated
native retained-scene-index fixture also rendered zero-count and positive-count
framebuffer cases; this does not validate live groom `Publish` or multi-tile
publication. GPU-count visibility is intentionally conservative and has no
performance claim.

Latest checkpoint: private Storm CMake build passed and all five selected
tests passed; the nonbenchmark T0/T1 suite passed 78/78. Abstract draw-count rendering produced 18 lit pixels for count
2 and 35 for count 4; a rejected candidate retained the exact finite
framebuffer. Production CUDA generation 0 rendered 50 pixels, a rejected
candidate retained the prior result, and generation 1 changed geometry/width
to 51 pixels. Native CUDA memcheck reported zero errors. Both patch
clean-apply and private reverse checks passed. A fresh initial-generation
regression passed in both Release and ASAN/UBSAN configurations. The ASAN/UBSAN
build instrumented private HdSt, bridge, and fixture/test translation units;
the linked usdGen core remained Release and leak detection was disabled, so
this is not a leak-check claim.

## Renderer-local group controller and staging bridge (patch 0004)

`0004-hdst-gpu-curve-group-controller.patch` is relative to `0001` through
`0003`. It adds the private HdSt controller, immutable per-member datasource,
per-render-index staging scene index and plugin, plus terminal-scene-index
wiring in `HdStRenderDelegate`. The terminal observer only snapshots and
queues controls. On the render-owned update path the controller prepares every
member and publishes its sole Ready or rejected result from the normal
post-Commit boundary. A rejected or superseded pending candidate cannot
invalidate a previously accepted provider.

The bridge is renderer-local: registry identities use a weak registry lease,
and a staging instance admits only results for its registry and rprim path.
It supports zero, one, or many members; it has no fixed tile-count floor and
does not perform GL work, scene-index notices, or application locking from the
terminal observer. The group plugin must be discovered through the private
HdSt library metadata only. Do not combine that metadata with the stock HdSt
plugin metadata in one test process, since that loads two registrations of the
same HdSt types.

The reentry regression first reproduced stale old scene data/generation 1
after a throwing observer (`RED719243`); the staging exception-recovery
fix is included in this patch. The final selected seven targets passed ten
consecutive Release repetitions (10.72 seconds, root run `350e75`), and the
same seven passed once under ASAN/UBSAN (`a481ed`) with leak detection disabled.
That sanitizer build instruments private HdSt, bridge, and test translation
units only; stock OpenUSD dependencies and the usdGen CUDA core remain Release.
The latest native SDK memcheck and InitCheck each reported zero errors
(`84da55` and `a93649`).

The isolated plugin-order test passed under both Hybrid and JsonMetadataOnly
chains, observing a 17-node chain with Groom before staging before pruning.
The native CUDA group-publication test passed on EGL CUDA device 0, including
exact framebuffer/material/generation/membership retention on rejection,
recovery, and an empty group. A fresh original `ee47c679a` archive with 0001
through 0004 sequentially applied produced all 13 modified HdSt files exactly
equal to the compiled private tree (`09459a`). A fresh full-harness rebuild
passed, then its 13-test run had 12 passes and one failure with no skips
(`8de0cc`): the known old bare-provider
`testUsdGenCudaNativeBasisCurves` still retained 0 rather than 50 pixels and
leaked `/Looks/Accepted` to `/Looks/Rejected`. The new group-publication test
passed. The standard full `ctest --output-on-failure` command above therefore
intentionally exposes that known failure; it is not a green-suite claim.

Local Qwen contributed portions of the member/control helpers and the
reentrant-observer test. Coordinator/root integrated and corrected the APIs
and authored exception cleanup from the root reproducer; Qwen is not credited
with that cleanup. Hivemind produced no usable current visible answer, so
coordinator review supplied the fallback reasoning. These results do not make
the old bare-provider regression green: live groom `Publish`, the full
application frontend routing, and all-tile atomic live publication remain
unwired. A new frontend helper is unbuilt and outside this checkpoint.

After configuring and building as above, a focused check is:

```sh
ctest --test-dir /path/to/private/build --output-on-failure -R \
  'testUsdGenStormGpuGroup|testUsdGenCudaGpuGroupPublication'
```

## Native-provider and draw-count contract (isolated validation)

Artifact `002` contains this contract and has passed fresh baseline plus
artifact-application equivalence checks. The isolated native provider,
draw-count, and framebuffer fixtures are validated; live groom/device
publication and multi-tile integration remain pending. With a valid CUDA device
provider, Storm must use that provider as the authoritative geometry source;
there is no silent CPU fallback. Publication is transactional: only a
complete candidate bundle containing every required channel may replace the
visible bundle, and any failed channel leaves the previous bundle visible.

Post-commit callback state belongs independently to the pending candidate and
is invalidated on finalization/rejection. It must not own the registry, reenter
Commit, or outlive the candidate. The GPU draw-count packer produces the
actual `uint32` value `records * indexArity`, never a capacity value. Host
validation checks scalar metadata/arity/capacity; device validation checks the
upstream status and overflow. Generated geometry and draw counts remain
device-resident with no host readback.

Conservative visibility/culling behavior is intentionally conservative and
has no performance claim. The framebuffer result above is from the isolated
native fixture, not a claim that live groom `Publish` is complete.
