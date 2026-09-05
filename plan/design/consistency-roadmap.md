# Cross-document consistency review — roadmap, gates and cross-references

Date: 2026-09-05. Status: review v1 (findings, pending reconciliation).

This report is the cross-document consistency pass over the fifteen usdGen plan documents, taken on one
lens: **roadmap, gates and cross-references**. It checks that every gate id referenced anywhere exists in
`09-performance-and-benchmarks.md` §5 with the same metric, criterion, tier and milestone; that contracts
C1–C5 freeze at the same milestone in every document; that every open question in
`12-risks-decisions-open-questions.md` closes on a gate or milestone that exists; that every "see `NN-…md`
§X" resolves to a section that covers the claimed topic; that the v1/v2/v3 boundaries in `00`, `04`, `11`
and `12` are identical; that the estimates match ADR §7; and that the Testing sections' tier assignments
agree with `10-build-dependencies-testing.md` §5. It edits no plan document: each finding names the
canonical value with the authority that makes it canonical and the file that must change.

Reads with: `design/adr-v1.md` (§3 contracts, §7 milestones and gates, §9 rulings R1–R45),
`design/brief-v1.md` (S1–S46), and the fifteen plan documents under `plan/`.

---

## 0. Method, and the authorities used

Five documents are *registries* by ADR ruling, and every conflict below is resolved in their favour:

| Registry | Owns | Authority |
|---|---|---|
| `09-performance-and-benchmarks.md` §5 | gate id → metric, pass criterion, driver, tier, milestone | ADR §9 R40 |
| `09-performance-and-benchmarks.md` §0.2 | the only frame ledger | ADR §9 R41 |
| `11-roadmap.md` §1, §2, §4, §5, §6.4 | PW ids, milestone contents and exits, estimates, SC-1…SC-10, RC-1…RC-11 | ADR §7, §9 R39 |
| `10-build-dependencies-testing.md` §5.1, §5.6, §3.5 | test tiers, the CTest name registry, the env-var registry | ADR §9 R2, R35 |
| `02-schema.md` | the normative property registry | ADR §9 R7 |

Two caveats on R40 itself. R40 fixes a list of gate → tier → milestone assignments and adds "**where 09
already places a gate not named here, 09 stands**". So `S-11`, `S-12`, `T-EXPR-1` and `T-PTEX-1` — ids 09
mints — are canonical *as 09 defines them*, while for every id R40 names (`SI-5`, `S-6`, `E-3`, `E-8`,
`S-4`, `T-INST-1`, …) R40 wins over 09's own text. Both directions produce findings below.

Method: every gate, milestone, contract, PW, SC, RC and EV id in the fifteen documents was extracted and
compared; every `NN-…md §X` cross-reference was resolved against the target file's actual section
headings (468 references; all numeric targets resolve — the failures below are *topical*, not numeric);
the version tables of `00` §2.1, `04` §1.2–§1.4, `11` §6.1–§6.3 and `12` §0.4/§6 were diffed row by row.

---

## 1. The gate registry, reconciled

### 1.1 Assignments that disagree across documents

Canonical column = ADR §9 R40, or 09 §5 where R40 is silent.

| Gate | Canonical | 09 §5 | 10 §6.5 | 11 §2.0 | 12 §5 | 01 §9 | 05 §9 | 06 §10.1 | Verdict |
|---|---|---|---|---|---|---|---|---|---|
| **SI-5** | T1, **M1** (R40 "SI-1…SI-5 T1 M1") | **M0** ✗ | M1 ✓ | M1 ✓ | **M0** ✗ | M0 proof / binding M1 ✓ | — | **M0** ✗ | fix 09 §5.2 + §5.5, 12 §5, 06 §10.1 |
| **S-6** | T2, **M1** (R40) | **M2** ✗ | M1 ✓ | M1 ✓ | **M2** ✗ | M1 ✓ | M1 ✓ | **M2** ✗ | fix 09 §5.3 + §5.5, 12 §5, 06 §10.1, appendix B §13 |
| **E-3** | T0, **M2** (R40) | M2 ✓ | M2 ✓ | M2 ✓ | **M1** ✗ | M2 ✓ | M2 ✓ | — | fix 12 §5; 09's status cell is stale (§1.3) |
| **S-4** | T2, **M2** (R40) | M2 ✓ | M2 ✓ | M2 ✓ | **M3** ✗ | — | — | M2 ✓ | fix 12 §5; 09/06 parentheticals stale |
| **E-8** | T0, **M1**, re-run M4 | M1 ✓ | M1 ✓ | M1 ✓ | M1 ✓ | M1 ✓ | M1 ✓ | — | 09's status cell ("`10-…` says M0") is stale |
| **SI-8** | T1, M0 pre-work, binding M1 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 02 §7.5/§9 call it **T0/T1** ✗ |
| **T-2** | **T1**, M5 | T1 ✓ | T1 ✓ | T1 ✓ | T1 ✓ | — | T1 ✓ | — | 00 §7 implies T3 ✗ |
| **T-INST-1** | **T1**, M6 | T1 ✓ | T1 ✓ | T1 ✓ | **T1/T3** ✗ | T1 ✓ | — | T1 ✓ | fix 12 §5 |
| **SI-9** | T1, **M2**, pruning-wrapper cost | M2, metric = sessions **and** pruning cost; test `testUsdGenSessions` | M2, pruning cost only; test `testUsdGenPruningCost` | M2, both | (absent; 12 requests SI-10 instead) | M2 ✓ | M2 ✓ | M2, test `testUsdGenPruningCost` | one metric and one test name, 09 §5.2 to decide |
| **S-10** | T2, **M8**, in-place overlay (R29) | M8 ✓ | M8 ✓ | M8 ✓ | M8 ✓ (§0.4 R29 row) | — | M8 ✓ | — | consistent; only 09's *status note* about 12 is stale |
| **S-11** | T2, **M7**, `head1M` draw record (09 §5.3) | M7 ✓ | absent ✗ | absent ✗ | **used for the tile sweep, M1** ✗ | — | — | — | fix 12, appendix A; add a row to 10 §6.5 |
| **S-12** | T2, **M1** before C2 freezes, tile sweep (09 §5.3) | M1 ✓ | absent ✗ | absent ✗ | absent (calls it S-11) ✗ | — | — | — | add to 10 §6.5, 11 §2.0/§6.4, 12 |
| **T-EXPR-1 / T-PTEX-1** | T0, **M4** (09 §5.4) | M4 ✓ | absent ✗ | absent ✗ | rejects the family (§0.4 R2) ✗ | — | — | — | one ruling needed; see F-26 |
| **B-1** | T0, **M0** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | — | consistent; the "09 has no build gate" claims in 12 §5 and appendix A §5 are stale |
| **L-2…L-5** | L-2 T2 M1; L-3/L-4/L-5 T0–T1 M4 | ✓ | ✓ | ✓ | ✓ (as extensions) | — | — | — | consistent; the "must be added to 09" notes in 07 §9.1, 11 §2.0, 12 §5 are stale |

