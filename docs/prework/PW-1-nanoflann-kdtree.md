# PW-1 — E-4 pre-work: nanoflann KD-tree capture-cost check

## 1. What was asked

Roadmap `plan/11-roadmap.md` §1.1, PW-1 row (line 109):

> | **PW-1** | E-4 | the M3 capture schedule for `UsdGenGuideInterpolate` and `UsdGenClump` | vendored nanoflann 1.12.1 (S38), `KDTreeSingleIndexAdaptor<L2_Simple_Adaptor<float, Cloud>, Cloud, 3>` — exact template arity **UNVERIFIED**: nanoflann is not on this host and `research/A8-seexpr-ptex-libs.md` §5 quotes the upstream README, so PW-1's first step is to vendor the header and confirm the instantiation compiles. k = 3 over 4 000 guide roots for 100 k and 1 M rest roots at 1/4/8/20 threads: ≤ 25 ms at 100 k on 8 threads and linear in roots; > 300 ms at 1 M triggers SC-6. Today's only figure, `design/proposal-performance.md` §5.6's "~10 ms", is an ASSUMPTION (`design/judge-delivery.md` §5 item 6) |

Gate E-4 (`plan/09-performance-and-benchmarks.md` line 558): *nanoflann kNN capture, 100 k / 1 M rest roots, 4 k guides, 8 threads — ≤ 25 ms at 100 k and linear in roots (ASSUMPTION). Stop: > 300 ms at 1 M ⇒ SC-6. M0 pre-work (PW-1), binding M3. Status before this run: UNMEASURED — the only capture term with no number.*

## 2. Method + exact commands

Vendored header: `thirdparty/nanoflann/nanoflann.hpp`, `NANOFLANN_VERSION_STRING "1.12.1"` (line 93).

Probes: `docs/prework/probes/PW-1/` (`compile_probe.cpp`, `bench_knn.cpp`, `Makefile`).

```bash
# compile probe — exact roadmap instantiation
g++ -std=c++17 -O3 -DNDEBUG -w -I /home/burkard/work/usdGen/thirdparty/nanoflann \
    compile_probe.cpp -o compile_probe -lpthread
./compile_probe

# benchmark — matches the repo's Release flags (CMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG")
g++ -std=c++17 -O3 -DNDEBUG -w -I /home/burkard/work/usdGen/thirdparty/nanoflann \
    bench_knn.cpp -o bench_knn -lpthread
./bench_knn 7   # run 1
./bench_knn 11  # run 2
```

Or `make run` (default 8 iterations) in the probe directory.

Design:
- Dataset: **4 000 guide roots** uniform in [0,100)³ (seed 9001). KD-tree rebuilt each iteration, so "total" = build + all queries (the full capture step).
- Queries: **k = 3** nearest guides for each of **100 000** (seed 777) or **1 000 000** rest roots.
- Parallelism: fixed `std::thread` pool of **1 / 4 / 8 / 20**; contiguous partition of the query range, one worker per slice; no shared mutable state (nanoflann `knnSearch` is `const`/read-only, header threading note lines ~1825–1832).
- **7 and 11 measured iterations** per configuration (+1 warm-up); median and p95 (nearest-rank) of the totals.
- Anti-DCE: result indices are written to a `sink` vector; deterministic checksums across runs confirm identical results (same data → same neighbors).

Note: no OpenUSD linkage needed — this is a pure nanoflann measurement.

## 3. Raw evidence

### 3.1 Compile probe output (`compile_probe_output.txt`)

```
PW-1 compile probe: KDTreeSingleIndexAdaptor<L2_Simple_Adaptor<float, Cloud>, Cloud, 3> instantiated OK; dataset=64 pts, knnSearch(k=3) returned 3 (sorted+distinct), radiusSearch returned 17 hits.
```

**The exact roadmap instantiation compiles and runs** — with one catch (§3.3).

### 3.2 Benchmark run 2, 11 iterations (`bench_run2.txt`; `-O3 -DNDEBUG`)

