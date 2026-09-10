# Ledger shard B1 — plans 05 + 06

| # | Plan § | Deliverable | Status | Evidence / exit |
|---|---|---|---|---|
| 1 | 05 §1 | C3 curve contract (rest, skinprim/uv, curveId, frozenEpoch) | Spec | freezes end of M2 (§1.6); tested by SI-1, E-1/E-3 (§9) |
| 2 | 05 §2 | `UsdGenCurveSource` load + validation ladder | Spec | load pull 0.19 ms EV-090 (§8.1); ragged ≤2× UNMEASURED → E-1r (§2.5, §9) |
| 3 | 05 §3 | Rest binding `UsdGenRestAPI` + root frames | GAP | 512 KiB rest-channel guard — adjudication pending (.omp/adjudication-1.md, T2.1) |
| 4 | 05 §4 | `UsdGenDeform` transport onto deformed surface | DONE | commit 8a50623 (.omp/decisions-05-done.md); tail 0.4–0.6 ms DERIVED EV-015/016+EV-008 (§8.1, S-2) |
| 5 | 05 §4.5/§6 | Per-sample re-run + `UsdGenSculptLayer` | DONE | deformers batch 2 (.omp/decisions-05b-done.md) |
| 6 | 05 §5 | `UsdGenFreeze` | Evaluated | §5 measured rows: 0.55–0.59 ms flat to 1 M EV-045; undo 0.02 ms EV-044; payload 21.5 ms EV-049; GUI EV-051 (§8.1) |
| 7 | 05 §7 | Guides as curves (reference lane) | Spec | §7.1–7.4; display prims per §6 of 06 |
| 8 | 05 §9 | M2 exits of this plan | Gated | E-1r, E-3, SI-9, S-2, S-3 (§9, lines 1367–68) |
| 9 | 06 §1–2 | Four registrations + UsdImaging adapters | DONE | 16 ops + v10 GPU-first (.omp/decisions-06-done.md) |
| 10 | 06 §2.6 | `UsdGenRestAPI` adapter (S12) | GAP | RestAdapter — adjudication pending (.omp/adjudication-1.md, T2.1) |
| 11 | 06 §3 | `UsdGenGroomSceneIndex` (pruning wrapper, registry, C ABI) | Spec | SI-1…SI-10 gates, M1/M2 (§10.1) |
| 12 | 06 §4 | Published prims; tile contract C2 | Spec | frozen end of M1 (§4.1); SI-1 exactness M1 (§10.1) |
| 13 | 06 §5 | Invalidation discipline | Spec | SI-2 exact locator set M1, re-run M2 (§10.1) |
| 14 | 06 §7 / fur | hdPrman+usdrecord parity; fur renders | PARTIAL | R-1 T4 release gate, built M7 (§10.1); renders/fur in flight |