### 1.2 Milestone exit sets — 09 §5.5 versus 11 §2.0

| M | 09 §5.5 | 11 §2.0 | Difference |
|---|---|---|---|
| M0 | B-1, **SI-5**; PW records for E-4, S-8, S-9, L-1, SI-8 | B-1; PW-1…PW-6 recorded | SI-5 (canonical M1 — 09 to change) |
| M1 | E-1, E-2, E-6, E-7, E-8; SI-1…SI-4, SI-6…SI-8; S-1, S-5, **S-12**; L-2; S-8/S-9/L-1 decisions | E-1, E-2, E-6, E-7, E-8; SI-1…SI-8; S-1, S-5, **S-6**, S-8, S-9; L-1, L-2 | **S-12 missing from 11**; S-6 is M1 (09 to change); 11 folds SI-5 into "SI-1…SI-8" |
| M2 | E-1r, E-3; SI-2 re-run; SI-9; S-2, S-3, S-4, **S-6** | E-1r, E-3; SI-9; S-2, S-3, S-4 (+ SI-2 re-run) | S-6 belongs at M1 |
| M3 | E-4, E-5, E-1 with 7 nodes | same | — |
| M4 | E-8 re-run; L-3, L-4, L-5; **T-EXPR-1, T-PTEX-1** | L-3, L-4, L-5 + goldens | **T-EXPR-1 / T-PTEX-1 missing from 11** |
| M5 | T-1…T-4; S-7 | same | — |
| M6 | T-INST-1/2 | same | — |
| M7 | E-5 re-run; **S-11**; full E-*/SI-*/S-* re-run; R-1 built | P2 assertion, `usdrecord --renderer GL`, hdPrman sampled-points contract; re-runs | **S-11 missing from 11**; 11's three exits are not gate ids and are not in 09 |
| M8 | T-5; S-10 | same | — |
| release | R-1, R-2, R-3 inside RC-1…RC-11 | same | — |

`11-roadmap.md` §6.4 RC-1 lists "S-1 … S-9" and therefore also omits **S-11** and **S-12**; §7's T2 row
says "S-1 … S-10", same omission; `appendix-B` §14 says "S-1…S-10" too.

### 1.3 Status and reconciliation notes that are now false

Each of these was true when written and is not true of the current sibling. They are load-bearing because
a reader follows them into an edit that has already been made — or skips one that has not.