```
PW-1 / E-4 nanoflann 1.12.1 kNN capture bench
guides=4000 (KD-tree dataset), k=3, uniform [0,100)^3, 11 measured iterations per config (+1 warm-up), fixed std::thread pool, contiguous query partition
std::thread::hardware_concurrency = 20

config           n_rest       threads
N=100000 T=1     n=100000     threads=1   build_med=  0.281 ms  query_med=  26.138 ms  total_med=  26.419 ms  total_p95=  26.579 ms  per1k= 0.2642 ms
N=100000 T=4     n=100000     threads=4   build_med=  0.332 ms  query_med=  11.023 ms  total_med=  11.412 ms  total_p95=  11.942 ms  per1k= 0.1141 ms
N=100000 T=8     n=100000     threads=8   build_med=  0.355 ms  query_med=   5.712 ms  total_med=   6.068 ms  total_p95=   6.173 ms  per1k= 0.0607 ms
N=100000 T=20    n=100000     threads=20  build_med=  0.306 ms  query_med=   3.413 ms  total_med=   3.720 ms  total_p95=   4.855 ms  per1k= 0.0372 ms
N=1000000 T=1    n=1000000    threads=1   build_med=  0.287 ms  query_med= 276.366 ms  total_med= 276.655 ms  total_p95= 277.298 ms  per1k= 0.2767 ms
N=1000000 T=4    n=1000000    threads=4   build_med=  0.354 ms  query_med=  81.219 ms  total_med=  81.595 ms  total_p95=  84.288 ms  per1k= 0.0816 ms
N=1000000 T=8    n=1000000    threads=8   build_med=  0.403 ms  query_med=  48.190 ms  total_med=  48.524 ms  total_p95=  51.250 ms  per1k= 0.0485 ms
N=1000000 T=20   n=1000000    threads=20  build_med=  0.348 ms  query_med=  22.566 ms  total_med=  22.919 ms  total_p95=  29.321 ms  per1k= 0.0229 ms

linearity ratio (total median, 1M / 100k at same thread count):
  T=1 : 10.47 x   (100k med 26.42 ms -> 1M med 276.65 ms)
  T=4 : 7.15 x   (100k med 11.41 ms -> 1M med 81.59 ms)
  T=8 : 8.00 x   (100k med  6.07 ms -> 1M med 48.52 ms)
  T=20: 6.16 x   (100k med  3.72 ms -> 1M med 22.92 ms)

ACCEPTANCE:
  100k @ 8 threads total median = 6.07 ms   gate: <= 25 ms -> PASS
  1M   @ 8 threads total median = 48.52 ms   stop: > 300 ms triggers SC-6 -> NOT triggered
```

### 3.3 Cross-validation

- **Earlier probe session** (Sep 5, same directory: `pw1_compile.cpp`, `pw1_bench.cpp`, `run1.txt` at `-O2`, `run_opt.txt` at `-O3`/`-O1`, 7 iterations each): identical data → identical result checksums (`205112367335` at 100k, `2050378987715` at 1M in every run and thread count), and consistent numbers:
  - 100k @ 8 threads: 5.72 ms (run_opt `-O3`), 6.07 ms (`run1` `-O2`) — matches run 2 here.
  - 1M @ 8 threads: 49.30 ms (run_opt `-O3`), 52.99 ms (`run1` `-O2`) — matches run 2 here.
  - Single-threaded `-O1` 1M = 329 ms (just above 300); `-O3` 1M ≈ 267–277 ms. The 300 ms SC-6 line only has teeth for slow/low-optimization single-thread scenarios; with the repo's Release flags it is far away.
- **Dataset-interface finding (the "arity" question, resolved):** in 1.12.1, `L2_Simple_Adaptor::evalMetric` calls `data_source.kdtree_get_pt(b_idx, i)` (nanoflann.hpp:695) and the index reads `dataset_.kdtree_get_point_count()` (nanoflann.hpp:1186). A raw `std::vector` therefore **does not compile** as `Cloud`; it must be a class implementing:
  - `size_t kdtree_get_point_count() const`
  - `T kdtree_get_pt(size_t, int) const`
  - `template <class BBOX> bool kdtree_get_bbox(BBOX&) const` — **required in practice** despite the docs calling it optional: `computeBoundingBox` (nanoflann.hpp:1220) calls it unconditionally; returning `false` selects the default bbox loop.
  - (`kdtree_add_point` only needed for writeable/dynamic datasets — not the capture path.)
  The roadmap's `KDTreeSingleIndexAdaptor<L2_Simple_Adaptor<float, Cloud>, Cloud, 3>` is arities-correct (3 explicit args + default `index_t = uint32_t`, header line ~1830): **it compiles once `Cloud` is that adapter class.**
