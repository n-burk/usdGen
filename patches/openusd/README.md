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
cmake -S tests/storm-extension -B /path/to/private/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSD_INSTALL_DIR=/path/to/OpenUSD_26_08 \
  -DUSDGEN_PATCHED_OPENUSD_SOURCE=/path/to/private/OpenUSD \
  -DUSDGEN_CUDA_BUILD_DIR=/path/to/usdGen/build-codex
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
