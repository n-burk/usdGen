# M1 compile lane — notes (2026-07)

Lane goal (m1-compile): make the whole tree build and keep the 13 M0 tests green.
All 54 census errors are fixed; clean rebuild of all 78 targets exits 0;
`ctest -L '^T[01]$'` → 13 M0 passed, 13 M1 stubs Skipped, 0 failed.

## CMake/CTest wiring change (recorded per lane rules)

`CMakeLists.txt` (M1 test block, ~line 386-456):
- CMake/CTest 3.28.3 on this host does **not** honor `PASS_RETURN_VALUES "77"`
  or `NOT_RUN_RETURN_VALUES "77"` (both verified with isolated repros — the test
  still reports `***Failed`). CTest ≥3.29 would honor `NOT_RUN_RETURN_VALUES`.
- Fix used: every M1 stub test gets `SKIP_RETURN_CODE "77"` (supported in 3.28,
  reports the test as **Skipped**, "0 tests failed"). `NOT_RUN_RETURN_VALUES "77"`
  is kept alongside so CMake ≥ 3.29 reports "Not Run" instead.
- No test names, labels, env or commands were changed — only properties.

## API fixes applied (OpenUSD 26.08 gotchas, confirmed against
$USD headers)

- `TfToken` has `Hash()` (returns `size_t`) — not `GetId()` and not `GetHash()`.
  Fixed in compiler.cpp:54, ops/grow.cpp:130, ops/scatter.cpp:134,
  ops/length.cpp:137-138.
- `VtValue::GetHash()` **does** exist (pxr/base/vt/value.h:1308) — the
  `GetValueHash()` calls in compiler.cpp:92,414 became `GetHash()`.
- `GfNormalize3f` is **not** declared in the 26.08 installed headers — use
  `GfGetNormalized(v)` / `v.GetNormalized()` (pxr/base/gf/vec3f.h:377).
  Fixed ops/scatter.cpp:207.
- `UsdGenChunkView::width` / `::hairT` were `const float*` but ops write their
  own planes through them (scheduler binds the node's output buffer when the
  op's `PlanesTouched()` includes that plane). Changed to writable `float*`
  (curveBuffer.h:90-93), mirroring px/py/pz.

## Compile repairs (files changed)

- graph.h: duplicate `descIdx` member removed (kept the graph-Desc().nodes one).
- graph.cpp: `UsdGenGraph::UsdGraph()` typo removed entirely — the header
  already has an in-class `= default`, so the out-of-line definition was both
  mistyped and illegal.
- ops/{grow,scatter,width,length,noise}.h: added the missing
  `CreateCapture() const override;` / `PlanesTouched() const override;`
  declarations (definitions existed in the .cpp but no header declaration →
  "no declaration matches").
- compiler.cpp: string-literal + char* concatenations wrapped in `std::string(...)`
  (4 sites); `OldNode &old` (was `const &`) so the E-6 reuse path can
  `std::move` capture/buffer/chunks out of the old graph.
- scheduler.cpp:120: `TfToken::Equals` → `operator==`; removed two broken
  generic lambdas in `EvaluateChunk` (`VtArrayBase` undeclared, missing
  `typename`, stray `)`); undefined `px_or_null(view)` → `view.px`;
  `view.inPx + cd.firstCv` → `view.inPx` (inPx already offset by
  `upFirstCv` in `fillIn` — the extra `cd.firstCv` was a double offset).
- mask.cpp: deleted the duplicate second definition of
  `UsdGenMaskSettingsFromParams(UsdGenParamView const&)` (kept the first,
  canonical one at line 151).
- ops/noise.cpp:331: `const float mi = int(...)` → `const int mi` (float
  array subscript → `invalid types 'const float*[const float]'`).
- session.cpp / usdGenDirtyRouter.cpp / usdGenImagingSession.cpp: no direct
  edits — they only failed transitively through graph.h.

## Semantic (non-compile) defects NOT fixed in this lane — flag for the
implement/lane owners

1. **width.cpp:221 — `inWidth = view->width` reads the op's OWN output plane.**
   `UsdGenWidthOp::PlanesTouched() == kPlaneWidths`, so the scheduler binds
   `view->width` to this node's freshly zeroed/aliased width buffer. The
   `replace == false` path (`w *= inWidth[o]`) therefore multiplies by the
   node's own output (0.0 when freshly allocated) instead of the upstream
   width. `UsdGenChunkView` has no `inWidth` field at all — the kernel
   contract needs an upstream-width read port (analogous to `inPx`) or width
   must be passed via `inF` slots.
2. **scheduler.cpp `EvaluateChunk` envelope block is a no-op stub.**
   `if (op->IsGenerator() || ctx.params == nullptr) { /* no envelope */ }`
   does nothing; the blend is then applied unconditionally when
   `0 < blend < 1` AND `up->buffer.px.size() == buf.px.size()`.
   - Per 03 §8.5, `w <= 0` must alias the input (copy in→out), which is not
     implemented: a non-generator with blend 0 leaves its px buffer empty/zero.
   - Generators are NOT excluded from blending (the no-op if was meant to be).
   - Only the px plane is blended; width/hairT/extra planes are not.
   - `TF_UNUSED(view)` leftover at the end of the function.
3. **scheduler.cpp generator short-circuit hard-codes the type string**
   `type == "UsdGenGrow"` instead of using `UsdGenCapture::OwnsBuffer()`;
   any future generator that does not write points (e.g. a CurveCount op
   like scatter, which relies on capture) is skipped — that part works, but
   the string check will break/drift if the type token changes.
4. **compiler.cpp E-6 reuse: only `UsdGenScatterCapture` overrides
   `Clone()`; all other op captures return `nullptr`** from the base, so
   digest-stable nodes are rebuilt instead of reused. E-6 is therefore
   effectively scatter-only; the deterministic-capture test
   (testUsdGenKernelDeterminism, M1) will not exercise reuse for grow/width/
   length/noise.
5. **LUT sizes are consistent (257)** across mask.cpp (`kRampLutSize`),
   width.cpp and noise.cpp (`kLutSize`) — verified, no defect.
   `mask.cpp MaskWeight` indexes `rampLut[j+1]` with `j <= 255` → max 256, in
   range for 257 entries.
6. **`UsdGenGraph().Output()` temporary in scheduler** (empty-graph fallback
   when a node has no input) constructs a throwaway graph per chunk; cheap
   now but will matter at E-7 scale.