| # | Where | Claim | Reality today |
|---|---|---|---|
| 1 | 09 §5.1, E-3 status | "`10-…` §6.5 and `11-…` §2.0 say M1 and are filed" | both say **M2** |
| 2 | 09 §5.1, E-8 status | "`10-…` §6.5 says M0" | 10 §6.5 says **M1** |
| 3 | 09 §5.3, S-4 status | "`10-…` §6.5 and `11-…` §2.0 say M3" | both say **M2** |
| 4 | 09 §5.3, S-10 status | "`12-…` §3, §7 use `S-10` for a tile-count sweep" | 12 now uses **S-11** for it — the collision moved, it did not close |
| 5 | 09 §8 | "`benchUsdGenMaps` (L-4) and `testUsdGenMaps` (L-3) are new here and must be added [to 10 §5.6]" | both are in 10 §5.6 |
| 6 | 11 §2.0 | "Five binding ids are absent from 09 §5 and must be added: B-1, SI-9, L-2, L-3/L-4/L-5, S-10" | all five are in 09 §5 |
| 7 | 11 §2.0 | "Rows of 09 §5 that R40 corrects: … E-3 M1 → M2; E-8 M0 → M1; S-4 M3 → M2; T-INST-1 T3 → T1; E-1 2.5 → 1.5 ms" | already corrected in 09; only **SI-5** and **S-6** remain |
| 8 | 11 §2.0 | "`12-…` §3 defines S-10 and SI-9 differently" | 12 now uses **S-11** and **SI-10** |
| 9 | 12 §5 | "S-11 and SI-10 must be defined [in 09]; B-1 must be added there; E-1 must move to ≤ 1.5 ms; E-8 from M0 to M1; L-3/L-4 must land in 09 §5.4" | B-1, E-1 ≤ 1.5 ms, E-8 M1, L-3/L-4/L-5 are all in 09 today |
| 10 | 12 §5 | "B-1 … today it appears only in `01-architecture.md` §5.2 and §9" | B-1 is in 09 §5.1, 10 §1.4/§6.5, 11 §2.0/§2.1, appendix A §5, appendix B §11 |
| 11 | 12 §2 RK-29 | "`02-schema.md` §7.3 and `10-…` §4.3 still route the `LibraryPath` at `usdGenImaging`" | 02 **§7.2** and 10 §4.3 both name `libusdGenSchema.so`; §7.3 is "Adapter registrations" — wrong section, wrong claim |
| 12 | 12 §4 A-19 | "`11-…` §5.2 SC-4 still carries the superseded unsalted formula" | 11 §5.2 SC-4 carries the salted R13 formula |
| 13 | 06 §10.1 | "SI-9 … is not yet a row in 09 §5.2 or 10 §6.5" | both carry it |
| 14 | 06 §10.1 | "S-4 … `09-…` §5.3 and `10-…` §6.5 still say M3"; "T-INST-1 … `09-…` §5.4 and `10-…` §6.5 record T3" | both say M2 and T1 |
| 15 | 07 §9.1 | "[09 §5] carries L-1 today and must gain the four new rows"; L-4's threshold "is cited from here once it lands there" | 09 §5.4 carries L-2…L-5 with the ≤ 200 ms threshold |
| 16 | 03 §12 / §12.1 corrections C, D, H, I | "09:531 still reads ≤ 2.5 ms"; "E-3 is M1 at :534"; "E-8 is M0 at :539"; "`grep -c 'EV-0' appendix-A` = 0"; "09 still cites Q2, Q4, Q5, Q9, Q13"; "`USDGEN_OP_CHECKS` absent from 10"; "06 §3.9 trigger (c) still reads *with no app driver attached*" | 09 reads ≤ 1.5 ms / M2 / M1; appendix A has 83 `EV-0nn` rows and no `Q<n>` handles; 10 §3.5 registers `USDGEN_OP_CHECKS`; 06 §3.9 says "**always applies**" |
| 17 | 10 §5.2 | "`09-…` §5.4 and `08-…` §9.5 … must be re-pointed at this numbering" | still owed — see §5 below |
| 18 | appendix A §5 | "09 §5 has no build-gate table today; B-1 (U20) needs one" | 09 §5.1 opens with the B-1 row |

---

## 2. Gate ids that collide or exist in one document only

| Id | Meaning A | Meaning B | Canonical | Files to change |
|---|---|---|---|---|
| **S-11** | tile-count sweep, M1, before C2 freezes (12 §2 RK-07, §3 Q-02, §5; appendix A §5 U1, §2.11) | `head1M` static draw record, M7 (09 §5.3) | **09 §5.3**: S-11 = `head1M` record (M7); the sweep is **S-12** (M1) | 12 §2, §3, §5; appendix A §5 and §2.11 |
| **SI-10** | session arbitration, M2 (12 §2 RK-09, §3, §5) | `reorder nameChildren` notice check, M0 (appendix A §5 U13) | neither: session arbitration is folded into **SI-9** (09 §5.2, R40); the reorder check is **PW-6**, explicitly not a gate (11 §1) | 12 §2, §3, §5; appendix A §5 |
| **SC-11** | M1 engine floor, E-1/E-7 (12 §2.1) | UsdSkel pickup / pruning wrapper, M2 (05 §4.2, §9) | undecided — `11-roadmap.md` §5 is the SC register and stops at SC-10 | 11 §5.2 must mint both (suggest SC-11 = engine floor, SC-12 = M0 chain order per 12; the UsdSkel stop condition takes the next free id), then 05 and 12 cite it |
| **SC-12** | M0 chain order, SI-5 (12 §2.1) | — | same: absent from 11 §5 | 11 §5.2, 12 §2.1 |
| **T-EXPR-1 / T-PTEX-1** | registered gates, T0, M4 (09 §5.4) | "ad-hoc gate families … superseded by R2" (12 §0.4), and the assertions folded into L-3/L-4 (12 §2, §5) | 09 §5 is the registry and R40 lets 09 stand where R40 is silent ⇒ keep them as gates | 12 §0.4/§2/§5, 10 §6.5, 11 §2.0 |
| **B1–B12** | the Storm-throughput benchmark protocol of `research/G-storm-throughput-and-prim-granularity.md` §3.5 (a **T2** EGL sweep; appendix B §9 derives S-3/S-5 thresholds from it) | "the B1–B12 runner" of the **T4 workstation protocol**, built at M7 (11 §2.1, §2.8, §3.3, §6.4 RC-3, §7) | B1–B12 is the T2 benchmark protocol; the T4 protocol is `docs/workstation-protocol.md` | 11 §2.1, §2.8, §3.3, §6.4, §7 |

---

## 3. Pre-work ids (PW-n)

