"""Regenerate examples/pomade-braid-hierarchy.usda from the COMMITTER.

The braid example used to be hand-authored text, which is exactly how the
committer's missing hierarchy stayed hidden (plan/18 §7 G1). This script
drives the real pipeline instead: it builds the model through the C ABI,
lets PomadeCommitter serialise it into a live layer, and flattens the stage
that carries it. Every UsdGenTube, its nesting, its deltas and the Guides in
the output are bytes the tool wrote.

Run it with the OpenUSD python that usdGen builds against, with the pomade
DLL named:

    $env:USDGENPOMADE_DLL = "<usdgen-src>\\build\\usdGenPomade.dll"
    $env:PYTHONPATH = "<usdrig-src>\\usd-install\\lib\\python"
    python examples\\tools\\gen_pomade_braid_hierarchy.py
"""
from __future__ import annotations

import ctypes
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(_HERE))
sys.path.insert(0, os.path.join(_ROOT, "plugin", "usdGenPomadeTools",
                                "python"))

from pxr import Gf, Sdf, Usd, UsdGeom  # noqa: E402
from usdGenPomadeTools import pomadeLib  # noqa: E402
from usdGenPomadeTools import pomadeLibStage  # noqa: E402

OUT = os.path.join(_ROOT, "examples", "pomade-braid-hierarchy.usda")
GRID = 4  # a GRID x GRID quad scalp in the XZ plane

HEADER = """
# POMADE BRAID HIERARCHY (plan/17 P4 / plan/18 V0b exit)
#
# COMMITTER OUTPUT. Regenerate with:
#     python examples/tools/gen_pomade_braid_hierarchy.py
# Nothing below /PomadeGroom is hand-authored: PomadeCommitter wrote the tube
# hierarchy (nested prims, childIndex, subdivide:*, deltas, the
# UsdGenTubeHierarchyAPI on the kept on-the-fly parent) and the Guides
# (K8/K9/K10 per leaf tube). PomadeHydrateModel reads this file back into an
# identical model, which is what testUsdGenPomadeCommit asserts.
#
#   PomadeGroom/Tubes/tube0                  L1 tube, rooted in region 0
#   PomadeGroom/Tubes/tube0/tube1, tube2     its two L2 children (k-means)
#   .../tube1/tube17, .../tube1/tube18      two L3 grandchildren
#   PomadeGroom/Tubes/group1                 the kept on-the-fly parent over
#                                           the two L2 children (members +
#                                           persistent)
# Prim names carry the model's stable tube ids (child = parent * 16 + 1 +
# childIndex), which is what lets hydrate re-mint them.
#   PomadeGroom/Guides                one fill per LEAF tube; a subdivided
#                                    parent's own fill is suspended (§2.3)
#   Groom/Hair                       the usdGen description the committer
#                                    filled in with a GuideInterpolate op
#
# View it:   .\\bin\\launch_usdview.ps1 examples\\pomade-braid-hierarchy.usda
"""


def _bindExtras(dll):
    """The entry points pomadeLib does not bind yet (committer + hierarchy)."""
    cvp = ctypes.c_void_p
    cip = ctypes.POINTER(ctypes.c_int)
    cfp = ctypes.POINTER(ctypes.c_float)
    ccp = ctypes.c_char_p
    dll.Pomade_SubdivideTube.argtypes = [cvp, ctypes.c_int, ctypes.c_int, ccp,
                                        ctypes.c_int, cip, ctypes.c_int, cip]
    dll.Pomade_SubdivideTube.restype = ctypes.c_int
    dll.Pomade_GroupTubes.argtypes = [cvp, cip, ctypes.c_int, ctypes.c_int, cip]
    dll.Pomade_GroupTubes.restype = ctypes.c_int
    dll.Pomade_MakePersistent.argtypes = [cvp, ctypes.c_int]
    dll.Pomade_MakePersistent.restype = ctypes.c_int
    dll.Pomade_MoveTubeCenterCV.argtypes = [cvp, ctypes.c_int, ctypes.c_int,
                                           ctypes.c_float, ctypes.c_float,
                                           ctypes.c_float]
    dll.Pomade_MoveTubeCenterCV.restype = ctypes.c_int
    dll.Pomade_CommitterCreate.argtypes = [cvp, ccp, ccp,
                                          ctypes.POINTER(ctypes.c_void_p)]
    dll.Pomade_CommitterCreate.restype = ctypes.c_int
    dll.Pomade_CommitterEnqueue.argtypes = [cvp, ctypes.c_int, ctypes.c_int,
                                           ctypes.c_int, ccp]
    dll.Pomade_CommitterEnqueue.restype = ctypes.c_int
    dll.Pomade_CommitterSwap.argtypes = [cvp, ccp, ctypes.c_int]
    dll.Pomade_CommitterSwap.restype = ctypes.c_int
    dll.Pomade_CommitterCommittedVersion.argtypes = [cvp]
    dll.Pomade_CommitterCommittedVersion.restype = ctypes.c_ulonglong
    dll.Pomade_CommitterDestroy.argtypes = [cvp]
    dll.Pomade_CommitterDestroy.restype = ctypes.c_int
    _ = cfp  # kept for symmetry with the other binders


