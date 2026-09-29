# usdGenPomadeTools.pomadeLib -- Qt-free ctypes binding of pomadeApi (P0).
#
# Follows the usdGenLib.py rules from plan/08-tools.md section 1.4: BLAS
# threads are pinned before the package's first "import numpy", and the DLL
# resolves from $USDGENPOMADE_DLL, else the install layout, else the repo
# build tree.
from __future__ import annotations

import ctypes
import os

for _v in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS"):
    os.environ.setdefault(_v, "1")     # S39: must precede the first "import numpy"

POMADE_OK = 0
POMADE_ERROR = 1
POMADE_NOT_IMPLEMENTED = 2

# PomadePickKind (pomadeTube.h): one enum for what a pick returns and what a
# selection holds. Eight kinds are screen-pickable; a hierarchy level is
# chosen from the breadcrumb and selects every tube at it.
POMADE_PICK_TUBE_VERT = 1 << 0
POMADE_PICK_CENTER_CV = 1 << 1
POMADE_PICK_SECTION_CV = 1 << 2
POMADE_PICK_GRAPH_NODE = 1 << 3
POMADE_PICK_GUIDE = 1 << 4
POMADE_PICK_GRAPH_EDGE = 1 << 5
POMADE_PICK_REGION = 1 << 6
POMADE_PICK_SECTION_RING = 1 << 7
POMADE_PICK_LEVEL = 1 << 8
POMADE_PICK_ALL = 0xFF
POMADE_SELECT_ALL = 0x1FF

# How a select call combines with what is already selected.
POMADE_SELECT_SET = 0
POMADE_SELECT_ADD = 1
POMADE_SELECT_TOGGLE = 2
# Python-only: the C ABI has no subtract yet, so pomadeLoops.selectItems /
# selectBand emulate it. Never pass it to a Pomade_Select* entry point.
POMADE_SELECT_REMOVE = 3

# PomadeGizmoKind (pomadeGizmo.h).
POMADE_GIZMO_NONE = 0
POMADE_GIZMO_TRANSLATE = 1
POMADE_GIZMO_RING_TRS = 2
POMADE_GIZMO_NODE_TRANSLATE = 3
POMADE_GIZMO_ROTATE = 4
POMADE_GIZMO_SCALE = 5

# PomadeDirty (pomadeModel.h): what Pomade_Publish is asked to republish ON TOP
# of the bits the model already reports pending. 0 -- POMADE_DIRTY_PENDING --
# publishes exactly the model's own pending set, which is what a gesture
# wants: the leaf-exact dirties are why a move stays inside its budget.
# POMADE_DIRTY_ALL is for the cases where something outside the model changed
# (a reattach, a fresh dock).
POMADE_DIRTY_PENDING = 0
POMADE_DIRTY_POINTS = 1 << 0
POMADE_DIRTY_TOPOLOGY = 1 << 1
POMADE_DIRTY_GRAPH = 1 << 2
POMADE_DIRTY_REGIONS = 1 << 3
POMADE_DIRTY_GUIDES = 1 << 4
POMADE_DIRTY_DISPLAY = 1 << 5
POMADE_DIRTY_SELECTION = 1 << 6
POMADE_DIRTY_GIZMO = 1 << 7
POMADE_DIRTY_BRUSH = 1 << 8
POMADE_DIRTY_ALL = 0x1FF

# Pomade_CommitterSwap result codes (pomadeApi.h).
POMADE_COMMITTER_SWAPPED = 0
POMADE_COMMITTER_PARTIAL = 1
POMADE_COMMITTER_NOTHING_PENDING = 2
POMADE_COMMITTER_SKIPPED_GESTURE = 3
POMADE_COMMITTER_SKIPPED_STALE = 4
POMADE_COMMITTER_DETACHED = 5
# The swap failed (unknown live layer, a throw): the reason is in
# Pomade_GetLastError. Negative, so it never reads as PARTIAL (SS-05).
POMADE_COMMITTER_ERROR = -1


def PomadeLibraryPath():
    """Resolve the usdGenPomade shared library, or "" when none exists."""
    override = os.environ.get("USDGENPOMADE_DLL", "")
    if override:
        return override
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = (
        # Installed layout: <prefix>/lib/python/usdGenPomadeTools -> <prefix>/lib.
        os.path.join(here, "..", "..", "..", "..", "lib"),
        # Build tree: <build>/python/usdGenPomadeTools -> <build>.
        os.path.join(here, "..", ".."),
    )
    libNames = ("usdGenPomade.dll", "libusdGenPomade.so", "libusdGenPomade.dylib")
    for directory in candidates:
        for name in libNames:
            path = os.path.normpath(os.path.join(directory, name))
            if os.path.isfile(path):
                return path
    return ""


def _dllSearchDirs(path):
    """Extra Windows DLL search dirs for usdGenPomade's dependencies."""
    dirs = []
    own = os.path.dirname(os.path.abspath(path))
    if os.path.isdir(own):
        dirs.append(own)
    # The CUDA runtime (cudart64_*.dll): the installer records it here.
    for var in ("CUDA_PATH",) + tuple(
            "CUDA_PATH_V%s" % v.replace(".", "_")
            for v in ("12_0", "12_1", "12_2", "12_3", "12_4", "12_5",
                      "12_6", "12_7", "12_8", "12_9")):
        root = os.environ.get(var, "")
        bindir = os.path.join(root, "bin") if root else ""
        if bindir and os.path.isdir(bindir) and bindir not in dirs:
            dirs.append(bindir)
    # The usd_*.dll core + its third-party bin: ctypes ignores PATH
    # (proven by bisect: even winmode=0 misses), so any PATH entry that
    # carries the core becomes an explicit cookie. Under CTest the
    # global runtime sweep prepends exactly those entries.
    for entry in os.environ.get("PATH", "").split(os.pathsep):
        entry = entry.strip().strip('"')
        if (entry and os.path.isdir(entry)
                and os.path.isfile(os.path.join(entry, "usd_tf.dll"))):
            for candidate in (entry,
                              os.path.join(os.path.dirname(entry), "bin")):
                if (os.path.isdir(candidate) and candidate not in dirs):
                    dirs.append(candidate)
    # The same pair next to pxr when it resolves to a USD install (its
    # <prefix>/lib/python/pxr anchor walks back up to <prefix>).
    try:
        import pxr  # noqa: F401 -- only its location is used
    except ImportError:
        pxr = None
    if pxr is not None:
        anchor = os.path.dirname(os.path.abspath(pxr.__file__))
        libdir = os.path.dirname(os.path.dirname(anchor))
        for candidate in (libdir,
                          os.path.join(os.path.dirname(libdir), "bin")):
            if os.path.isdir(candidate) and candidate not in dirs:
                dirs.append(candidate)
    return dirs


