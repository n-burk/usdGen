#!/usr/bin/env python3
# testTonicSoak -- TN-5: 30 min scripted editing on a reference-fanout
# model (36-tube L1 subtree + preview guides). NOT registered in CTest:
# T4 by design (30-minute wall clock, human-run per the workstation
# protocol). Run headless:
#
#   python testTonicSoak.py [--minutes 30] [--seed 7] [--smoke]
#
# --smoke runs 1 minute. Exit 0 (PASS) when no gesture op exceeds
# 33 ms, device memory does not grow, rejects stay capped (25 % of all
# iterations, 10 % of direct-edit attempts), and zero hard failures
# occur; exit 1 (FAIL) otherwise. Rejected edits (engine TONIC_ERROR
# with clean rollback) are artist-handled -- undo and continue -- while
# hard failures (unexpected exceptions, unrecoverable rejects, refill
# errors) fail the run. Full-density refills happen on release, not
# mid-gesture, so they are timed separately (informational) rather
# than against the frame budget.
import argparse
import ctypes
import importlib.util
import os
import random
import subprocess
import sys
import time

SRC = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "python", "usdGenTonicTools"))
FRAME_BUDGET_MS = 33.0


def _load(name):
    spec = importlib.util.spec_from_file_location(
        "soak_%s" % name, os.path.join(SRC, "%s.py" % name))
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _deviceMemUsed():
    """Device bytes in use, or None when unmeasurable."""
    for cuda in ("cudart64_12.dll", "cudart64_11.dll"):
        try:
            rt = ctypes.CDLL(cuda)
            break
        except OSError:
            rt = None
    if rt is not None:
        try:
            free = ctypes.c_size_t(0)
            total = ctypes.c_size_t(0)
            if rt.cudaMemGetInfo(ctypes.byref(free),
                                 ctypes.byref(total)) == 0:
                return total.value - free.value
        except (OSError, AttributeError):
            pass
    try:
        out = subprocess.run(
            ["nvidia-smi", "--query-gpu=memory.used",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=30, check=True)
        return int(out.stdout.splitlines()[0].strip()) * 1024 * 1024
    except (OSError, subprocess.SubprocessError, ValueError, IndexError):
        return None


def main():
    args = argparse.ArgumentParser(description="TN-5 model soak")
    args.add_argument("--minutes", type=float, default=30.0)
    args.add_argument("--seed", type=int, default=7)
    args.add_argument("--smoke", action="store_true",
                      help="1-minute smoke run")
    args.add_argument("--until", type=int, default=0, metavar="N",
                      help="replay to iteration N, dump state, time one "
                      "more op of each edit kind, exit (outlier forensics; "
                      "the RNG stream is deterministic per seed)")
    opts = args.parse_args()
    budget = 60.0 if opts.smoke else opts.minutes * 60.0
    rng = random.Random(opts.seed)
    replayUntil = opts.until

    tonicLib = _load("tonicLib")
    hier = _load("tonicHierarchy")
    sculpt = _load("tonicSculpt")
    lib = tonicLib.Library()
    dll = lib.dll

    ctx = ctypes.c_void_p(None)
    assert dll.Tonic_Create(ctypes.byref(ctx)) == 0, "create"
    assert dll.Tonic_BuildTestTube(ctx, 17, 16, 0.5, 4.0) == 0, "build"
    l1 = hier.subdivide(dll, ctx, 0, 5, "kmeans", 7)
    for i, kid in enumerate(l1):
        for seed in (11 + i, 101 + i, 201 + i):
            try:
                hier.subdivide(dll, ctx, kid, 6, "kmeans", seed)
                break
            except RuntimeError:
                continue
    assert hier.tubeCount(dll, ctx) == 36, "reference fanout"
    assert dll.Tonic_SetFillParams(ctx, ctypes.c_float(150.0), 16, 11,
                                   ctypes.c_float(0.0), None, 0) == 0
    assert dll.Tonic_RefillGuides(ctx, ctypes.c_float(0.25)) == 0

    vp = (ctypes.c_float * 16)(1, 0, 0, 0, 0, 1, 0, 0,
                               0, 0, 1, 0, 0, 0, 0, 1)
    hit = ctypes.c_int(0)
    kind = ctypes.c_uint(0)
    index = ctypes.c_int(0)
    sub = ctypes.c_int(0)
    dist = ctypes.c_float(0)
    depth = ctypes.c_float(0)

    def pick():
        # A miss still returns 0 (*outHit stays 0); nonzero is engine
        # breakage, surfaced as a hard failure by the loop handler.
        assert dll.Tonic_Pick(
            ctx, vp, 400, 400, ctypes.c_float(300.0),
            ctypes.c_float(200.0), ctypes.c_float(5.0), 31,
            ctypes.byref(hit), ctypes.byref(kind),
            ctypes.byref(index), ctypes.byref(sub),
            ctypes.byref(dist), ctypes.byref(depth)) == 0, "pick rc"

    # Deltas alternate sign per site (a drag that keeps coming back,
    # like the TN-1 probe): a random walk would drift a full tube radius
    # and break k-means re-partitioning, which is a shape failure, not a
    # soak failure. Parity MUST be per site: a global counter interleaved
    # across op types random-walks each CV (proven: 25K same-sign-ish hits
    # drifted a CV ~2 units and broke Sync).
    flip = {}

    def jiggle(site):
        flip[site] = not flip.get(site, False)
        s = 0.02 if flip[site] else -0.02
        return s * rng.uniform(0.5, 1.0)

    dll.Tonic_GetLastError.restype = ctypes.c_char_p

    def lastError():
        msg = dll.Tonic_GetLastError()
        return msg.decode("utf-8", "replace") if msg else ""

    def topologyCycle():
        # Merge one L1's children away and split them back (exercises
        # topology churn + undo of topology). The first seed retry is the
        # L1's own setup seed (like the repair loop): a foreign seed
        # measures chart luck, not engine fidelity.
        kid = rng.choice(l1)
        i = l1.index(kid)
        hier.mergeChildren(dll, ctx, kid)
        for seed in (11 + i, 101 + i, 201 + i):
            try:
                hier.subdivide(dll, ctx, kid, 6, "kmeans", seed)
                return
            except RuntimeError:
                continue
        raise RuntimeError("topology cycle could not resubdivide")

    def liveLeaves():
        # Undo can rewind past a topology op (children gone); topology
        # cycles restore them. Always pick from live state.
        return [c for kid in l1 for c in hier.tubeChildren(dll, ctx, kid)]

    def moveLeaf():
        found = liveLeaves()
        if not found:
            hier.moveTubeCenterCV(dll, ctx, 0, 8, jiggle("l1"), 0.0, 0.0)
        else:
            leaf = rng.choice(found)
            hier.moveTubeCenterCV(dll, ctx, leaf, 2,
                                  jiggle("m%d" % leaf), 0.0, 0.0)

    def sculptStroke():
        found = liveLeaves()
        if not found:
            hier.moveTubeCenterCV(dll, ctx, 0, 8, jiggle("l1"), 0.0, 0.0)
            return
        leaf = rng.choice(found)
        j = jiggle("s%d" % leaf)
        sculpt.stroke(dll, ctx, leaf, "grab", [2, 3],
                      [j, 0.0, 0.0, j, 0.0, 0.0])

    def previewRefill():
        # Refills have no shape-reject mode: any nonzero rc is engine
        # breakage, surfaced as a hard failure by the loop handler.
        assert dll.Tonic_RefillGuides(
            ctx, ctypes.c_float(0.25)) == 0, "preview refill rc"

    ops = {
        "move-l1": lambda: hier.moveTubeCenterCV(
            dll, ctx, 0, 8, jiggle("l1"), 0.0, 0.0),
        "move-leaf": moveLeaf,
        "sculpt": sculptStroke,
        "pick": pick,
        "preview-refill": previewRefill,
        # Undo's rc is ignored on purpose: an empty-stack undo is an
        # artist no-op, not a failure.
        "undo": lambda: dll.Tonic_Undo(ctx, None),
        "topology": topologyCycle,
    }
    # Warm up every op (allocates the high-water device caches) before
    # the memory baseline, so first-touch growth is not misread as a
    # leak.
    for warm in sorted(ops):
        try:
            ops[warm]()
        except RuntimeError:
            # Same artist handling as the loop: a rejected warmup edit
            # rewinds past the edit that led here.
            if dll.Tonic_GetUndoDepth(ctx) > 0:
                assert dll.Tonic_Undo(ctx, None) == 0, "warmup reject undo"
    # Warmup ends with an undo (past the topology resubdivide): repair
    # any childless L1 so the loop starts at full fanout, then PROVE it:
    # a soak that starts degraded proves nothing, so fail fast instead.
    for i, kid in enumerate(l1):
        if not hier.tubeChildren(dll, ctx, kid):
            for seed in (11 + i, 101 + i, 201 + i):
                try:
                    hier.subdivide(dll, ctx, kid, 6, "kmeans", seed)
                    break
                except RuntimeError:
                    continue
    assert hier.tubeCount(dll, ctx) == 36, "warmup must end at full fanout"
    for kid in l1:
        assert len(hier.tubeChildren(dll, ctx, kid)) == 6, \
            "warmup must restore every L1"
    # Rejected edits (the engine returning TONIC_ERROR with a clean
    # rollback, e.g. a K7-merged parent ring that no longer re-partitions
    # into its Voronoi chart) are artist-handled: undo the edit that led
    # here and continue. TN-5 gates frame budget + device memory, not a
    # zero-rejection rate; rejects are counted and capped instead (a soak
    # that rejects everything proves nothing about editing).
    # Attribution instrumentation (TN-5 gates frames "attributable to the
    # tool"): every sample keeps wall ms; over-budget samples also keep
    # thread-CPU ms -- wall >> CPU means the thread was descheduled
    # (OS/driver jitter, not tool work), wall ~= CPU means the tool
    # burned the cycles. Percentiles come from the full sample lists.
    fullRefills = []
    worst = {}
    worstCtx = {}
    counts = {name: 0 for name in ops}
    rejects = {name: 0 for name in ops}
    samples = {name: [] for name in ops}
    overBudget = {name: 0 for name in ops}
    failures = []

    # Every over-budget event with a wall-clock timestamp, for post-hoc
    # correlation against the out-of-process scheduler control (same box,
    # no shared GIL): coincident control overshoot = external stall.
    overEvents = []

    # NOTE: thread_time was tried for wall-vs-CPU attribution but its
    # 15.6 ms quantum on Windows cannot resolve sub-33 ms frames; the
    # timestamped over-budget events below (correlated post-hoc against
    # an out-of-process sleeper) are the attribution signal instead.
    def noteSample(name, wallMs, it, extra):
        samples[name].append(wallMs)
        if wallMs > FRAME_BUDGET_MS:
            overBudget[name] += 1
            overEvents.append((time.time(), name, wallMs, extra))
        if wallMs > worst.get(name, 0.0):
            worst[name] = wallMs
            reason = dll.Tonic_GetDeviceFallbackReason(ctx)
            worstCtx[name] = (wallMs, time.time(), it, extra,
                              hier.tubeCount(dll, ctx),
                              dll.Tonic_GetUndoDepth(ctx),
                              reason.decode("utf-8", "replace")
                              if reason else "")
    mem0 = _deviceMemUsed()
    start = time.perf_counter()
    it = 0
    while time.perf_counter() - start < budget:
        it += 1
        # Every 200th iteration is a release: full refill (timed apart).
        # A failed refill records a hard failure and runs on (an abort
        # here would discard the whole run's evidence for one bad op).
        if it % 200 == 0:
            t0 = time.perf_counter()
            rc = dll.Tonic_RefillGuides(ctx, ctypes.c_float(1.0))
            if rc != 0:
                failures.append("full-refill: HARD rc=%d [%s]"
                                % (rc, lastError()))
                continue
            fullRefills.append((time.perf_counter() - t0) * 1000.0)
            continue
        name = rng.choice(sorted(ops))
        t0 = time.perf_counter()
        try:
            ops[name]()
        except RuntimeError as exc:
            # Engine rejection (see _check): undo-to-recover, artist cost
            # folded into this op's frame time.
            undoDepth = dll.Tonic_GetUndoDepth(ctx)
            if undoDepth <= 0:
                failures.append("%s: %s [%s] (no undo to recover)"
                                % (name, exc, lastError()))
                continue
            assert dll.Tonic_Undo(ctx, None) == 0, "reject recovery undo"
            wallMs = (time.perf_counter() - t0) * 1000.0
            rejects[name] += 1
            noteSample(name, wallMs, it, "reject")
            if replayUntil and it == replayUntil:
                print("replay: it=%d %s %.3f ms (reject)" % (it, name, wallMs))
            if replayUntil and it >= replayUntil:
                break
            continue
        except Exception as exc:  # noqa: BLE001 - hard failure, still runs on
            failures.append("%s: HARD %r [%s]" % (name, exc, lastError()))
            continue
        wallMs = (time.perf_counter() - t0) * 1000.0
        counts[name] += 1
        noteSample(name, wallMs, it, "accept")
        if replayUntil and it == replayUntil:
            print("replay: it=%d %s %.3f ms (accept)" % (it, name, wallMs))
        if replayUntil and it >= replayUntil:
            break
    elapsed = time.perf_counter() - start
    if replayUntil:
        print("replay: stopped at it=%d tubes=%d undoDepth=%d" % (
            it, hier.tubeCount(dll, ctx), dll.Tonic_GetUndoDepth(ctx)))
        for kid in l1:
            print("replay:   l1 %d children: %s"
                  % (kid, hier.tubeChildren(dll, ctx, kid)))
        # Time one more op per edit kind against the replayed state; a
        # shape-driven worst case reproduces, a transient does not.
        for probe in ("move-l1", "move-leaf", "sculpt", "topology"):
            t0 = time.perf_counter()
            try:
                ops[probe]()
                print("replay: %-9s accept %.3f ms"
                      % (probe, (time.perf_counter() - t0) * 1000.0))
            except RuntimeError as exc:
                print("replay: %-9s reject %.3f ms (%s)"
                      % (probe, (time.perf_counter() - t0) * 1000.0, exc))
        dll.Tonic_Destroy(ctx)
        return 0
    mem1 = _deviceMemUsed()

    def pct(data, q):
        if not data:
            return 0.0
        ordered = sorted(data)
        return ordered[min(len(ordered) - 1, int(q * len(ordered)))]

    nReject = sum(rejects.values())
    print("soak: %d iterations in %.1f s (%d rejects, %d hard failures)"
          % (it, elapsed, nReject, len(failures)))
    for name in sorted(counts):
        print("soak: %-14s x%-6d +%4d rejects "
              "p50 %6.3f p99 %6.3f p999 %6.3f worst %7.3f ms (%d over)"
              % (name, counts[name], rejects[name],
                 pct(samples[name], 0.50), pct(samples[name], 0.99),
                 pct(samples[name], 0.999), worst.get(name, 0.0),
                 overBudget[name]))
    for name in sorted(worstCtx):
        wallMs, stamp, at, extra, tubes, depth, reason = worstCtx[name]
        print("soak: %-14s worst %.3f ms at %s it=%d %s "
              "tubes=%d undoDepth=%d fallback=%r"
              % (name, wallMs,
                 time.strftime("%H:%M:%S", time.localtime(stamp)),
                 at, extra, tubes, depth, reason))
    if overEvents:
        print("soak: %d over-budget events (first 50):" % len(overEvents))
        for stamp, name, ms, extra in overEvents[:50]:
            print("soak:   %s %-14s %7.3f ms %s"
                  % (time.strftime("%H:%M:%S", time.localtime(stamp)),
                     name, ms, extra))
    if fullRefills:
        print("soak: full refill x%-6d worst %7.3f ms (informational)"
              % (len(fullRefills), max(fullRefills)))
    ok = True
    for name, ms in sorted(worst.items()):
        if ms > FRAME_BUDGET_MS:
            print("FAIL: %s worst %.3f ms exceeds %.1f ms"
                  % (name, ms, FRAME_BUDGET_MS))
            ok = False
    if nReject > it // 4:
        print("FAIL: %d rejects exceed 25%% of %d iterations"
              % (nReject, it))
        ok = False
    # Edit-health guard: topology's merge->resplit rejects are the known
    # K7 one-way-door class (touched children merge to rings that no
    # longer re-partition); the direct-edit ops must stay nearly clean.
    editAtt = sum(counts[n] + rejects[n]
                  for n in ("move-l1", "move-leaf", "sculpt"))
    editRej = sum(rejects[n]
                  for n in ("move-l1", "move-leaf", "sculpt"))
    if editAtt and editRej > editAtt // 10:
        print("FAIL: %d edit rejects exceed 10%% of %d edit attempts"
              % (editRej, editAtt))
        ok = False
    if failures:
        print("FAIL: %d hard failures (first: %s)"
              % (len(failures), failures[0]))
        ok = False
    if mem0 is None or mem1 is None:
        print("soak: device memory unmeasurable here (no cudart/nvidia-smi)")
    else:
        print("soak: device used %.1f MB -> %.1f MB"
              % (mem0 / 1048576.0, mem1 / 1048576.0))
        if mem1 > mem0 + 4 * 1048576:
            print("FAIL: device memory grew by %.1f MB"
                  % ((mem1 - mem0) / 1048576.0))
            ok = False
    print("soak: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
