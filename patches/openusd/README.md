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
cmake -S tests/storm-extension -B /path/to/private/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSD_INSTALL_DIR=/path/to/OpenUSD_26_08 \
  -DUSDGEN_PATCHED_OPENUSD_SOURCE=/path/to/private/OpenUSD \
  -DUSDGEN_CUDA_BUILD_DIR=/path/to/usdGen/build-codex
cmake --build /path/to/private/build -j 6
ctest --test-dir /path/to/private/build --output-on-failure
```

`USDGEN_CUDA_BUILD_DIR` is optional; without it, only the generic publication
test is built. With it, build usdGen's CUDA targets first on the same source
revision and toolchain. CTest selects the private HdSt library only for these
test processes, even if the calling shell points at the original SDK. Do not
globally prepend this development library to an application's environment.

The generic test uses real registry allocation and `HdStUpdateDrawItemBAR`,
checking failed replacement, aggregate-offset draw-batch invalidation,
parallel registration, callback exception isolation, deferred callbacks,
recursive-commit rejection, and cancellation on destruction. The CUDA test
uses the production transfer bridge and GPU Width evaluation, checks whole
eight-channel publication, failed and superseded candidates, and redraws
retained old/new snapshots after source retirement. Only framebuffer pixels
are read back. These are not native BasisCurves scene-publication tests.

Remaining: immutable external geometry provider, safe rprim pending-bundle
lifetime, GPU topology/index consumption and draw counts, coherent shader/
extent/motion/culling updates, persistent asynchronous interop and full
renderer/performance/release validation.