def _check(lib, status, what):
    if status != pomadeLib.POMADE_OK:
        raise RuntimeError("%s failed: %s" % (what, lib.lastError()))


def _gridMesh():
    points, counts, indices = [], [], []
    for ix in range(GRID + 1):
        for iz in range(GRID + 1):
            points.extend((float(ix), 0.0, float(iz)))

    def pid(ix, iz):
        return ix * (GRID + 1) + iz

    for ix in range(GRID):
        for iz in range(GRID):
            counts.append(4)
            indices.extend((pid(ix, iz), pid(ix, iz + 1),
                            pid(ix + 1, iz + 1), pid(ix + 1, iz)))
    return points, counts, indices


def _locate(x, z):
    """(faceId, u, v) for a point on the grid, in the PomadeFacePosition
    encoding the graph ABI takes."""
    ix = min(max(int(x), 0), GRID - 1)
    iz = min(max(int(z), 0), GRID - 1)
    return ix * GRID + iz, z - iz, x - ix


def _buildGraph(lib, dll, ctx):
    """Two adjoining rectangles: region 0 over x in [0, 2], region 1 over
    x in [2, 4], both z in [1, 3]."""
    nodes = []
    for (x, z) in ((0.0, 1.0), (2.0, 1.0), (2.0, 3.0), (0.0, 3.0),
                   (4.0, 1.0), (4.0, 3.0)):
        face, u, v = _locate(x, z)
        out = ctypes.c_int(0)
        _check(lib, dll.Pomade_GraphAddNode(ctx, face, u, v,
                                           ctypes.byref(out)),
               "Pomade_GraphAddNode")
        nodes.append(out.value)
    edges = ((0, 1), (1, 2), (2, 3), (3, 0), (1, 4), (4, 5), (5, 2))
    for a, b in edges:
        out = ctypes.c_int(0)
        _check(lib, dll.Pomade_GraphConnect(ctx, nodes[a], nodes[b],
                                           ctypes.byref(out)),
               "Pomade_GraphConnect")
    _check(lib, dll.Pomade_Rasterise(ctx), "Pomade_Rasterise")


def _subdivide(lib, dll, ctx, tubeId, count, seed):
    ids = (ctypes.c_int * 8)()
    got = ctypes.c_int(0)
    _check(lib, dll.Pomade_SubdivideTube(ctx, tubeId, count, b"kmeans", seed,
                                        ids, 8, ctypes.byref(got)),
           "Pomade_SubdivideTube")
    return [ids[i] for i in range(got.value)]


