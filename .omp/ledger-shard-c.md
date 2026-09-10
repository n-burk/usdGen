# SHARD-C plan/09-11 + full inventory — evidence-grounded

Date 2026-09-10. Sources: glob plan/** (this pass); heading maps (`grep '^#'`) of
plan/09, plan/10, plan/11 (this pass). Section bodies NOT re-read this pass →
statuses UNVERIFIED unless a cited line proves it. Line refs = `file:line`.

## a) Classification — full inventory

Class: EXEC = executable-plan (numbered doc carrying gates/milestones/contracts);
SUP = supporting-evidence. Underpins cited where the cited line was seen in this
pass; (name-match) = inference from naming, NEEDS-VERIFY.

| File | Class | Underpins |
|---|---|---|
| plan/README.md | SUP (authority; class rules §3-4 — NEEDS-VERIFY) | all |
| plan/00-request-and-scope.md | EXEC | 09:532 (09 §5 cites it), 11:3 |
| plan/01-architecture.md | EXEC | (name-match) |
| plan/02-schema.md | EXEC | 11:766-767 (11 §8) |
| plan/03-execution-engine.md | EXEC | 09:642, 09:656 (09 §6), 11:766-767 |
| plan/04-operators.md | EXEC | 11:686-687 (v1 catalogue), 11:766 |
| plan/05-static-curves-and-deformation.md | EXEC | (name-match) |
| plan/06-imaging.md | EXEC | (name-match) |
| plan/07-look-maps-expressions.md | EXEC | (name-match) |
| plan/08-tools.md | EXEC | (name-match) |
| plan/09-performance-and-benchmarks.md | EXEC (frame ledger 09:44; gate registry 09:529) | 11:54-57, 10:1015 |
| plan/10-build-dependencies-testing.md | EXEC (tiers 10:794; gate map 10:1013) | 09:532, 11:151 |
| plan/11-roadmap.md | EXEC (M0-M8 11:141; PW 11:94) | — |
| plan/12-risks-decisions-open-questions.md | EXEC (numbered register — NEEDS-VERIFY) | 09:532 (cited by 09 §5) |
| plan/09a-gpu-inference-context.md | **MISSING** — not in plan/** glob; grep: path not found (this pass) | n/a |
| plan/appendix-A-evidence-ledger.md | SUP (EV-nnn ledger) | 09:119-120 (09 §1), 11:83-84 (09.5), 10:104 |
| plan/appendix-B-prototype-inventory.md | SUP (prototype→plan map) | 11:122 (PW-7…13), 11:629-630 |
| plan/design/ (10: brief-v1, proposal-artist/risk/performance, judge-*, adr-v1, consistency-*) | SUP | adr-v1: 09:759-760 (09 §10), 11:143 (ADR §7); proposal-performance: 09:46-47 (corrects §0.2), 10:267-268; proposal-risk: 11:27-28, 10:281-282 |
| plan/research/ (14: ENVIRONMENT, A1-A8, B-usdrig-build, G-*×10) | SUP | ENVIRONMENT: 09:437-438 (T3), 10:26-27; B-usdrig-build: 10:95-96, 10:118-119; G-* (name-match to prototypes/): NEEDS-VERIFY |
| plan/prototypes/ (9 subdirs: chain-order, data-plane-benchmark, evaluation-scheduling, freeze-bake, instancing, motion-blur, stage-free-transport, storm-hair-look, storm-throughput, tool-loop, thirdparty-bench, usdrig-linux-build) | SUP (measured probe outputs) | 09:468-469 (bench scenes from storm-throughput); appendix-B per 11:122 |

## b) Deliverables — 09/10/11

Statuses: UNVERIFIED = structure confirmed via heading map, body not re-read;
nothing evidence-cited this pass beyond cited line refs.

| # | Doc | Deliverable | Status | M | Gate-id | Evidence | Next-packet | CUDA-candidate |
|---|---|---|---|---|---|---|---|---|
| 1 | 09 | Frame ledger §0.2 (single, ADR R41; corrects proposal-perf §0.2) | UNVERIFIED | M0/M1 | S-9 (09:372-375) | 09:44-47 | re-read 09:44-115 | — |
| 2 | 09 | Gate registry §5.1-5.5 (single, ADR R40) | UNVERIFIED | all | E-*/SI-*/S-*/T-* | 09:529-547; 11:54-57; 10:1015 | re-read 09:529-636; diff .omp/gate-status.md | Basis named: E-1 chain + Storm counters (drawBatches/vboRelocated) — counter rows NEEDS-VERIFY |
| 3 | 09 | Benchmark protocol §4 (EGL T2 / Xvfb T3 / workstation T4 / 4 grooms) | UNVERIFIED | M0 | tier T2-T4 | 09:404-491 | re-read 09:404-491 | no — GPU eval out of scope (09:744-747) |
| 4 | 09 | Cost model §2 (formulas, 1M scaling, memory, motion cache) | UNVERIFIED | M1+ | — | 09:190-330 | re-read 09:190-330 | E-1 basis: chain memory-bound (09:232-234) |
| 5 | 10 | Target graph §1.2 + B-1 dependency rule §1.4 (3× T0, gate:B-1) | UNVERIFIED | M0 | B-1 | 10:166-193, 233-263 | re-read 10:166-263; .omp/gate-status B-1 row | no |
| 6 | 10 | Test tiers T0-T4 §5 + CTest registry §5.6 + gate→test §6.5 | UNVERIFIED | M0 | all | 10:794-1088 | re-read 10:794-1088 | — |
| 7 | 10 | Third-party vendoring §2 (TP-1…TP-8) | UNVERIFIED | M0 | — | 10:299-401 | re-read 10:299-401 | — |
| 8 | 10 | Known build issues §8 (ffp-contract, RemovePrim defect, traps) | UNVERIFIED | M0 | — | 10:1199-1263 | re-read 10:1199-1263 | — |
| 9 | 11 | Pre-work PW-1…PW-13 §1 (PW numbering owned) | UNVERIFIED | M0 | per-PW gates | 11:94-139 | re-read 11:94-139 | — |
| 10 | 11 | Milestones M0-M8 §2 + gate→milestone §2.0 | UNVERIFIED | M0-M8 | — | 11:141-542 | re-read 11:141-220 | — |
| 11 | 11 | Stop conditions §5 (SC-1 hard stop = SI-2 + register) | UNVERIFIED | M1 | SI-2 | 11:643-650 | re-read 11:643-680 | — |
| 12 | 11 | Definition of done §6 (v1/v2/v3 + release checklist) | UNVERIFIED | M7/M8 | — | 11:682-728 | re-read 11:682-728 | — |
| 13 | cross | .omp/gate-status.md + .omp/perf-results.md cross-check | NOT-CROSSED (no budget this pass) | — | — | launch STEP-1 | next packet: read tail ranges; verify E-1 + drawBatches/vboRelocated rows (CUDA basis) | basis rows live here |
| 14 | 09a | plan/09a-gpu-inference-context.md | **MISSING** — absent from plan/** (glob + grep this pass) | — | — | inventory §a | adjudicate: expected by launch context; not on disk | n/a |

Status counts: UNVERIFIED 12 · NOT-CROSSED 1 · MISSING 1 · evidence-cited 0 (bodies not re-read this pass)

Top MISSING (≤8):
1. 09a-gpu-inference-context.md absent from plan/ (rows 14)
2. .omp/gate-status.md + perf-results.md rows not crossed (row 13)
3. 09 §5.1-5.5 registry rows (gate metric/status) not re-read (row 2)
4. 09 §0.2 frame-ledger numbers not re-read (row 1)
5. 11 §2.0 gate→milestone table body not re-read (row 10)
6. README §3-4 class rules not re-read (classification self-rule)
7. G-*/prototype underpins are name-match inferences (inventory §a)
8. 09a CUDA-candidate counter rows (drawBatches/vboRelocated) unlocated — need 09 §5.3 + .omp rows