`11-roadmap.md` §1 defines **PW-1…PW-6** and `10-build-dependencies-testing.md` §6.5 states explicitly
that "the numbering is `11-roadmap.md` §1's, which owns it". `appendix-B-prototype-inventory.md` §13
publishes a different, longer numbering, and two ids mean different things in the two files:

| Id | 11 §1 (canonical) | 10 §6.5 | appendix B §13 |
|---|---|---|---|
| PW-1 | E-4 kNN | E-4 | E-4 ✓ |
| PW-2 | S-8 `hairTangent` | S-8 | S-8 ✓ |
| PW-3 | S-9 refineLevel | S-9 | S-9 ✓ |
| PW-4 | L-1 render context | L-1 | L-1 ✓ |
| **PW-5** | **SI-8 auto-apply** | SI-8 | **S-1 re-measured at 100 k** ✗ |
| **PW-6** | **`reorder nameChildren` check** | reorder check | **SI-8 auto-apply** ✗ |
| PW-7…PW-13 | — | — | SI-7, SI-6, E-1r, reorder check, motion P1/P2, GPU counters, SI-9 pruning cost |

Consequence: appendix B §5 ("PW-7"), §7 ("PW-2"), §11 ("PW-1, PW-9 and PW-6"), §12 ("PW-11"), §14 ("gate
SI-9 is PW-13"), and the T2 row's "`make_scene_100000.py` for PW-5" all resolve to the wrong item under
the canonical numbering. Either appendix B renumbers onto 11 §1's six and gives its extra harness items
new ids (PW-7…PW-13 with 11 §1 gaining them), or 11 §1 adopts appendix B's thirteen — but 10 §6.5 and 11
§2.1 must then be re-pointed. **Recommendation: 11 §1 gains PW-7…PW-13 as harness pre-work, and appendix
B swaps its PW-5/PW-6 to match 11.**

---

## 4. Version boundaries v1 / v2 / v3

`04-operators.md` §1.2–§1.4, `11-roadmap.md` §6.1–§6.3 and `12-…` §0.4 (R38 row) agree exactly:
**v1 = the M0–M7 deliverable set** including `UsdGenPtexMap` (M4), `UsdGenInstance` (M6) and motion
profiles P0/P1/P2 (M7); **v2 = M8 breadth** including `Scatter` mode `uniform`, `UsdGenExprOp`, TsSpline
ramps in the UI, sculpt rebase, progressive generation (T-5) and the in-place overlay (S-10);
**v3 unchanged**.

`00-request-and-scope.md` §2.1 is the sole outlier: its v2 row still carries `UsdGenInstance`,
`UsdGenPtexMap` and motion profile P1, and its v1 row carries P2 under an "ASSUMPTION … the label conflict
is recorded unresolved" note. ADR §9 R38 closed that conflict and R44 requires 00's version table to carry
R38's split. Two consequential knock-ons inside 00 §2.3:

* "`04-operators.md` §1.2 dates all four modes M1" — 04 §1.2 dates `random` M1, `atGuides` M3, `points` M5,
  `uniform` v2/M8, and the sentence's conclusion ("no document dates it") is therefore wrong twice.
* "`11-roadmap.md` §6.1 and `04-operators.md` §1.2–1.4 record the tension … as unresolved" — both now
  record R38 as the resolution.

One smaller version cross-reference: `04-operators.md` §1.2 note ² says "neither ADR §7 nor `11-roadmap.md`
§6.1 schedules `UsdGenScale`". 11 §6.1 lists "`UsdGenScale` (M4)" and 11 §2.5 puts it in M4's scope.

---

## 5. Cross-references that resolve numerically but not topically

1. **The T4 workstation protocol has three numberings.** 09 §4.3 lists five items ((1) frame time, (2)
   MSAA/A2C vs OIT, (3) Hgi resource generation, (4) non-NVIDIA drivers, (5) hdPrman parity). 10 §5.2
   defines the protocol as `research/G-storm-hair-look-prototype.md` §6 verbatim (**§§1–6**) plus §7
   hdPrman and §8 4K, and 10 §6.5/§9 map **R-1 → §7, R-2 → §3, R-3 → §5**. 08 §8.3 defines **W1–W4** and
   says they "are appended [to 09 §4.3] as items 6–9". Under 09's numbering R-2 is item 2 and R-3 is items
   3–4; under 10's they are §3 and §5. Any reader following 10 §6.5 into 09 §4.3 lands on the wrong check.
   10 §5.2 already records the edit as owed; it is owed against 09 §4.3 **and** 08 §8.3/§9.5.
2. **`EV-nnn` ids do not resolve between 09 and appendix A.** `appendix-A-evidence-ledger.md` §2 numbers
   its rows sequentially, **EV-001…EV-083**, and §2.0 publishes the retired-id map. 09 §1 declares a
   different scheme — "a 100-block per appendix subsection — §2.1 → EV-001…, §2.2 → EV-101…, §2.3 →
   EV-201…" — and uses it throughout (§0.2, §0.3, §1, §2, §5, §7). No other document does. The result is
   both dangling and *silently wrong* citations:

   | 09 cites | 09 means | appendix A's row with that id | appendix A's actual row for the datum |
   |---|---|---|---|
   | EV-201/202/203 | Storm draw 4 k / 40 k / 200 k | none | EV-019 / EV-020 / EV-021 |
   | EV-204 | deform upload delta | none | EV-022 |
   | **EV-021** | SoA/AoS layout, 0.506 ms | **200 k Storm draw, 23.93 ms** | EV-014 |
   | **EV-023** | SoA strip, 0.158 ms | **refineLevel 3 costs nothing extra** | EV-016 |
   | EV-303/304/305/307/308/310 | Storm CPU costs | none | EV-026/027/028/030/031/033 |
   | EV-401…405, EV-504, EV-510, EV-601…611, EV-701…704, EV-801…805, EV-904, EV-951 | RigExec, freeze, tool loop, third party, motion, build, adapter | none | EV-035…039, EV-044, EV-050, EV-055…066, EV-067…070, EV-071…075, EV-079, EV-081 |
   | EV-312 | `HdRetainedSceneIndex::DirtyPrims` delivery | none | **no row exists** — 09 §1 says appendix A "must gain `A2.4-CPU12`", naming the id scheme R1 retired |

   `04-operators.md` (EV-014, EV-016, EV-052, EV-067, EV-069, EV-070, EV-081, EV-082) and `03`, `10`, `11`,
   `12` (EV-001) all use appendix A's numbering. **Canonical: appendix A §2's `EV-001…EV-083`** (ADR §9 R1
   and R43 make appendix A's rows the citation handle). 09 must be remapped; appendix A must add the
   missing notice-delivery row.
3. **`12-…` §2 RK-29 → `02-schema.md` §7.3.** The compute-extent registration is 02 **§7.2**; §7.3 is
   "Adapter registrations", and the claim itself is stale (§1.3 row 11).
4. **`08-tools.md` §1.4's entry-point count.** 11 §2.6 and §7 both say "the eighteen entry points of
   `08-tools.md` §1.4". 08 §1.4 (and 06 §3.8, which repeats it verbatim) declares **nineteen** exported
   symbols: R31's eighteen plus `UsdGenImaging_GetLastError()`. `testUsdGenAbiSymbols` compares
   `nm -D` output against that list, so the count is load-bearing.
5. **`08-tools.md` §8.1** quotes S45's "~15 ms/frame for usdGen at 60 Hz". ADR §9 R41 and 09 §0.2 retire
   that reading: 60 Hz at 100 k × 8 CV is **not established**, and usdGen's share of a deform frame is
   ≤ 1.0 ms with Storm holding ~12.2 ms (DERIVED).
6. **`01-architecture.md` §0.3** restates a full frame ledger where R41 says it must cite 09 §0.2. The two
   disagree in five rows (routing 0.05 vs 0.04 ms; interleave 0.25–0.5 vs 0.25; per-tile extent tagged
   DERIVED vs ASSUMPTION; total 0.8–1.2 vs 0.8–1.0; frame 16 vs 15.8–16.0), and 01's header line targets a
   "1920×1080-class viewport" while every draw number in it is 720p — which 09 §2.5 forbids quoting at
   another resolution until S-1 lands.

---

## 6. Testing sections versus `10-…` §5

### 6.1 Tier assignments

Tiers agree everywhere (T0 engine + build, T1 headless scene index, T2 EGL Storm, T3 `testusdview` under
Xvfb, T4 workstation = release criteria only) except:

* `02-schema.md` §7.5 and §9 row 4 place **SI-8** at "T0/T1" / "T0+T1"; canonical is **T1** (09 §5.2).
* `00-request-and-scope.md` §7 item 3 says "Tier T3 gates are ordinary milestone exits: T-1, T-2, T-3 and
  T-4 all exit M5" — **T-2 is T1** (R40, 09 §5.4, 10 §6.5, 11 §0.3).
* `12-…` §5 gives T-INST-1/2 as "T1/T3"; canonical **T1**.
* `03-execution-engine.md` §13.2 asserts SI-3 as "ten interactive events produce exactly ten commits, all
  on the **main thread**"; 09 §5.2 and appendix A §5 U12 fix the criterion as "on the **app or notice**
  thread — the thread that committed", never a Hydra reader thread.
* `06-imaging.md` §10.1 gives SI-3 a second threshold ("≤ 20 batched") that 09 §5.2 does not carry.

### 6.2 Test-binary names

`10-…` §5.6 is the CTest name registry (09 §8 and 08 §8.2 both say so). Divergences:

| Gate / role | 10 §5.6 + §6.5 | Other documents |
|---|---|---|
| T-1 | `testUsdviewUsdGenComb.py` | `testUsdviewUsdGenBrush` (09 §5.4, §8; 08 §8.2, §9.5) |
| T-3 | `testUsdviewUsdGenPick.py` | `testUsdviewUsdGenPickAccuracy` (09 §5.4, §8; 08 §8.2) |
| T-INST-1 | `testUsdGenInstancerPick` (T1) + `testUsdviewUsdGenCards.py` (T3) | `testUsdviewUsdGenInstancerPick` (09 §8) |
| L-2 | `testUsdGenStormLook` | `testUsdGenStormGolden` (09 §5.4, §8) |
| SI-9 | `testUsdGenPruningCost` | `testUsdGenSessions` (09 §5.2, §8) |
| B-1 | `testUsdGenLinkRule_{usdGen,usdGenMath,usdGenTestUtils}` + `testUsdGenIncludeRule` | `testUsdGenLinkRule` (09 §5.1, §8) |
| C1/C2/C3/C5 freeze | *absent* | `testUsdGenContracts` (T1) — 11 §0.2, §7 |
| C4 freeze | `testUsdgenAbi` (T1) | `testUsdGenAbiSymbols` (T0) — 11 §0.2, §7 |
| engine T0 | `testUsdGenMath`, `testUsdGenKernelDeterminism`, `benchUsdGenChain`, `benchUsdGenKnn` | `testUsdGenKernels`, `testUsdGenSchedule` (03 §13.1); `benchChain`, `benchKnn` (11 §7) |
| deform / one-tile / relocation / scrub | `benchUsdGenStorm --deform|--onetile|--scrub` | `benchUsdGenDeformFrame`, `benchUsdGenOneTile`, `benchUsdGenNoRelocation`, `benchUsdGenDensityScrub` (05 §9) |
| UsdSkel pickup | `testUsdGenSkelInterop` (also 01 §9, 11 §2.3) | `testUsdGenSkelPickup` (05 §9) |
| look T2 | `testUsdGenStormMaterial` | `testUsdGenMaterialBinding` (07 §9.2) |
| undo T3 | *absent* | `testUsdviewUsdGenUndo.py` (08 §9.5) |
| 06's four additions | `testUsdGenPruningCost` present; `testUsdGenInstanceKeys`, `testUsdGenSurfaceResolver`, `testUsdGenMotionSamples` absent | 06 §10.2 (which already flags three of the four as owed) |

### 6.3 Targets per milestone

Three documents schedule the CMake targets and disagree:

| Target | 10 §1.6 | 11 §2.1 | 01 §5.2 |
|---|---|---|---|
| `usdGen_seexpr` | **M0** (noise only), interpreter M4 | **M4** | **M4** |
| `usdGenTestUtilsHd` | M0 | not named (only `usdGenTestUtils`) | not a target — 01 §5.1's `usdGenTestUtils` links `hdSt hgiGL usdImagingGL` |
| `usdGenUsdview` | M5 | **M0 shell**, M5 full | M5 |
| `nanoflann` | M0 | M0 | — |

10 §1.2 is the target registry and its split of `usdGenTestUtils` (T0, no Hydra, covered by B-1) from
`usdGenTestUtilsHd` (T1/T2/T3) is load-bearing for gate B-1; 01 §5.1's single Hydra-linking
`usdGenTestUtils` would fail it.

---

## 7. Estimates

`11-roadmap.md` §4.1 (2 + 5 + 4 + 5 + 4 + 5 + 3 + 5 = 33 calendar weeks, +1 week integration float at the
C4 freeze = **34**; 84 eng-weeks; group totals M0–M2 ≈ 11–12, M0–M5 ≈ 25, M0–M7 ≈ 34) matches ADR §7 and
ADR §9 R39 exactly, and `12-…` A-01/A-30/Q-16 and `00` §2.3's three group totals agree.

One mismatch: `00-request-and-scope.md` §2.3 describes the contingency as "30 % … applied to M1/M3/M7 and
a 25 % calendar stretch to M2/M4/M5". 11 §4.1 applies **+25 % on M1/M3/M5/M7 and +33 % on M2/M4**, and
says in terms that this is "a coarser instrument than ADR §7's 30 % on M1/M3/M7, with the same intent and
the same totals". 00 has merged the two schemes into a third that matches neither.

Contracts are clean: C1 M1, C2 M1, C3 M2, C4 M5, C5 M1 in ADR §3, 00 §5.3, 01 §5.3, 11 §0.2, 02 §0/§8.1,
05 §1.6, 06 §4.1, 07 §2.2, 08 §1.4 — no discrepancy found.

---

## 8. Open questions in `12-…` §3 — do they close on something that exists?

| Q | Closes on | Exists? |
|---|---|---|
| Q-01 | SI-7 / M1 | ✓ |
| **Q-02** | **S-11** (requested) / M1 | ✗ — the tile sweep is **S-12** (09 §5.3) |
| Q-03 | L-1 / M0 pre-work, binding M1 | ✓ |
| Q-04 | S-8 | ✓ |
| Q-05 | S-9 | ✓ |
| **Q-06** | "M0 pre-work notice check `PW-n`" | ✓ but unnamed — it is **PW-6** (11 §1); appendix B calls the same item PW-10 |
| Q-07 | SI-8 | ✓ |
| Q-08 | E-1r / M2 | ✓ |
| Q-09 | SI-4 at M1, extended at M7 | ✓ |
| Q-10 | T-INST-2 / M6 | ✓ |
| Q-11 | T-5 / M8 | ✓ |
| Q-12 | S-1 / M1 | ✓ |
| Q-13 | M5 | ✓ (a milestone, not a gate — allowed) |
| Q-14 | M3 | ✓ |
| Q-15 | S-8 M1 → M5 → release | ✓ |
| Q-16 | M0 re-baseline | ✓ |

Also in §2: RK-07 and RK-09 name **S-11** and **SI-10**; §2.1 names **SC-11** and **SC-12**, which
`11-roadmap.md` §5 does not carry.

---

## 9. Files to change, in the order that closes the most conflicts

1. **`09-performance-and-benchmarks.md`** — SI-5 M0 → M1 (§5.2, §5.5); S-6 M2 → M1 (§5.3, §5.5); refresh
   the six stale Status cells (§1.3 rows 1–5, 18); decide SI-9's metric and test name against 10 §6.5;
   renumber every `EV-1nn…EV-9nn` citation onto appendix A's `EV-001…EV-083`; align the test names of §5
   and §8 with 10 §5.6; renumber §4.3's workstation items to 10 §5.2's §§1–8.
2. **`11-roadmap.md`** — add S-12 (M1 exit, before C2 freezes), S-11 (M7), T-EXPR-1/T-PTEX-1 (M4) to §2.0,
   §6.4 RC-1 and §7; delete the three stale paragraphs in §2.0; mint SC-11/SC-12 (and the UsdSkel stop
   condition) in §5.2; adopt or absorb appendix B's PW-7…PW-13; fix "eighteen entry points" → nineteen;
   reconcile §2.1's target list with 10 §1.6; separate the B1–B12 benchmark protocol from the T4
   workstation protocol; use 10 §5.6's test names in §0.2 and §7.
3. **`12-risks-decisions-open-questions.md`** — rename its S-11 → **S-12** and SI-10 → **SI-9** everywhere
   (§2 RK-07/RK-09, §2.1, §3 preamble/Q-02, §5); fix the milestone cells E-3 M1 → M2, S-4 M3 → M2, S-6
   M2 → M1, SI-5 M0 → M1, T-INST-1 "T1/T3" → T1; delete the stale reconciliation paragraph in §5 and the
   stale claims in RK-29, RK-30 and A-19; reconcile the R2 row's rejection of `T-EXPR-*` with 09 §5.4;
   name PW-6 in Q-06; point RK-29 at 02 **§7.2**.
4. **`10-build-dependencies-testing.md`** — add S-11, S-12, T-EXPR-1, T-PTEX-1 rows to §6.5; add
   `testUsdGenContracts` and the C4 ABI-symbol test to §5.6 (and settle `testUsdgenAbi` vs
   `testUsdGenAbiSymbols`, T0 vs T1); add 06 §10.2's three remaining tests; reconcile SI-9's test name with
   09; keep §5.2's protocol numbering and require 09 §4.3 and 08 §8.3 to follow it.
5. **`00-request-and-scope.md`** — §2.1 version table to R38; §2.3's two stale claims about 04 §1.2 and the
   "unresolved tension"; §2.3's contingency wording to 11 §4.1; §5.4's gate-family list to R44 (add `B-`,
   SI-9, S-10/S-11/S-12, L-2…L-5, T-EXPR-1/T-PTEX-1); §7's "T-2 is a T3 gate"; §5.2/§6.1's `P1–P8` →
   `I1–I8` (R1) and `usdGenPy` → `usdgen` (R5).
6. **`appendix-A-evidence-ledger.md`** — drop the SI-10 mint and the S-11 rename proposal (use S-12); add a
   row for the notice-delivery measurement 09 cites as EV-312; delete the "09 has no build-gate table"
   note; extend §2.0's legacy map to cover 09's retired 100-block ids.
7. **`appendix-B-prototype-inventory.md`** — renumber PW-5/PW-6 onto 11 §1 (and register PW-7…PW-13 there);
   fix "S-4/S-5/S-6 at M2" (S-5 and S-6 are M1); extend the T2 coverage row past S-10.
8. **`06-imaging.md`** — SI-5 M0 → M1; S-6 M2 → M1; drop the four stale parentheticals; align SI-3's
   criterion with 09 §5.2.
9. **`05-static-curves-and-deformation.md`** — take the SC id 11 §5.2 mints for the UsdSkel stop condition;
   rename `testUsdGenSkelPickup` → `testUsdGenSkelInterop`; use 10 §5.6's T2 driver names.
10. **`03-execution-engine.md`** — refresh §12's E-1/E-3/E-8 cells and §12.1 corrections C, D, H and I;
    "S-1…S-10" → "S-1…S-12"; fix §13.2's SI-3 "main thread"; use 10 §5.6's binary names in §13.1.
11. **`08-tools.md`** — §8.2's test names to 10 §5.6 (or state the exception); §8.1's "~15 ms for usdGen"
    to 09 §0.2 under R41; §8.3's W1–W4 mapping to 10 §5.2's numbering; register
    `testUsdviewUsdGenUndo.py` in 10 §5.6.
12. **`01-architecture.md`** — §0.3 to cite 09 §0.2 rather than restate it (and drop the 1920×1080 header
    line); §5.1/§5.2's target-milestone list and `usdGenTestUtils` link set to 10 §1.2/§1.6.
13. **`07-look-maps-expressions.md`** — delete the two stale "must land in 09" notes; `testUsdGenMaterialBinding`
    → 10 §5.6's name.
14. **`02-schema.md`** — SI-8's tier T0/T1 → T1 in §7.5 and §9 row 4.
15. **`04-operators.md`** — §1.2 note ²'s claim that 11 §6.1 does not schedule `UsdGenScale`.

---

## 10. Findings index

| # | File | Severity | Short |
|---|---|---|---|
| F-01 | 09 | high | SI-5 milestone M0 ≠ R40's M1 |
| F-02 | 09 | high | S-6 milestone M2 ≠ R40's M1 |
| F-03 | 09 | medium | six Status cells describe siblings that have already changed |
| F-04 | 09 | medium | SI-9 metric and test name disagree with 10 §6.5 |
| F-05 | 09 | high | `EV-nnn` citations use a scheme appendix A does not carry; two ids collide |
| F-06 | 09 | medium | §5/§8 test names diverge from 10 §5.6 |
| F-07 | 09 | medium | workstation-protocol item numbers disagree with 10 §5.2 and 08 §8.3 |
| F-08 | 11 | medium | §2.0's "absent from 09" and "rows R40 corrects" paragraphs are stale |
| F-09 | 11 | high | S-12 and S-11 missing from §2.0, §6.4 RC-1 and §7 |
| F-10 | 11 | medium | T-EXPR-1 / T-PTEX-1 missing from M4's exit set and RC-1 |
| F-11 | 11 | high | §5 carries no SC-11/SC-12 though 12 and 05 cite them |
| F-12 | 11 | medium | PW numbering conflicts with appendix B |
| F-13 | 11 | medium | contract-freeze test names absent from / renamed in 10 §5.6 |
| F-14 | 11 | low | "eighteen entry points" vs 08 §1.4's nineteen |
| F-15 | 11 | medium | target→milestone list conflicts with 10 §1.6 and 01 §5.2 |
| F-16 | 11 | medium | B1–B12 (T2 benchmark protocol) presented as the T4 workstation runner |
| F-17 | 00 | high | §5.4 gate-family list missing B-, SI-9, S-10…S-12, L-2…L-5, T-EXPR/T-PTEX (R44) |
| F-18 | 00 | medium | §7 calls T-2 a tier-T3 gate |
| F-19 | 00 | high | §2.1 version table predates R38; §2.3's claims about 04 are stale |
| F-20 | 00 | low | contingency wording matches neither ADR §7 nor 11 §4.1 |
| F-21 | 00 | low | `P1–P8` invariants and `usdGenPy` survive R1/R5 |
| F-22 | 12 | high | S-11 used for the tile sweep (canonical S-12) |
| F-23 | 12 | high | SI-10 requested where SI-9 already covers it |
| F-24 | 12 | medium | §5's four "reconciliations owed" are done |
| F-25 | 12 | high | five gate/tier cells in §5 disagree with 09 §5 |
| F-26 | 12 | medium | R2 row rejects `T-EXPR-*` while 09 §5.4 registers T-EXPR-1/T-PTEX-1 |
| F-27 | 12 | medium | SC-11 collides with 05's SC-11 and exists in no register |
| F-28 | 12 | low | A-19's claim about 11 §5.2 SC-4 is stale |
| F-29 | 12 | low | RK-29 cites 02 §7.3 and a routing both siblings already fixed |
| F-30 | appendix A | high | mints SI-10 and S-11 against the registry; stale B-1 note |
| F-31 | appendix A | medium | no row for 09's EV-312; §2.0 map does not cover 09's ids |
| F-32 | appendix B | high | PW-1…PW-13 numbering conflicts with 11 §1 |
| F-33 | appendix B | medium | "S-4/S-5/S-6 at M2" — S-5 and S-6 are M1 |
| F-34 | appendix B | low | T2 coverage stops at S-10; "SI-9 is PW-13" |
| F-35 | 06 | high | SI-5 M0, S-6 M2, four stale parentheticals, SI-3's extra threshold |
| F-36 | 05 | medium | SC-11 mint, `testUsdGenSkelPickup`, T2 driver names |
| F-37 | 03 | medium | §12/§12.1 cells and corrections describe superseded sibling text |
| F-38 | 03 | medium | SI-3 asserted "on the main thread" |
| F-39 | 08 | medium | §8.2 claims 10 §5.6's names verbatim but uses 09's |
| F-40 | 08 | medium | §8.1's "~15 ms for usdGen at 60 Hz" contradicts R41 / 09 §0.2 |
| F-41 | 07 | low | two stale "must land in 09" notes; one test name |
| F-42 | 02 | low | SI-8 tier given as T0/T1 |
| F-43 | 01 | medium | §0.3 restates the frame ledger with different numbers and a 1080p header |
| F-44 | 01 | medium | target→milestone list and a Hydra-linking `usdGenTestUtils` |
| F-45 | 10 | medium | §6.5 lacks S-11, S-12, T-EXPR-1, T-PTEX-1; §5.6 lacks four named tests |
| F-46 | 10 | low | SI-9's test name and metric narrower than 09's |
| F-47 | 04 | low | §1.2 note ² denies a schedule 11 §6.1 states |

---

## 11. What is consistent (checked, no finding)

* **Contracts C1–C5** freeze at the same milestone in all nine documents that state them.
* **Milestone vocabulary** is M0–M8 everywhere; no "S" or "P" phase names survive; motion profiles keep
  P0/P1/P2 and the engine invariants are I1–I8 in 01, 03, 09 (00 is the exception, F-21).
* **Estimates**: 33 + 1 = 34 weeks, 84 eng-weeks, and the three group totals agree across ADR §7, 11 §4.1,
  00 §2.3 and 12 A-30 (only the contingency *wording* differs, F-20).
* **The v1/v2/v3 boundary** agrees between 04, 11 and 12 (00 is the exception, F-19).
* **Numeric section cross-references**: all 468 `NN-…md §X` references resolve to a section that exists;
  the failures reported in §5 are topical or stale, not dangling.
* **Test tiers T0–T4** carry the same definitions in 00 §5.4, 01 §9, 02 §9, 03 §13, 04 §8, 05 §9, 06 §10,
  07 §9, 08 §9, 09 §5, 10 §5.1, 11 §0.3 and 12 §5, and every document repeats R39's rule that a T4 gate is
  never a milestone exit.