- Concurrency model matches the header's guarantee: `knnSearch`/`query` are `const`, thread-safe for concurrent readers on a finished index (nanoflann.hpp ~1825–1832).

## 4. Results vs. gate

| Quantity | Measured | Gate / stop | Verdict |
|---|---|---|---|
| 100k rest roots, 4k guides, 8 threads (build + query, median) | **6.07 ms** (p95 6.17) | ≤ 25 ms | **PASS** (4.1× headroom) |
| Linear in root count | 1M/100k ratio ≈ 10.5× (T=1), 7–8× (T=4/8) | linear | **Holds** (per-root cost flat or decreasing; no super-linear growth) |
| 1M rest roots, 8 threads (median) | **48.5 ms** (p95 51.3) | > 300 ms ⇒ SC-6 | **Not triggered** (6.2× below stop line) |
| Build (4k guides, any thread count) | 0.23–0.40 ms | — | Negligible |

Superseded assumption: `design/proposal-performance.md` §5.6 "~10 ms" (100k capture) — measured **5.9–6.1 ms at 8 threads**; the assumption was conservative.

## 5. DECISION

DECISION: E-4 PASSES (M0 pre-work; binding at M3) — vendored nanoflann 1.12.1's `KDTreeSingleIndexAdaptor<L2_Simple_Adaptor<float, Cloud>, Cloud, 3>` compiles and runs when `Cloud` is a dataset-adapter class implementing `kdtree_get_point_count()`/`kdtree_get_pt()`/`kdtree_get_bbox()` (raw `std::vector` does not compile; `kdtree_get_bbox` is effectively mandatory in 1.12.1 despite the docs). k=3 over 4 000 guide roots: **6.07 ms median (p95 6.17) at 100 k / 8 threads** (≤ 25 ms gate), **48.52 ms median (p95 51.25) at 1 M / 8 threads** (< 300 ms, **SC-6 not triggered**), cost linear in root count (1M/100k ≈ 10.5× single-thread, 7–8× multithreaded). The M3 capture schedule for `UsdGenGuideInterpolate` and `UsdGenClump` can commit to full-batch kNN capture: budget ~6 ms per 100 k rest roots on 8 threads (~0.05 ms per 1 k), ~48 ms per 1 M, plus < 0.5 ms tree build over 4 k guides.

## 6. Risks / follow-ups

1. **Uniform-random data is optimistic.** Real rest roots are clumpy (scalp-shaped), and query points near data-dense regions can be slower per query. The gate is *binding at M3* precisely for this: re-measure E-4 on the real `UsdGenGuideInterpolate` capture via `tests/perf/benchUsdGenKnn.cpp` (roadmap line 363). The 4.1× headroom at 100k / 6.2× at 1M absorbs modest adverse distributions, but a > 4× regression would need re-planning.
2. **Optimizer level matters more than threads for the 1M stop line.** `-O1` single-thread 1M was 329 ms (over 300); `-O3` is ~267–277 ms. The shipped build is `-O3` (Release), so this is fine, but note it in `benchUsdGenKnn` — the stop condition is only meaningful against release-flags numbers.
3. **`Cloud` adapter interface is a design constraint for M3.** Any capture code must wrap the guide-root buffer in the 3-method adapter class (not pass a raw vector). Cheap, but it fixes the data layout decision (per-point `kdtree_get_pt` on a SoA/AoS buffer) — an SoA float buffer with `kdtree_get_pt(i,d)=buf[i*3+d]` keeps cache lines tight and matches the existing `usdGenMath` SoA style.
4. **Sublinear multi-thread scaling at 1M** (ratio 6.2–8× instead of 10× at T=4–20) is likely Amdahl/cache effects on the fixed 4k-point tree, not a correctness issue. If M3 wants more than 8 threads for capture, expect diminishing returns beyond 20 (here: only ~2× speedup from T=8→T=20 at 1M).
5. **p95 jitter at high thread counts** (e.g. 1M@20: median 22.9 vs p95 29.3 ms) is scheduler noise on a shared 20-core host; p95s at 8 threads are tight (< 6% over median). Schedule the M3 gate at 8 threads as specified.