def _load(path):
    """CDLL(path), with its dependencies searchable on Windows.

    usdGenPomade imports usdGen from beside it plus the CUDA runtime, and
    since Python 3.8 a Windows CDLL searches neither the loaded
    library's own directory nor PATH for those (the same rule behind
    usdGenTools' exprApi._load). The plain load covers the C++ hosts
    (testusdview preloads everything); the retry opens the build dir,
    the CUDA bin and the USD lib/bin as explicit cookies, which is what
    plain python runs (the P5 T1) need. The retry keeps ctypes'
    default flags: winmode=0 was bisected to MISS here even with the
    cookies open.
    """
    try:
        return ctypes.CDLL(path)
    except OSError:
        if os.name != "nt" or not hasattr(os, "add_dll_directory"):
            raise
    cookies = []
    try:
        for directory in _dllSearchDirs(path):
            cookies.append(os.add_dll_directory(directory))
        return ctypes.CDLL(path)
    finally:
        for cookie in cookies:
            cookie.close()


class Library:
    """ctypes handle over pomadeApi.h. Raises OSError when unloadable."""

    def __init__(self, path=""):
        path = path or PomadeLibraryPath()
        if not path:
            raise OSError("usdGenPomade library not found "
                          "(set USDGENPOMADE_DLL to its path)")
        self._dll = _load(path)
        self._bind()

    def _bind(self):
        dll = self._dll
        dll.Pomade_Create.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
        dll.Pomade_Create.restype = ctypes.c_int
        dll.Pomade_Destroy.argtypes = [ctypes.c_void_p]
        dll.Pomade_Destroy.restype = ctypes.c_int
        dll.Pomade_BuildTestTube.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                            ctypes.c_int, ctypes.c_float,
                                            ctypes.c_float]
        dll.Pomade_BuildTestTube.restype = ctypes.c_int
        dll.Pomade_MoveCenterRing.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                             ctypes.c_float, ctypes.c_float]
        dll.Pomade_MoveCenterRing.restype = ctypes.c_int
        dll.Pomade_GetVersion.argtypes = [ctypes.c_void_p]
        dll.Pomade_GetVersion.restype = ctypes.c_ulonglong
        dll.Pomade_TakeDirty.argtypes = [ctypes.c_void_p]
        dll.Pomade_TakeDirty.restype = ctypes.c_int
        dll.Pomade_GetVertexCount.argtypes = [ctypes.c_void_p]
        dll.Pomade_GetVertexCount.restype = ctypes.c_int
        dll.Pomade_GetQuadCount.argtypes = [ctypes.c_void_p]
        dll.Pomade_GetQuadCount.restype = ctypes.c_int
        dll.Pomade_HasCudaMirror.argtypes = [ctypes.c_void_p]
        dll.Pomade_HasCudaMirror.restype = ctypes.c_int
        dll.Pomade_GetDeviceFallbackReason.argtypes = [ctypes.c_void_p]
        dll.Pomade_GetDeviceFallbackReason.restype = ctypes.c_char_p
        dll.Pomade_Undo.argtypes = [ctypes.c_void_p,
                                   ctypes.POINTER(ctypes.c_uint)]
        dll.Pomade_Undo.restype = ctypes.c_int
        dll.Pomade_GetUndoDepth.argtypes = [ctypes.c_void_p]
        dll.Pomade_GetUndoDepth.restype = ctypes.c_int
        dll.Pomade_GetUndoBytes.argtypes = [ctypes.c_void_p]
        dll.Pomade_GetUndoBytes.restype = ctypes.c_uint64
        dll.Pomade_SetUndoBudget.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                            ctypes.c_uint64]
        dll.Pomade_SetUndoBudget.restype = ctypes.c_int
        dll.Pomade_ClearUndo.argtypes = [ctypes.c_void_p]
        dll.Pomade_ClearUndo.restype = ctypes.c_int
        self._bindGraph(dll)
        self._bindTube(dll)
        self._bindViewport(dll)
        self._bindSelection(dll)
        self._bindBake(dll)
        self._bindCommitter(dll)
        dll.Pomade_GetLastError.argtypes = []
        dll.Pomade_GetLastError.restype = ctypes.c_char_p

    def _bindGraph(self, dll):
        cvp = ctypes.c_void_p
        cfp = ctypes.POINTER(ctypes.c_float)
        cip = ctypes.POINTER(ctypes.c_int)
        dll.Pomade_BindScalp.argtypes = [cvp, cfp, ctypes.c_int, cip,
                                        ctypes.c_int, cip, ctypes.c_int]
        dll.Pomade_BindScalp.restype = ctypes.c_int
        # A face GeomSubset scalp: the parent mesh's arrays plus its
        # parent-mesh face ids (plan/02 section 2.20).
        dll.Pomade_BindScalpSubset.argtypes = [cvp, cfp, ctypes.c_int, cip,
                                              ctypes.c_int, cip, ctypes.c_int,
                                              cip, ctypes.c_int]
        dll.Pomade_BindScalpSubset.restype = ctypes.c_int
        dll.Pomade_ReadScalpFaceActive.argtypes = [cvp, cip, ctypes.c_int, cip]
        dll.Pomade_ReadScalpFaceActive.restype = ctypes.c_int
        dll.Pomade_HasScalp.argtypes = [cvp]
        dll.Pomade_HasScalp.restype = ctypes.c_int
        dll.Pomade_Raycast.argtypes = [cvp, cfp, cfp, cip, cip, cfp, cfp, cfp]
        dll.Pomade_Raycast.restype = ctypes.c_int
        dll.Pomade_ClosestPoint.argtypes = [cvp, cfp, cip, cip, cfp, cfp, cfp]
        dll.Pomade_ClosestPoint.restype = ctypes.c_int
        dll.Pomade_GraphAddNode.argtypes = [cvp, ctypes.c_int, ctypes.c_float,
                                           ctypes.c_float, cip]
        dll.Pomade_GraphAddNode.restype = ctypes.c_int
        dll.Pomade_GraphMoveNode.argtypes = [cvp, ctypes.c_int, ctypes.c_int,
                                            ctypes.c_float, ctypes.c_float]
        dll.Pomade_GraphMoveNode.restype = ctypes.c_int
        dll.Pomade_GraphConnect.argtypes = [cvp, ctypes.c_int, ctypes.c_int,
                                           cip]
        dll.Pomade_GraphConnect.restype = ctypes.c_int
        dll.Pomade_GraphSplitEdge.argtypes = [cvp, ctypes.c_int, ctypes.c_int,
                                             ctypes.c_float, ctypes.c_float,
                                             cip]
        dll.Pomade_GraphSplitEdge.restype = ctypes.c_int
        dll.Pomade_GraphWeld.argtypes = [cvp, ctypes.c_int, ctypes.c_int]
        dll.Pomade_GraphWeld.restype = ctypes.c_int
        dll.Pomade_GraphWeldAll.argtypes = [cvp, ctypes.c_float, cip]
        dll.Pomade_GraphWeldAll.restype = ctypes.c_int
        dll.Pomade_GraphUnweld.argtypes = [cvp, ctypes.c_int, cip,
                                          ctypes.c_int, cip]
        dll.Pomade_GraphUnweld.restype = ctypes.c_int
        dll.Pomade_GraphDeleteEdge.argtypes = [cvp, ctypes.c_int]
        dll.Pomade_GraphDeleteEdge.restype = ctypes.c_int
        dll.Pomade_GraphDeleteNode.argtypes = [cvp, ctypes.c_int]
        dll.Pomade_GraphDeleteNode.restype = ctypes.c_int
        dll.Pomade_GraphSnapNode.argtypes = [cvp, cfp, ctypes.c_float]
        dll.Pomade_GraphSnapNode.restype = ctypes.c_int
        dll.Pomade_GraphSnapEdge.argtypes = [cvp, cfp, ctypes.c_float]
        dll.Pomade_GraphSnapEdge.restype = ctypes.c_int
        dll.Pomade_GraphLinkRegions.argtypes = [cvp, ctypes.c_int,
                                               ctypes.c_int]
        dll.Pomade_GraphLinkRegions.restype = ctypes.c_int
        dll.Pomade_GraphUnlinkRegions.argtypes = [cvp, ctypes.c_int,
                                                 ctypes.c_int]
        dll.Pomade_GraphUnlinkRegions.restype = ctypes.c_int
        dll.Pomade_GraphStroke.argtypes = [cvp, cip, cfp, ctypes.c_int,
                                          ctypes.c_float, ctypes.c_float, cip,
                                          ctypes.c_int, cip, cip, cip, cip]
        dll.Pomade_GraphStroke.restype = ctypes.c_int
        dll.Pomade_GraphCreateRegion.argtypes = [cvp, cip, cip, cfp,
                                                ctypes.c_int, cip]
        dll.Pomade_GraphCreateRegion.restype = ctypes.c_int
        dll.Pomade_GraphGetNode.argtypes = [cvp, ctypes.c_int, cip, cfp, cfp]
        dll.Pomade_GraphGetNode.restype = ctypes.c_int
        dll.Pomade_GraphGetEdge.argtypes = [cvp, ctypes.c_int, cip]
        dll.Pomade_GraphGetEdge.restype = ctypes.c_int
        dll.Pomade_GraphGetNodeDisplayPosition.argtypes = [cvp, ctypes.c_int,
                                                           cfp]
        dll.Pomade_GraphGetNodeDisplayPosition.restype = ctypes.c_int
        dll.Pomade_GraphMoveNodes.argtypes = [cvp, cip, cip, cfp, ctypes.c_int]
        dll.Pomade_GraphMoveNodes.restype = ctypes.c_int
        dll.Pomade_GraphMirrorX.argtypes = [cvp, cip, ctypes.c_int, cip]
        dll.Pomade_GraphMirrorX.restype = ctypes.c_int
        dll.Pomade_SetSnapRadius.argtypes = [cvp, ctypes.c_float]
        dll.Pomade_SetSnapRadius.restype = ctypes.c_int
        dll.Pomade_GetSnapRadius.argtypes = [cvp]
        dll.Pomade_GetSnapRadius.restype = ctypes.c_float
        dll.Pomade_SetMirrorX.argtypes = [cvp, ctypes.c_int]
        dll.Pomade_SetMirrorX.restype = ctypes.c_int
        dll.Pomade_GetMirrorX.argtypes = [cvp]
        dll.Pomade_GetMirrorX.restype = ctypes.c_int
        dll.Pomade_Rasterise.argtypes = [cvp]
        dll.Pomade_Rasterise.restype = ctypes.c_int
        dll.Pomade_RegionAtSurface.argtypes = [cvp, ctypes.c_int,
                                              ctypes.c_float, ctypes.c_float]
        dll.Pomade_RegionAtSurface.restype = ctypes.c_int
        dll.Pomade_GetMapVersion.argtypes = [cvp]
        dll.Pomade_GetMapVersion.restype = ctypes.c_ulonglong
        dll.Pomade_GetGraphCounts.argtypes = [cvp, cip, cip, cip]
        dll.Pomade_GetGraphCounts.restype = ctypes.c_int
        dll.Pomade_GetRegionStats.argtypes = [cvp, cip, cip, cip]
        dll.Pomade_GetRegionStats.restype = ctypes.c_int
        dll.Pomade_ReadFaceRegions.argtypes = [cvp, cip, ctypes.c_int, cip]
        dll.Pomade_ReadFaceRegions.restype = ctypes.c_int
        dll.Pomade_ReadFaceRegionIds.argtypes = [cvp, cip, ctypes.c_int, cip]
        dll.Pomade_ReadFaceRegionIds.restype = ctypes.c_int
        dll.Pomade_ReadGraphNodes.argtypes = [cvp, cip, cfp, cfp,
                                             ctypes.c_int, cip]
        dll.Pomade_ReadGraphNodes.restype = ctypes.c_int
        dll.Pomade_ReadGraphEdges.argtypes = [cvp, cip, ctypes.c_int, cip]
        dll.Pomade_ReadGraphEdges.restype = ctypes.c_int
        dll.Pomade_ReadRegionColors.argtypes = [cvp, cfp, ctypes.c_int, cip]
        dll.Pomade_ReadRegionColors.restype = ctypes.c_int
        dll.Pomade_ReadRegionLoops.argtypes = [cvp, cip, ctypes.c_int, cip,
                                              ctypes.c_int, cip, cip]
        dll.Pomade_ReadRegionLoops.restype = ctypes.c_int

    def _bindTube(self, dll):
        cvp = ctypes.c_void_p
        cfp = ctypes.POINTER(ctypes.c_float)
        cip = ctypes.POINTER(ctypes.c_int)
        cup = ctypes.POINTER(ctypes.c_uint)
        dll.Pomade_BuildTubeFromRegion.argtypes = [
            cvp, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_float]
        dll.Pomade_BuildTubeFromRegion.restype = ctypes.c_int
        # V6 (plan/18 section 7 G14): one L1 tube per closed region.
        dll.Pomade_ReadL1TubeIds.argtypes = [cvp, cip, ctypes.c_int, cip]
        dll.Pomade_ReadL1TubeIds.restype = ctypes.c_int
        dll.Pomade_TubeForRegion.argtypes = [cvp, ctypes.c_int]
        dll.Pomade_TubeForRegion.restype = ctypes.c_int
        dll.Pomade_RegionForTube.argtypes = [cvp, ctypes.c_int]
        dll.Pomade_RegionForTube.restype = ctypes.c_int
        dll.Pomade_SyncRegionTubes.argtypes = [cvp, cip, cip]
        dll.Pomade_SyncRegionTubes.restype = ctypes.c_int
        dll.Pomade_MoveCenterCV.argtypes = [
            cvp, ctypes.c_int, ctypes.c_float, ctypes.c_float, ctypes.c_float]
        dll.Pomade_MoveCenterCV.restype = ctypes.c_int
        dll.Pomade_InsertCenterCV.argtypes = [cvp, ctypes.c_int]
        dll.Pomade_InsertCenterCV.restype = ctypes.c_int
        dll.Pomade_DeleteCenterCV.argtypes = [cvp, ctypes.c_int]
        dll.Pomade_DeleteCenterCV.restype = ctypes.c_int
        dll.Pomade_SetTubeLength.argtypes = [cvp, ctypes.c_float]
        dll.Pomade_SetTubeLength.restype = ctypes.c_int
        dll.Pomade_MatchSurface.argtypes = [cvp]
        dll.Pomade_MatchSurface.restype = ctypes.c_int
        dll.Pomade_GetCenterCVCount.argtypes = [cvp]
        dll.Pomade_GetCenterCVCount.restype = ctypes.c_int
        dll.Pomade_GetCenterCV.argtypes = [cvp, ctypes.c_int, cfp]
        dll.Pomade_GetCenterCV.restype = ctypes.c_int
        dll.Pomade_GetSectionCount.argtypes = [cvp]
        dll.Pomade_GetSectionCount.restype = ctypes.c_int
        dll.Pomade_GetSection.argtypes = [
            cvp, ctypes.c_int, cfp, cfp, ctypes.c_int, cip, cfp, cfp]
        dll.Pomade_GetSection.restype = ctypes.c_int
        dll.Pomade_MoveSectionRing.argtypes = [
            cvp, ctypes.c_int, ctypes.c_float, ctypes.c_float]
        dll.Pomade_MoveSectionRing.restype = ctypes.c_int
        dll.Pomade_ScaleSectionRing.argtypes = [
            cvp, ctypes.c_int, ctypes.c_float]
        dll.Pomade_ScaleSectionRing.restype = ctypes.c_int
        dll.Pomade_TwistSectionRing.argtypes = [
            cvp, ctypes.c_int, ctypes.c_float]
        dll.Pomade_TwistSectionRing.restype = ctypes.c_int
        dll.Pomade_MoveSectionCV.argtypes = [
            cvp, ctypes.c_int, ctypes.c_int, ctypes.c_float, ctypes.c_float]
        dll.Pomade_MoveSectionCV.restype = ctypes.c_int
        dll.Pomade_AddSectionRing.argtypes = [cvp, ctypes.c_float, cip]
        dll.Pomade_AddSectionRing.restype = ctypes.c_int
        dll.Pomade_RemoveSectionRing.argtypes = [cvp, ctypes.c_int]
        dll.Pomade_RemoveSectionRing.restype = ctypes.c_int
        dll.Pomade_CopySectionRing.argtypes = [
            cvp, ctypes.c_int, ctypes.c_int]
        dll.Pomade_CopySectionRing.restype = ctypes.c_int
        dll.Pomade_SetSoftSelection.argtypes = [
            cvp, ctypes.c_float, ctypes.c_float]
        dll.Pomade_SetSoftSelection.restype = ctypes.c_int
        dll.Pomade_GetSoftSelection.argtypes = [cvp, cfp, cfp]
        dll.Pomade_GetSoftSelection.restype = ctypes.c_int
        dll.Pomade_RelaxCenter.argtypes = [
            cvp, ctypes.c_float, ctypes.c_int]
        dll.Pomade_RelaxCenter.restype = ctypes.c_int
        dll.Pomade_SnapRootToScalp.argtypes = [cvp]
        dll.Pomade_SnapRootToScalp.restype = ctypes.c_int
        dll.Pomade_SetDisplaySegments.argtypes = [cvp, ctypes.c_int]
        dll.Pomade_SetDisplaySegments.restype = ctypes.c_int
        dll.Pomade_GetDisplaySegments.argtypes = [cvp]
        dll.Pomade_GetDisplaySegments.restype = ctypes.c_int
        dll.Pomade_SetTubeRegionId.argtypes = [cvp, ctypes.c_int]
        dll.Pomade_SetTubeRegionId.restype = ctypes.c_int
        dll.Pomade_GetTubeRegionId.argtypes = [cvp]
        dll.Pomade_GetTubeRegionId.restype = ctypes.c_int
        dll.Pomade_ReadTubeRegionFaces.argtypes = [cvp, cip, ctypes.c_int, cip]
        dll.Pomade_ReadTubeRegionFaces.restype = ctypes.c_int
        dll.Pomade_SetFillParams.argtypes = [
            cvp, ctypes.c_float, ctypes.c_int, ctypes.c_int, ctypes.c_float,
            cfp, ctypes.c_int]
        dll.Pomade_SetFillParams.restype = ctypes.c_int
        dll.Pomade_GetFillParams.argtypes = [
            cvp, cfp, cip, cip, cfp, cfp, ctypes.c_int, cip]
        dll.Pomade_GetFillParams.restype = ctypes.c_int
        dll.Pomade_SetPreviewFraction.argtypes = [cvp, ctypes.c_float]
        dll.Pomade_SetPreviewFraction.restype = ctypes.c_int
        dll.Pomade_GetPreviewFraction.argtypes = [cvp]
        dll.Pomade_GetPreviewFraction.restype = ctypes.c_float
        dll.Pomade_SetFreezeRoots.argtypes = [cvp, ctypes.c_int]
        dll.Pomade_SetFreezeRoots.restype = ctypes.c_int
        dll.Pomade_GetFreezeRoots.argtypes = [cvp]
        dll.Pomade_GetFreezeRoots.restype = ctypes.c_int
        dll.Pomade_RefillGuides.argtypes = [cvp, ctypes.c_float]
        dll.Pomade_RefillGuides.restype = ctypes.c_int
        dll.Pomade_GenerateGuides.argtypes = [cvp, ctypes.c_float]
        dll.Pomade_GenerateGuides.restype = ctypes.c_int
        dll.Pomade_ClearGeneratedCurves.argtypes = [cvp]
        dll.Pomade_ClearGeneratedCurves.restype = ctypes.c_int
        dll.Pomade_SetGeneratedCurvesVisible.argtypes = [cvp, ctypes.c_int]
        dll.Pomade_SetGeneratedCurvesVisible.restype = ctypes.c_int
        dll.Pomade_GetGeneratedCurvesVisible.argtypes = [cvp, cip]
        dll.Pomade_GetGeneratedCurvesVisible.restype = ctypes.c_int
        dll.Pomade_GetGuideCounts.argtypes = [cvp, cip, cip]
        dll.Pomade_GetGuideCounts.restype = ctypes.c_int
        # Tubes the last refill skipped (tube ids + per-index reason): a
        # refill that loses some tubes still returns 0. Guarded so a DLL
        # predating the entry still binds.
        if hasattr(dll, "Pomade_ReadRefillDrops"):
            dll.Pomade_ReadRefillDrops.argtypes = [cvp, cip, ctypes.c_int,
                                                  cip]
            dll.Pomade_ReadRefillDrops.restype = ctypes.c_int
            dll.Pomade_GetRefillDropReason.argtypes = [cvp, ctypes.c_int]
            dll.Pomade_GetRefillDropReason.restype = ctypes.c_char_p
        dll.Pomade_ReadGuidePreview.argtypes = [
            cvp, cfp, ctypes.c_int, cip, ctypes.c_int, cip]
        dll.Pomade_ReadGuidePreview.restype = ctypes.c_int
        dll.Pomade_ReadGuideRoots.argtypes = [
            cvp, cip, cfp, cfp, ctypes.c_int, cip]
        dll.Pomade_ReadGuideRoots.restype = ctypes.c_int
        dll.Pomade_Pick.argtypes = [
            cvp, cfp, ctypes.c_int, ctypes.c_int, ctypes.c_float,
            ctypes.c_float, ctypes.c_float, ctypes.c_uint, cip, cup, cip,
            cip, cfp, cfp]
        dll.Pomade_Pick.restype = ctypes.c_int

    def _bindViewport(self, dll):
        """The V0 publication ABI (plan/18 sections 2.1, 2.2).

        Pomade_Publish is the only way the viewport learns about a model
        change, so every mutating call in a gesture is followed by one.
        Note its return convention: the number of scene indices refreshed
        (-1 on error), not a status code.
        """
        cvp = ctypes.c_void_p
        cfp = ctypes.POINTER(ctypes.c_float)
        cip = ctypes.POINTER(ctypes.c_int)
        dll.Pomade_GetModelId.argtypes = [cvp]
        dll.Pomade_GetModelId.restype = ctypes.c_int
        dll.Pomade_Activate.argtypes = [cvp]
        dll.Pomade_Activate.restype = ctypes.c_int
        dll.Pomade_Deactivate.argtypes = [cvp]
        dll.Pomade_Deactivate.restype = ctypes.c_int
        dll.Pomade_Publish.argtypes = [cvp, ctypes.c_uint]
        dll.Pomade_Publish.restype = ctypes.c_int
        dll.Pomade_SetAmplifiedHair.argtypes = [cvp, ctypes.c_int]
        dll.Pomade_SetAmplifiedHair.restype = ctypes.c_int
        dll.Pomade_GetAmplifiedHair.argtypes = [cvp]
        dll.Pomade_GetAmplifiedHair.restype = ctypes.c_int
        # Optional until the Output-description native extension is present;
        # headless panel fakes and older plugin DLLs keep their old surface.
        outputSet = getattr(dll, "Pomade_SetOutputSettings", None)
        if outputSet is not None:
            outputSet.argtypes = [cvp, ctypes.c_int, ctypes.c_float,
                                  ctypes.c_float]
            outputSet.restype = ctypes.c_int
        outputGet = getattr(dll, "Pomade_GetOutputSettings", None)
        if outputGet is not None:
            outputGet.argtypes = [cvp, ctypes.POINTER(ctypes.c_int),
                                  ctypes.POINTER(ctypes.c_float),
                                  ctypes.POINTER(ctypes.c_float)]
            outputGet.restype = ctypes.c_int
        dll.Pomade_SetRingDisplay.argtypes = [cvp, ctypes.c_int]
        dll.Pomade_SetRingDisplay.restype = ctypes.c_int
        dll.Pomade_GetRingDisplay.argtypes = [cvp]
        dll.Pomade_GetRingDisplay.restype = ctypes.c_int
        dll.Pomade_SetLevelDisplay.argtypes = [
            cvp, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        dll.Pomade_SetLevelDisplay.restype = ctypes.c_int
        dll.Pomade_GetLevelDisplay.argtypes = [cvp, ctypes.c_int, cip, cip]
        dll.Pomade_GetLevelDisplay.restype = ctypes.c_int
        dll.Pomade_SetFocusLevel.argtypes = [cvp, ctypes.c_int]
        dll.Pomade_SetFocusLevel.restype = ctypes.c_int
        dll.Pomade_GetFocusLevel.argtypes = [cvp]
        dll.Pomade_GetFocusLevel.restype = ctypes.c_int
        dll.Pomade_SetActiveCutEnabled.argtypes = [cvp, ctypes.c_int]
        dll.Pomade_SetActiveCutEnabled.restype = ctypes.c_int
        dll.Pomade_GetActiveCutEnabled.argtypes = [cvp]
        dll.Pomade_GetActiveCutEnabled.restype = ctypes.c_int
        dll.Pomade_SetTubeExpanded.argtypes = [
            cvp, ctypes.c_int, ctypes.c_int]
        dll.Pomade_SetTubeExpanded.restype = ctypes.c_int
        dll.Pomade_GetTubeExpanded.argtypes = [cvp, ctypes.c_int]
        dll.Pomade_GetTubeExpanded.restype = ctypes.c_int
        dll.Pomade_IsTubeVisible.argtypes = [cvp, ctypes.c_int]
        dll.Pomade_IsTubeVisible.restype = ctypes.c_int
        dll.Pomade_SetDisplayScale.argtypes = [cvp, ctypes.c_float]
        dll.Pomade_SetDisplayScale.restype = ctypes.c_int
        dll.Pomade_GetDisplayScale.argtypes = [cvp, cfp]
        dll.Pomade_GetDisplayScale.restype = ctypes.c_int
        dll.Pomade_GetPublishedLevelInfo.argtypes = [
            cvp, ctypes.c_int, cip, cip, cip]
        dll.Pomade_GetPublishedLevelInfo.restype = ctypes.c_int
        # V9 (plan/18 section 2.4a): the whole viewport look of a mode in
        # one call. The table lives in C++ (PomadePolicyLevelDisplay), so
        # Python only says which mode, sub-mode and focus level it is in.
        dll.Pomade_SetDisplayPolicy.argtypes = [
            cvp, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        dll.Pomade_SetDisplayPolicy.restype = ctypes.c_int
        dll.Pomade_GetLevelDraw.argtypes = [cvp, ctypes.c_int, cfp, cip]
        dll.Pomade_GetLevelDraw.restype = ctypes.c_int
        dll.Pomade_SetGroomPath.argtypes = [cvp, ctypes.c_char_p]
        dll.Pomade_SetGroomPath.restype = ctypes.c_int
        dll.Pomade_GetGroomPath.argtypes = [cvp, ctypes.c_char_p, ctypes.c_int]
        dll.Pomade_GetGroomPath.restype = ctypes.c_int

    def _bindSelection(self, dll):
        """The V1 selection, overlay and gesture ABI (plan/18 sections 2.3
        to 2.5).

        Kind bits mirror PomadePickKind in pomadeTube.h, so a pick result and
        a selection call speak the same enum. POMADE_SELECT_* below is the
        combine mode a rubber band or a click applies.
        """
        cvp = ctypes.c_void_p
        cfp = ctypes.POINTER(ctypes.c_float)
        cip = ctypes.POINTER(ctypes.c_int)
        cup = ctypes.POINTER(ctypes.c_uint)
        dll.Pomade_SelectClear.argtypes = [cvp, ctypes.c_uint]
        dll.Pomade_SelectClear.restype = ctypes.c_int
        for name in ("Pomade_SelectSet", "Pomade_SelectAdd",
                     "Pomade_SelectToggle"):
            entry = getattr(dll, name)
            entry.argtypes = [cvp, ctypes.c_uint, cip, cip, cip,
                              ctypes.c_int]
            entry.restype = ctypes.c_int
        dll.Pomade_SelectRect.argtypes = [
            cvp, cfp, ctypes.c_int, ctypes.c_int, ctypes.c_float,
            ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_uint,
            ctypes.c_int]
        dll.Pomade_SelectRect.restype = ctypes.c_int
        dll.Pomade_SelectPolygon.argtypes = [
            cvp, cfp, ctypes.c_int, ctypes.c_int, cfp, ctypes.c_int,
            ctypes.c_uint, ctypes.c_int]
        dll.Pomade_SelectPolygon.restype = ctypes.c_int
        dll.Pomade_SetHover.argtypes = [cvp, ctypes.c_uint, ctypes.c_int,
                                       ctypes.c_int, ctypes.c_int]
        dll.Pomade_SetHover.restype = ctypes.c_int
        dll.Pomade_GetHover.argtypes = [cvp, cup, cip, cip, cip]
        dll.Pomade_GetHover.restype = ctypes.c_int
        dll.Pomade_ReadSelection.argtypes = [cvp, ctypes.c_uint, cip, cip,
                                            cip, ctypes.c_int, cip]
        dll.Pomade_ReadSelection.restype = ctypes.c_int
        dll.Pomade_GetSelectionCount.argtypes = [cvp, ctypes.c_uint]
        dll.Pomade_GetSelectionCount.restype = ctypes.c_int
        dll.Pomade_GetSelectionBounds.argtypes = [cvp, cfp, cfp]
        dll.Pomade_GetSelectionBounds.restype = ctypes.c_int
        dll.Pomade_PickItem.argtypes = [
            cvp, cfp, ctypes.c_int, ctypes.c_int, ctypes.c_float,
            ctypes.c_float, ctypes.c_float, ctypes.c_uint, cip, cup, cip,
            cip, cip]
        dll.Pomade_PickItem.restype = ctypes.c_int
        dll.Pomade_SetGizmo.argtypes = [cvp, ctypes.c_int, cfp, cfp,
                                       ctypes.c_float, ctypes.c_int]
        dll.Pomade_SetGizmo.restype = ctypes.c_int
        dll.Pomade_GetGizmo.argtypes = [cvp, cip, cfp, cfp, cfp, cip]
        dll.Pomade_GetGizmo.restype = ctypes.c_int
        dll.Pomade_SetBrushRing.argtypes = [cvp, cfp, cfp, ctypes.c_float]
        dll.Pomade_SetBrushRing.restype = ctypes.c_int
        dll.Pomade_GetBrushRing.argtypes = [cvp, cip, cfp, cfp, cfp]
        dll.Pomade_GetBrushRing.restype = ctypes.c_int
        dll.Pomade_BeginGesture.argtypes = [cvp, ctypes.c_char_p]
        dll.Pomade_BeginGesture.restype = ctypes.c_int
        dll.Pomade_EndGesture.argtypes = [cvp]
        dll.Pomade_EndGesture.restype = ctypes.c_int
        dll.Pomade_CancelGesture.argtypes = [cvp, cup]
        dll.Pomade_CancelGesture.restype = ctypes.c_int
        dll.Pomade_GetGestureDepth.argtypes = [cvp]
        dll.Pomade_GetGestureDepth.restype = ctypes.c_int
        dll.Pomade_Redo.argtypes = [cvp, cup]
        dll.Pomade_Redo.restype = ctypes.c_int
        dll.Pomade_GetRedoDepth.argtypes = [cvp]
        dll.Pomade_GetRedoDepth.restype = ctypes.c_int
        dll.Pomade_GetUndoLabel.argtypes = [cvp, ctypes.c_int,
                                           ctypes.c_char_p, ctypes.c_int]
        dll.Pomade_GetUndoLabel.restype = ctypes.c_int

    def _bindCommitter(self, dll):
        """The P1 commit pipeline (plan/17 section 3).

        Every handle crosses as c_void_p and every version as c_ulonglong:
        without the argtypes a 64-bit committer pointer passed as a Python
        int is truncated, which is exactly the kind of silent corruption
        the V2 idle pump would hit first.
        """
        cvp = ctypes.c_void_p
        dll.Pomade_CommitterCreate.argtypes = [
            cvp, ctypes.c_char_p, ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_void_p)]
        dll.Pomade_CommitterCreate.restype = ctypes.c_int
        dll.Pomade_CommitterDestroy.argtypes = [cvp]
        dll.Pomade_CommitterDestroy.restype = ctypes.c_int
        dll.Pomade_CommitterEnqueue.argtypes = [
            cvp, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_char_p]
        dll.Pomade_CommitterEnqueue.restype = ctypes.c_int
        dll.Pomade_CommitterSwap.argtypes = [cvp, ctypes.c_char_p,
                                            ctypes.c_int]
        dll.Pomade_CommitterSwap.restype = ctypes.c_int
        dll.Pomade_CommitterCommittedVersion.argtypes = [cvp]
        dll.Pomade_CommitterCommittedVersion.restype = ctypes.c_ulonglong
        dll.Pomade_CommitterPendingVersion.argtypes = [cvp]
        dll.Pomade_CommitterPendingVersion.restype = ctypes.c_ulonglong
        dll.Pomade_CommitterLastSwapMs.argtypes = [cvp]
        dll.Pomade_CommitterLastSwapMs.restype = ctypes.c_double
        dll.Pomade_CommitterPartialMode.argtypes = [cvp]
        dll.Pomade_CommitterPartialMode.restype = ctypes.c_int
        dll.Pomade_CommitterSetSwapBudgetMs.argtypes = [cvp, ctypes.c_double]
        dll.Pomade_CommitterSetSwapBudgetMs.restype = ctypes.c_int
        dll.Pomade_CommitterCancelCooks.argtypes = [cvp]
        dll.Pomade_CommitterCancelCooks.restype = ctypes.c_int
        dll.Pomade_CommitterSetScalpPath.argtypes = [cvp, ctypes.c_char_p]
        dll.Pomade_CommitterSetScalpPath.restype = ctypes.c_int
        dll.Pomade_CommitterSetScalpTarget.argtypes = [cvp, ctypes.c_char_p,
                                                      ctypes.c_char_p]
        dll.Pomade_CommitterSetScalpTarget.restype = ctypes.c_int
        dll.Pomade_CommitterDetach.argtypes = [cvp]
        dll.Pomade_CommitterDetach.restype = ctypes.c_int
        dll.Pomade_CommitterReattach.argtypes = [cvp]
        dll.Pomade_CommitterReattach.restype = ctypes.c_int
        # SS-05: a failed build or a refused enqueue, taken once per pump.
        # Optional so a DLL that predates them still loads; the session
        # probes with getattr.
        if hasattr(dll, "Pomade_CommitterTakeDiagnostic"):
            dll.Pomade_CommitterTakeDiagnostic.argtypes = [
                cvp, ctypes.c_char_p, ctypes.c_int]
            dll.Pomade_CommitterTakeDiagnostic.restype = ctypes.c_int
            dll.Pomade_CommitterFailedVersion.argtypes = [cvp]
            dll.Pomade_CommitterFailedVersion.restype = ctypes.c_ulonglong

    def _bindBake(self, dll):
        cvp = ctypes.c_void_p
        dll.Pomade_BakeCreate.argtypes = [cvp, ctypes.c_char_p, ctypes.c_char_p,
                                         ctypes.POINTER(ctypes.c_void_p)]
        dll.Pomade_BakeCreate.restype = ctypes.c_int
        dll.Pomade_BakeDestroy.argtypes = [cvp]
        dll.Pomade_BakeDestroy.restype = ctypes.c_int
        dll.Pomade_BakeSetOptions.argtypes = [cvp, ctypes.c_int, ctypes.c_int]
        dll.Pomade_BakeSetOptions.restype = ctypes.c_int
        dll.Pomade_BakeEnqueue.argtypes = [cvp]
        dll.Pomade_BakeEnqueue.restype = ctypes.c_int
        dll.Pomade_BakeTakeCompleted.argtypes = [
            cvp, ctypes.POINTER(ctypes.c_ulonglong), ctypes.c_char_p,
            ctypes.c_int]
        dll.Pomade_BakeTakeCompleted.restype = ctypes.c_int
        dll.Pomade_BakeSwap.argtypes = [cvp, ctypes.c_ulonglong, ctypes.c_char_p,
                                       ctypes.c_char_p, ctypes.c_char_p]
        dll.Pomade_BakeSwap.restype = ctypes.c_int
        dll.Pomade_BakePendingVersion.argtypes = [cvp]
        dll.Pomade_BakePendingVersion.restype = ctypes.c_ulonglong
        dll.Pomade_BakeCompletedVersion.argtypes = [cvp]
        dll.Pomade_BakeCompletedVersion.restype = ctypes.c_ulonglong
        # The hierarchy-carrying enqueue lives in pomadeApiStage.h, and so
        # does its error text: it writes Pomade_StageGetLastError, not the
        # model buffer Pomade_GetLastError reads (SS-05), so the session
        # needs both bound whether or not StageLibrary was ever built.
        if hasattr(dll, "Pomade_BakeEnqueueLevels"):
            dll.Pomade_BakeEnqueueLevels.argtypes = [cvp]
            dll.Pomade_BakeEnqueueLevels.restype = ctypes.c_int
        if hasattr(dll, "Pomade_StageGetLastError"):
            dll.Pomade_StageGetLastError.argtypes = []
            dll.Pomade_StageGetLastError.restype = ctypes.c_char_p

    @property
    def dll(self):
        return self._dll

    def lastError(self):
        text = self._dll.Pomade_GetLastError()
        return text.decode("utf-8") if text else ""


def readRefillDrops(dll, ctx):
    """[(tubeId, reason)] for every tube the last refill skipped.

    A refill returns 0 while ANY tube filled, so a tube whose rings its
    material chart cannot triangulate loses its guides without an error;
    this is the one place that loss is visible. Empty when every tube
    filled, when nothing is bound, or with a DLL predating the entry.
    """
    if dll is None or ctx is None or \
            not hasattr(dll, "Pomade_ReadRefillDrops"):
        return []
    count = ctypes.c_int(0)
    if dll.Pomade_ReadRefillDrops(ctx, None, 0, ctypes.byref(count)) != 0 \
            or count.value <= 0:
        return []
    ids = (ctypes.c_int * count.value)()
    got = ctypes.c_int(0)
    if dll.Pomade_ReadRefillDrops(ctx, ids, count.value,
                                 ctypes.byref(got)) != 0 \
            or got.value > count.value:
        # A refill between the probe and the read grew the list; the ABI
        # left `ids` unwritten. The next dock refresh reads it again.
        return []
    out = []
    for i in range(got.value):
        raw = dll.Pomade_GetRefillDropReason(ctx, i)
        out.append((int(ids[i]),
                    raw.decode("utf-8", "replace") if raw else ""))
    return out