def _scaffold(stage, points, counts, indices):
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    UsdGeom.SetStageMetersPerUnit(stage, 1.0)
    world = UsdGeom.Xform.Define(stage, "/World")
    stage.SetDefaultPrim(world.GetPrim())
    camera = UsdGeom.Camera.Define(stage, "/World/Cam")
    camera.AddTransformOp().Set(Gf.Matrix4d(
        0.749, 0.0, -0.663, 0.0,
        -0.166, 0.968, -0.188, 0.0,
        0.641, 0.251, 0.725, 0.0,
        4.6, 3.0, 6.0, 1.0))
    mesh = UsdGeom.Mesh.Define(stage, "/World/Scalp")
    mesh.CreatePointsAttr([Gf.Vec3f(points[i * 3], points[i * 3 + 1],
                                    points[i * 3 + 2])
                           for i in range(len(points) // 3)])
    mesh.CreateFaceVertexCountsAttr(counts)
    mesh.CreateFaceVertexIndicesAttr(indices)
    mesh.CreateExtentAttr([Gf.Vec3f(0, 0, 0), Gf.Vec3f(GRID, 0, GRID)])
    stage.DefinePrim("/Groom", "Scope")
    stage.DefinePrim("/Groom/Hair", "UsdGenDescription")
    stage.DefinePrim("/Groom/Hair/Ops", "Scope")


def main():
    lib = pomadeLib.Library()
    stageLib = pomadeLibStage.StageLibrary(lib)
    dll = lib.dll
    _bindExtras(dll)

    ctx = ctypes.c_void_p()
    _check(lib, dll.Pomade_Create(ctypes.byref(ctx)), "Pomade_Create")
    points, counts, indices = _gridMesh()
    cpoints = (ctypes.c_float * len(points))(*points)
    ccounts = (ctypes.c_int * len(counts))(*counts)
    cindices = (ctypes.c_int * len(indices))(*indices)
    _check(lib, dll.Pomade_BindScalp(ctx, cpoints, len(points), ccounts,
                                    len(counts), cindices, len(indices)),
           "Pomade_BindScalp")
    _buildGraph(lib, dll, ctx)

    # One L1 tube over region 0, then the braid: two L2 children, two L3
    # grandchildren under the first, a sculpted L2 sibling, and a kept
    # on-the-fly parent over the two L2 children.
    _check(lib, dll.Pomade_BuildTubeFromRegion(ctx, 0, 5, 8, 3.0),
           "Pomade_BuildTubeFromRegion")
    _check(lib, dll.Pomade_SetFillParams(ctx, 24.0, 8, 5, 0.0, None, 0),
           "Pomade_SetFillParams")
    kids = _subdivide(lib, dll, ctx, 0, 2, 3)
    grand = _subdivide(lib, dll, ctx, kids[0], 2, 7)
    _check(lib, dll.Pomade_MoveTubeCenterCV(ctx, kids[1], 3, 0.18, 0.0, 0.10),
           "Pomade_MoveTubeCenterCV")
    _check(lib, dll.Pomade_MoveTubeCenterCV(ctx, grand[1], 4, -0.08, 0.0,
                                           0.12),
           "Pomade_MoveTubeCenterCV")
    stageLib.setFillParams(ctx, grand[0], 10.0, 6, 11, 0.2)
    members = (ctypes.c_int * 2)(kids[0], kids[1])
    group = ctypes.c_int(0)
    _check(lib, dll.Pomade_GroupTubes(ctx, members, 2, 0,
                                     ctypes.byref(group)),
           "Pomade_GroupTubes")
    _check(lib, dll.Pomade_MakePersistent(ctx, group.value),
           "Pomade_MakePersistent")
    _check(lib, dll.Pomade_Rasterise(ctx), "Pomade_Rasterise")

    # The stage the committer writes into: scaffolding in the root layer,
    # the live layer sublayered under it so Flatten folds both together.
    stage = Usd.Stage.CreateInMemory("pomade-braid")
    _scaffold(stage, points, counts, indices)
    live = Sdf.Layer.CreateAnonymous("pomade-braid-live")
    stage.GetRootLayer().subLayerPaths.insert(0, live.identifier)

    committer = ctypes.c_void_p()
    _check(lib, dll.Pomade_CommitterCreate(ctx, b"/PomadeGroom", b"/Groom/Hair",
                                          ctypes.byref(committer)),
           "Pomade_CommitterCreate")
    _check(lib, dll.Pomade_CommitterSetScalpPath(committer, b"/World/Scalp"),
           "Pomade_CommitterSetScalpPath")
    _check(lib, dll.Pomade_CommitterEnqueue(committer, 1, 1, 1,
                                           b"/Groom/Hair/Ops/pomadeInterp"),
           "Pomade_CommitterEnqueue")
    want = dll.Pomade_GetVersion(ctx)
    for _ in range(20000):
        dll.Pomade_CommitterSwap(committer, live.identifier.encode("utf-8"), 0)
        if dll.Pomade_CommitterCommittedVersion(committer) >= want:
            break
    if dll.Pomade_CommitterCommittedVersion(committer) < want:
        raise RuntimeError("the committer never caught up with the model")

    flat = stage.Flatten()
    flat.documentation = (
        "Pomade V0b: an L1/L2/L3 braid hierarchy and its guides, written by "
        "PomadeCommitter and read back by PomadeHydrateModel.")
    text = flat.ExportToString()
    # Put the explanation under the layer metadata block, where the other
    # examples carry theirs.
    marker = "\n)\n"
    cut = text.index(marker) + len(marker)
    text = text[:cut] + HEADER + text[cut:]
    with open(OUT, "w", newline="\n") as handle:
        handle.write(text)
    print("wrote %s (%d bytes)" % (OUT, len(text)))

    # Prove the shipped file round-trips: hydrate it back into a fresh
    # model. A failure here means the example is not committer output.
    check = ctypes.c_void_p()
    _check(lib, dll.Pomade_Create(ctypes.byref(check)), "Pomade_Create")
    tubes, guides, imported = stageLib.hydrate(check, OUT, "/PomadeGroom")
    print("hydrate: %d tubes, %d guides, %d imported" % (tubes, guides,
                                                         imported))
    _check(lib, dll.Pomade_Destroy(check), "Pomade_Destroy")

    _check(lib, dll.Pomade_CommitterDestroy(committer),
           "Pomade_CommitterDestroy")
    _check(lib, dll.Pomade_Destroy(ctx), "Pomade_Destroy")


if __name__ == "__main__":
    main()
