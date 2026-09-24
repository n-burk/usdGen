# usdGenTonicTools.tonicLib -- Qt-free ctypes binding of tonicApi (P0).
#
# Follows the usdGenLib.py rules from plan/08-tools.md section 1.4: BLAS
# threads are pinned before the package's first "import numpy", and the DLL
# resolves from $USDGENTONIC_DLL, else the install layout, else the repo
# build tree.
from __future__ import annotations

import ctypes
import os

for _v in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS"):
    os.environ.setdefault(_v, "1")     # S39: must precede the first "import numpy"

TONIC_OK = 0
TONIC_ERROR = 1
TONIC_NOT_IMPLEMENTED = 2

# TonicPickKind (tonicTube.h): one enum for what a pick returns and what a
# selection holds. Eight kinds are screen-pickable; a hierarchy level is
# chosen from the breadcrumb and selects every tube at it.
TONIC_PICK_TUBE_VERT = 1 << 0
TONIC_PICK_CENTER_CV = 1 << 1
TONIC_PICK_SECTION_CV = 1 << 2
TONIC_PICK_GRAPH_NODE = 1 << 3
TONIC_PICK_GUIDE = 1 << 4
TONIC_PICK_GRAPH_EDGE = 1 << 5
TONIC_PICK_REGION = 1 << 6
TONIC_PICK_SECTION_RING = 1 << 7
TONIC_PICK_LEVEL = 1 << 8
TONIC_PICK_ALL = 0xFF
TONIC_SELECT_ALL = 0x1FF

# How a select call combines with what is already selected.
TONIC_SELECT_SET = 0
TONIC_SELECT_ADD = 1
TONIC_SELECT_TOGGLE = 2
# Python-only: the C ABI has no subtract yet, so tonicLoops.selectItems /
# selectBand emulate it. Never pass it to a Tonic_Select* entry point.
TONIC_SELECT_REMOVE = 3

# TonicGizmoKind (tonicGizmo.h).
TONIC_GIZMO_NONE = 0
TONIC_GIZMO_TRANSLATE = 1
TONIC_GIZMO_RING_TRS = 2
TONIC_GIZMO_NODE_TRANSLATE = 3
TONIC_GIZMO_ROTATE = 4
TONIC_GIZMO_SCALE = 5

# TonicDirty (tonicModel.h): what Tonic_Publish is asked to republish ON TOP
# of the bits the model already reports pending. 0 -- TONIC_DIRTY_PENDING --
# publishes exactly the model's own pending set, which is what a gesture
# wants: the leaf-exact dirties are why a move stays inside its budget.
# TONIC_DIRTY_ALL is for the cases where something outside the model changed
# (a reattach, a fresh dock).
TONIC_DIRTY_PENDING = 0
TONIC_DIRTY_POINTS = 1 << 0
TONIC_DIRTY_TOPOLOGY = 1 << 1
TONIC_DIRTY_GRAPH = 1 << 2
TONIC_DIRTY_REGIONS = 1 << 3
TONIC_DIRTY_GUIDES = 1 << 4
TONIC_DIRTY_DISPLAY = 1 << 5
TONIC_DIRTY_SELECTION = 1 << 6
TONIC_DIRTY_GIZMO = 1 << 7
TONIC_DIRTY_BRUSH = 1 << 8
TONIC_DIRTY_ALL = 0x1FF

# Tonic_CommitterSwap result codes (tonicApi.h).
TONIC_COMMITTER_SWAPPED = 0
TONIC_COMMITTER_PARTIAL = 1
TONIC_COMMITTER_NOTHING_PENDING = 2
TONIC_COMMITTER_SKIPPED_GESTURE = 3
TONIC_COMMITTER_SKIPPED_STALE = 4
TONIC_COMMITTER_DETACHED = 5
# The swap failed (unknown live layer, a throw): the reason is in
# Tonic_GetLastError. Negative, so it never reads as PARTIAL (SS-05).
TONIC_COMMITTER_ERROR = -1


def TonicLibraryPath():
    """Resolve the usdGenTonic shared library, or "" when none exists."""
    override = os.environ.get("USDGENTONIC_DLL", "")
    if override:
        return override
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = (
        # Installed layout: <prefix>/lib/python/usdGenTonicTools -> <prefix>/lib.
        os.path.join(here, "..", "..", "..", "..", "lib"),
        # Build tree: <build>/python/usdGenTonicTools -> <build>.
        os.path.join(here, "..", ".."),
    )
    libNames = ("usdGenTonic.dll", "libusdGenTonic.so", "libusdGenTonic.dylib")
    for directory in candidates:
        for name in libNames:
            path = os.path.normpath(os.path.join(directory, name))
            if os.path.isfile(path):
                return path
    return ""


def _dllSearchDirs(path):
    """Extra Windows DLL search dirs for usdGenTonic's dependencies."""
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

    usdGenTonic imports usdGen from beside it plus the CUDA runtime, and
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
    """ctypes handle over tonicApi.h. Raises OSError when unloadable."""

    def __init__(self, path=""):
        path = path or TonicLibraryPath()
        if not path:
            raise OSError("usdGenTonic library not found "
                          "(set USDGENTONIC_DLL to its path)")
        self._dll = _load(path)
        self._bind()

    def _bind(self):
        dll = self._dll
        dll.Tonic_Create.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
        dll.Tonic_Create.restype = ctypes.c_int
        dll.Tonic_Destroy.argtypes = [ctypes.c_void_p]
        dll.Tonic_Destroy.restype = ctypes.c_int
        dll.Tonic_BuildTestTube.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                            ctypes.c_int, ctypes.c_float,
                                            ctypes.c_float]
        dll.Tonic_BuildTestTube.restype = ctypes.c_int
        dll.Tonic_MoveCenterRing.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                             ctypes.c_float, ctypes.c_float]
        dll.Tonic_MoveCenterRing.restype = ctypes.c_int
        dll.Tonic_GetVersion.argtypes = [ctypes.c_void_p]
        dll.Tonic_GetVersion.restype = ctypes.c_ulonglong
        dll.Tonic_TakeDirty.argtypes = [ctypes.c_void_p]
        dll.Tonic_TakeDirty.restype = ctypes.c_int
        dll.Tonic_GetVertexCount.argtypes = [ctypes.c_void_p]
        dll.Tonic_GetVertexCount.restype = ctypes.c_int
        dll.Tonic_GetQuadCount.argtypes = [ctypes.c_void_p]
        dll.Tonic_GetQuadCount.restype = ctypes.c_int
        dll.Tonic_HasCudaMirror.argtypes = [ctypes.c_void_p]
        dll.Tonic_HasCudaMirror.restype = ctypes.c_int
        dll.Tonic_GetDeviceFallbackReason.argtypes = [ctypes.c_void_p]
        dll.Tonic_GetDeviceFallbackReason.restype = ctypes.c_char_p
        dll.Tonic_Undo.argtypes = [ctypes.c_void_p,
                                   ctypes.POINTER(ctypes.c_uint)]
        dll.Tonic_Undo.restype = ctypes.c_int
        dll.Tonic_GetUndoDepth.argtypes = [ctypes.c_void_p]
        dll.Tonic_GetUndoDepth.restype = ctypes.c_int
        dll.Tonic_GetUndoBytes.argtypes = [ctypes.c_void_p]
        dll.Tonic_GetUndoBytes.restype = ctypes.c_uint64
        dll.Tonic_SetUndoBudget.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                            ctypes.c_uint64]
        dll.Tonic_SetUndoBudget.restype = ctypes.c_int
        dll.Tonic_ClearUndo.argtypes = [ctypes.c_void_p]
        dll.Tonic_ClearUndo.restype = ctypes.c_int
        self._bindGraph(dll)
        self._bindTube(dll)
        self._bindViewport(dll)
        self._bindSelection(dll)
        self._bindBake(dll)
        self._bindCommitter(dll)
        dll.Tonic_GetLastError.argtypes = []
        dll.Tonic_GetLastError.restype = ctypes.c_char_p

    def _bindGraph(self, dll):
        cvp = ctypes.c_void_p
        cfp = ctypes.POINTER(ctypes.c_float)
        cip = ctypes.POINTER(ctypes.c_int)
        dll.Tonic_BindScalp.argtypes = [cvp, cfp, ctypes.c_int, cip,
                                        ctypes.c_int, cip, ctypes.c_int]
        dll.Tonic_BindScalp.restype = ctypes.c_int
        dll.Tonic_HasScalp.argtypes = [cvp]
        dll.Tonic_HasScalp.restype = ctypes.c_int
        dll.Tonic_Raycast.argtypes = [cvp, cfp, cfp, cip, cip, cfp, cfp, cfp]
        dll.Tonic_Raycast.restype = ctypes.c_int
        dll.Tonic_ClosestPoint.argtypes = [cvp, cfp, cip, cip, cfp, cfp, cfp]
        dll.Tonic_ClosestPoint.restype = ctypes.c_int
        dll.Tonic_GraphAddNode.argtypes = [cvp, ctypes.c_int, ctypes.c_float,
                                           ctypes.c_float, cip]
        dll.Tonic_GraphAddNode.restype = ctypes.c_int
        dll.Tonic_GraphMoveNode.argtypes = [cvp, ctypes.c_int, ctypes.c_int,
                                            ctypes.c_float, ctypes.c_float]
        dll.Tonic_GraphMoveNode.restype = ctypes.c_int
        dll.Tonic_GraphConnect.argtypes = [cvp, ctypes.c_int, ctypes.c_int,
                                           cip]
        dll.Tonic_GraphConnect.restype = ctypes.c_int
        dll.Tonic_GraphSplitEdge.argtypes = [cvp, ctypes.c_int, ctypes.c_int,
                                             ctypes.c_float, ctypes.c_float,
                                             cip]
        dll.Tonic_GraphSplitEdge.restype = ctypes.c_int
        dll.Tonic_GraphWeld.argtypes = [cvp, ctypes.c_int, ctypes.c_int]
        dll.Tonic_GraphWeld.restype = ctypes.c_int
        dll.Tonic_GraphWeldAll.argtypes = [cvp, ctypes.c_float, cip]
        dll.Tonic_GraphWeldAll.restype = ctypes.c_int
        dll.Tonic_GraphUnweld.argtypes = [cvp, ctypes.c_int, cip,
                                          ctypes.c_int, cip]
        dll.Tonic_GraphUnweld.restype = ctypes.c_int
        dll.Tonic_GraphDeleteEdge.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_GraphDeleteEdge.restype = ctypes.c_int
        dll.Tonic_GraphDeleteNode.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_GraphDeleteNode.restype = ctypes.c_int
        dll.Tonic_GraphSnapNode.argtypes = [cvp, cfp, ctypes.c_float]
        dll.Tonic_GraphSnapNode.restype = ctypes.c_int
        dll.Tonic_GraphSnapEdge.argtypes = [cvp, cfp, ctypes.c_float]
        dll.Tonic_GraphSnapEdge.restype = ctypes.c_int
        dll.Tonic_GraphLinkRegions.argtypes = [cvp, ctypes.c_int,
                                               ctypes.c_int]
        dll.Tonic_GraphLinkRegions.restype = ctypes.c_int
        dll.Tonic_GraphUnlinkRegions.argtypes = [cvp, ctypes.c_int,
                                                 ctypes.c_int]
        dll.Tonic_GraphUnlinkRegions.restype = ctypes.c_int
        dll.Tonic_GraphStroke.argtypes = [cvp, cip, cfp, ctypes.c_int,
                                          ctypes.c_float, ctypes.c_float, cip,
                                          ctypes.c_int, cip, cip, cip, cip]
        dll.Tonic_GraphStroke.restype = ctypes.c_int
        dll.Tonic_GraphCreateRegion.argtypes = [cvp, cip, cip, cfp,
                                                ctypes.c_int, cip]
        dll.Tonic_GraphCreateRegion.restype = ctypes.c_int
        dll.Tonic_GraphGetNode.argtypes = [cvp, ctypes.c_int, cip, cfp, cfp]
        dll.Tonic_GraphGetNode.restype = ctypes.c_int
        dll.Tonic_GraphGetEdge.argtypes = [cvp, ctypes.c_int, cip]
        dll.Tonic_GraphGetEdge.restype = ctypes.c_int
        dll.Tonic_GraphGetNodeDisplayPosition.argtypes = [cvp, ctypes.c_int,
                                                           cfp]
        dll.Tonic_GraphGetNodeDisplayPosition.restype = ctypes.c_int
        dll.Tonic_GraphMoveNodes.argtypes = [cvp, cip, cip, cfp, ctypes.c_int]
        dll.Tonic_GraphMoveNodes.restype = ctypes.c_int
        dll.Tonic_GraphMirrorX.argtypes = [cvp, cip, ctypes.c_int, cip]
        dll.Tonic_GraphMirrorX.restype = ctypes.c_int
        dll.Tonic_SetSnapRadius.argtypes = [cvp, ctypes.c_float]
        dll.Tonic_SetSnapRadius.restype = ctypes.c_int
        dll.Tonic_GetSnapRadius.argtypes = [cvp]
        dll.Tonic_GetSnapRadius.restype = ctypes.c_float
        dll.Tonic_SetMirrorX.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_SetMirrorX.restype = ctypes.c_int
        dll.Tonic_GetMirrorX.argtypes = [cvp]
        dll.Tonic_GetMirrorX.restype = ctypes.c_int
        dll.Tonic_Rasterise.argtypes = [cvp]
        dll.Tonic_Rasterise.restype = ctypes.c_int
        dll.Tonic_RegionAtSurface.argtypes = [cvp, ctypes.c_int,
                                              ctypes.c_float, ctypes.c_float]
        dll.Tonic_RegionAtSurface.restype = ctypes.c_int
        dll.Tonic_GetMapVersion.argtypes = [cvp]
        dll.Tonic_GetMapVersion.restype = ctypes.c_ulonglong
        dll.Tonic_GetGraphCounts.argtypes = [cvp, cip, cip, cip]
        dll.Tonic_GetGraphCounts.restype = ctypes.c_int
        dll.Tonic_GetRegionStats.argtypes = [cvp, cip, cip, cip]
        dll.Tonic_GetRegionStats.restype = ctypes.c_int
        dll.Tonic_ReadFaceRegions.argtypes = [cvp, cip, ctypes.c_int, cip]
        dll.Tonic_ReadFaceRegions.restype = ctypes.c_int
        dll.Tonic_ReadFaceRegionIds.argtypes = [cvp, cip, ctypes.c_int, cip]
        dll.Tonic_ReadFaceRegionIds.restype = ctypes.c_int
        dll.Tonic_ReadGraphNodes.argtypes = [cvp, cip, cfp, cfp,
                                             ctypes.c_int, cip]
        dll.Tonic_ReadGraphNodes.restype = ctypes.c_int
        dll.Tonic_ReadGraphEdges.argtypes = [cvp, cip, ctypes.c_int, cip]
        dll.Tonic_ReadGraphEdges.restype = ctypes.c_int
        dll.Tonic_ReadRegionColors.argtypes = [cvp, cfp, ctypes.c_int, cip]
        dll.Tonic_ReadRegionColors.restype = ctypes.c_int
        dll.Tonic_ReadRegionLoops.argtypes = [cvp, cip, ctypes.c_int, cip,
                                              ctypes.c_int, cip, cip]
        dll.Tonic_ReadRegionLoops.restype = ctypes.c_int

    def _bindTube(self, dll):
        cvp = ctypes.c_void_p
        cfp = ctypes.POINTER(ctypes.c_float)
        cip = ctypes.POINTER(ctypes.c_int)
        cup = ctypes.POINTER(ctypes.c_uint)
        dll.Tonic_BuildTubeFromRegion.argtypes = [
            cvp, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_float]
        dll.Tonic_BuildTubeFromRegion.restype = ctypes.c_int
        # V6 (plan/18 section 7 G14): one L1 tube per closed region.
        dll.Tonic_ReadL1TubeIds.argtypes = [cvp, cip, ctypes.c_int, cip]
        dll.Tonic_ReadL1TubeIds.restype = ctypes.c_int
        dll.Tonic_TubeForRegion.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_TubeForRegion.restype = ctypes.c_int
        dll.Tonic_RegionForTube.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_RegionForTube.restype = ctypes.c_int
        dll.Tonic_SyncRegionTubes.argtypes = [cvp, cip, cip]
        dll.Tonic_SyncRegionTubes.restype = ctypes.c_int
        dll.Tonic_MoveCenterCV.argtypes = [
            cvp, ctypes.c_int, ctypes.c_float, ctypes.c_float, ctypes.c_float]
        dll.Tonic_MoveCenterCV.restype = ctypes.c_int
        dll.Tonic_InsertCenterCV.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_InsertCenterCV.restype = ctypes.c_int
        dll.Tonic_DeleteCenterCV.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_DeleteCenterCV.restype = ctypes.c_int
        dll.Tonic_SetTubeLength.argtypes = [cvp, ctypes.c_float]
        dll.Tonic_SetTubeLength.restype = ctypes.c_int
        dll.Tonic_MatchSurface.argtypes = [cvp]
        dll.Tonic_MatchSurface.restype = ctypes.c_int
        dll.Tonic_GetCenterCVCount.argtypes = [cvp]
        dll.Tonic_GetCenterCVCount.restype = ctypes.c_int
        dll.Tonic_GetCenterCV.argtypes = [cvp, ctypes.c_int, cfp]
        dll.Tonic_GetCenterCV.restype = ctypes.c_int
        dll.Tonic_GetSectionCount.argtypes = [cvp]
        dll.Tonic_GetSectionCount.restype = ctypes.c_int
        dll.Tonic_GetSection.argtypes = [
            cvp, ctypes.c_int, cfp, cfp, ctypes.c_int, cip, cfp, cfp]
        dll.Tonic_GetSection.restype = ctypes.c_int
        dll.Tonic_MoveSectionRing.argtypes = [
            cvp, ctypes.c_int, ctypes.c_float, ctypes.c_float]
        dll.Tonic_MoveSectionRing.restype = ctypes.c_int
        dll.Tonic_ScaleSectionRing.argtypes = [
            cvp, ctypes.c_int, ctypes.c_float]
        dll.Tonic_ScaleSectionRing.restype = ctypes.c_int
        dll.Tonic_TwistSectionRing.argtypes = [
            cvp, ctypes.c_int, ctypes.c_float]
        dll.Tonic_TwistSectionRing.restype = ctypes.c_int
        dll.Tonic_MoveSectionCV.argtypes = [
            cvp, ctypes.c_int, ctypes.c_int, ctypes.c_float, ctypes.c_float]
        dll.Tonic_MoveSectionCV.restype = ctypes.c_int
        dll.Tonic_AddSectionRing.argtypes = [cvp, ctypes.c_float, cip]
        dll.Tonic_AddSectionRing.restype = ctypes.c_int
        dll.Tonic_RemoveSectionRing.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_RemoveSectionRing.restype = ctypes.c_int
        dll.Tonic_CopySectionRing.argtypes = [
            cvp, ctypes.c_int, ctypes.c_int]
        dll.Tonic_CopySectionRing.restype = ctypes.c_int
        dll.Tonic_SetSoftSelection.argtypes = [
            cvp, ctypes.c_float, ctypes.c_float]
        dll.Tonic_SetSoftSelection.restype = ctypes.c_int
        dll.Tonic_GetSoftSelection.argtypes = [cvp, cfp, cfp]
        dll.Tonic_GetSoftSelection.restype = ctypes.c_int
        dll.Tonic_RelaxCenter.argtypes = [
            cvp, ctypes.c_float, ctypes.c_int]
        dll.Tonic_RelaxCenter.restype = ctypes.c_int
        dll.Tonic_SnapRootToScalp.argtypes = [cvp]
        dll.Tonic_SnapRootToScalp.restype = ctypes.c_int
        dll.Tonic_SetDisplaySegments.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_SetDisplaySegments.restype = ctypes.c_int
        dll.Tonic_GetDisplaySegments.argtypes = [cvp]
        dll.Tonic_GetDisplaySegments.restype = ctypes.c_int
        dll.Tonic_SetTubeRegionId.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_SetTubeRegionId.restype = ctypes.c_int
        dll.Tonic_GetTubeRegionId.argtypes = [cvp]
        dll.Tonic_GetTubeRegionId.restype = ctypes.c_int
        dll.Tonic_ReadTubeRegionFaces.argtypes = [cvp, cip, ctypes.c_int, cip]
        dll.Tonic_ReadTubeRegionFaces.restype = ctypes.c_int
        dll.Tonic_SetFillParams.argtypes = [
            cvp, ctypes.c_float, ctypes.c_int, ctypes.c_int, ctypes.c_float,
            cfp, ctypes.c_int]
        dll.Tonic_SetFillParams.restype = ctypes.c_int
        dll.Tonic_GetFillParams.argtypes = [
            cvp, cfp, cip, cip, cfp, cfp, ctypes.c_int, cip]
        dll.Tonic_GetFillParams.restype = ctypes.c_int
        dll.Tonic_SetPreviewFraction.argtypes = [cvp, ctypes.c_float]
        dll.Tonic_SetPreviewFraction.restype = ctypes.c_int
        dll.Tonic_GetPreviewFraction.argtypes = [cvp]
        dll.Tonic_GetPreviewFraction.restype = ctypes.c_float
        dll.Tonic_SetFreezeRoots.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_SetFreezeRoots.restype = ctypes.c_int
        dll.Tonic_GetFreezeRoots.argtypes = [cvp]
        dll.Tonic_GetFreezeRoots.restype = ctypes.c_int
        dll.Tonic_RefillGuides.argtypes = [cvp, ctypes.c_float]
        dll.Tonic_RefillGuides.restype = ctypes.c_int
        dll.Tonic_GenerateGuides.argtypes = [cvp, ctypes.c_float]
        dll.Tonic_GenerateGuides.restype = ctypes.c_int
        dll.Tonic_ClearGeneratedCurves.argtypes = [cvp]
        dll.Tonic_ClearGeneratedCurves.restype = ctypes.c_int
        dll.Tonic_SetGeneratedCurvesVisible.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_SetGeneratedCurvesVisible.restype = ctypes.c_int
        dll.Tonic_GetGeneratedCurvesVisible.argtypes = [cvp, cip]
        dll.Tonic_GetGeneratedCurvesVisible.restype = ctypes.c_int
        dll.Tonic_GetGuideCounts.argtypes = [cvp, cip, cip]
        dll.Tonic_GetGuideCounts.restype = ctypes.c_int
        # Tubes the last refill skipped (tube ids + per-index reason): a
        # refill that loses some tubes still returns 0. Guarded so a DLL
        # predating the entry still binds.
        if hasattr(dll, "Tonic_ReadRefillDrops"):
            dll.Tonic_ReadRefillDrops.argtypes = [cvp, cip, ctypes.c_int,
                                                  cip]
            dll.Tonic_ReadRefillDrops.restype = ctypes.c_int
            dll.Tonic_GetRefillDropReason.argtypes = [cvp, ctypes.c_int]
            dll.Tonic_GetRefillDropReason.restype = ctypes.c_char_p
        dll.Tonic_ReadGuidePreview.argtypes = [
            cvp, cfp, ctypes.c_int, cip, ctypes.c_int, cip]
        dll.Tonic_ReadGuidePreview.restype = ctypes.c_int
        dll.Tonic_ReadGuideRoots.argtypes = [
            cvp, cip, cfp, cfp, ctypes.c_int, cip]
        dll.Tonic_ReadGuideRoots.restype = ctypes.c_int
        dll.Tonic_Pick.argtypes = [
            cvp, cfp, ctypes.c_int, ctypes.c_int, ctypes.c_float,
            ctypes.c_float, ctypes.c_float, ctypes.c_uint, cip, cup, cip,
            cip, cfp, cfp]
        dll.Tonic_Pick.restype = ctypes.c_int

    def _bindViewport(self, dll):
        """The V0 publication ABI (plan/18 sections 2.1, 2.2).

        Tonic_Publish is the only way the viewport learns about a model
        change, so every mutating call in a gesture is followed by one.
        Note its return convention: the number of scene indices refreshed
        (-1 on error), not a status code.
        """
        cvp = ctypes.c_void_p
        cfp = ctypes.POINTER(ctypes.c_float)
        cip = ctypes.POINTER(ctypes.c_int)
        dll.Tonic_GetModelId.argtypes = [cvp]
        dll.Tonic_GetModelId.restype = ctypes.c_int
        dll.Tonic_Activate.argtypes = [cvp]
        dll.Tonic_Activate.restype = ctypes.c_int
        dll.Tonic_Deactivate.argtypes = [cvp]
        dll.Tonic_Deactivate.restype = ctypes.c_int
        dll.Tonic_Publish.argtypes = [cvp, ctypes.c_uint]
        dll.Tonic_Publish.restype = ctypes.c_int
        dll.Tonic_SetAmplifiedHair.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_SetAmplifiedHair.restype = ctypes.c_int
        dll.Tonic_GetAmplifiedHair.argtypes = [cvp]
        dll.Tonic_GetAmplifiedHair.restype = ctypes.c_int
        # Optional until the Output-description native extension is present;
        # headless panel fakes and older plugin DLLs keep their old surface.
        outputSet = getattr(dll, "Tonic_SetOutputSettings", None)
        if outputSet is not None:
            outputSet.argtypes = [cvp, ctypes.c_int, ctypes.c_float,
                                  ctypes.c_float]
            outputSet.restype = ctypes.c_int
        outputGet = getattr(dll, "Tonic_GetOutputSettings", None)
        if outputGet is not None:
            outputGet.argtypes = [cvp, ctypes.POINTER(ctypes.c_int),
                                  ctypes.POINTER(ctypes.c_float),
                                  ctypes.POINTER(ctypes.c_float)]
            outputGet.restype = ctypes.c_int
        dll.Tonic_SetRingDisplay.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_SetRingDisplay.restype = ctypes.c_int
        dll.Tonic_GetRingDisplay.argtypes = [cvp]
        dll.Tonic_GetRingDisplay.restype = ctypes.c_int
        dll.Tonic_SetLevelDisplay.argtypes = [
            cvp, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        dll.Tonic_SetLevelDisplay.restype = ctypes.c_int
        dll.Tonic_GetLevelDisplay.argtypes = [cvp, ctypes.c_int, cip, cip]
        dll.Tonic_GetLevelDisplay.restype = ctypes.c_int
        dll.Tonic_SetFocusLevel.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_SetFocusLevel.restype = ctypes.c_int
        dll.Tonic_GetFocusLevel.argtypes = [cvp]
        dll.Tonic_GetFocusLevel.restype = ctypes.c_int
        dll.Tonic_SetActiveCutEnabled.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_SetActiveCutEnabled.restype = ctypes.c_int
        dll.Tonic_GetActiveCutEnabled.argtypes = [cvp]
        dll.Tonic_GetActiveCutEnabled.restype = ctypes.c_int
        dll.Tonic_SetTubeExpanded.argtypes = [
            cvp, ctypes.c_int, ctypes.c_int]
        dll.Tonic_SetTubeExpanded.restype = ctypes.c_int
        dll.Tonic_GetTubeExpanded.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_GetTubeExpanded.restype = ctypes.c_int
        dll.Tonic_IsTubeVisible.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_IsTubeVisible.restype = ctypes.c_int
        dll.Tonic_SetDisplayScale.argtypes = [cvp, ctypes.c_float]
        dll.Tonic_SetDisplayScale.restype = ctypes.c_int
        dll.Tonic_GetDisplayScale.argtypes = [cvp, cfp]
        dll.Tonic_GetDisplayScale.restype = ctypes.c_int
        dll.Tonic_GetPublishedLevelInfo.argtypes = [
            cvp, ctypes.c_int, cip, cip, cip]
        dll.Tonic_GetPublishedLevelInfo.restype = ctypes.c_int
        # V9 (plan/18 section 2.4a): the whole viewport look of a mode in
        # one call. The table lives in C++ (TonicPolicyLevelDisplay), so
        # Python only says which mode, sub-mode and focus level it is in.
        dll.Tonic_SetDisplayPolicy.argtypes = [
            cvp, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        dll.Tonic_SetDisplayPolicy.restype = ctypes.c_int
        dll.Tonic_GetLevelDraw.argtypes = [cvp, ctypes.c_int, cfp, cip]
        dll.Tonic_GetLevelDraw.restype = ctypes.c_int
        dll.Tonic_SetGroomPath.argtypes = [cvp, ctypes.c_char_p]
        dll.Tonic_SetGroomPath.restype = ctypes.c_int
        dll.Tonic_GetGroomPath.argtypes = [cvp, ctypes.c_char_p, ctypes.c_int]
        dll.Tonic_GetGroomPath.restype = ctypes.c_int

    def _bindSelection(self, dll):
        """The V1 selection, overlay and gesture ABI (plan/18 sections 2.3
        to 2.5).

        Kind bits mirror TonicPickKind in tonicTube.h, so a pick result and
        a selection call speak the same enum. TONIC_SELECT_* below is the
        combine mode a rubber band or a click applies.
        """
        cvp = ctypes.c_void_p
        cfp = ctypes.POINTER(ctypes.c_float)
        cip = ctypes.POINTER(ctypes.c_int)
        cup = ctypes.POINTER(ctypes.c_uint)
        dll.Tonic_SelectClear.argtypes = [cvp, ctypes.c_uint]
        dll.Tonic_SelectClear.restype = ctypes.c_int
        for name in ("Tonic_SelectSet", "Tonic_SelectAdd",
                     "Tonic_SelectToggle"):
            entry = getattr(dll, name)
            entry.argtypes = [cvp, ctypes.c_uint, cip, cip, cip,
                              ctypes.c_int]
            entry.restype = ctypes.c_int
        dll.Tonic_SelectRect.argtypes = [
            cvp, cfp, ctypes.c_int, ctypes.c_int, ctypes.c_float,
            ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_uint,
            ctypes.c_int]
        dll.Tonic_SelectRect.restype = ctypes.c_int
        dll.Tonic_SelectPolygon.argtypes = [
            cvp, cfp, ctypes.c_int, ctypes.c_int, cfp, ctypes.c_int,
            ctypes.c_uint, ctypes.c_int]
        dll.Tonic_SelectPolygon.restype = ctypes.c_int
        dll.Tonic_SetHover.argtypes = [cvp, ctypes.c_uint, ctypes.c_int,
                                       ctypes.c_int, ctypes.c_int]
        dll.Tonic_SetHover.restype = ctypes.c_int
        dll.Tonic_GetHover.argtypes = [cvp, cup, cip, cip, cip]
        dll.Tonic_GetHover.restype = ctypes.c_int
        dll.Tonic_ReadSelection.argtypes = [cvp, ctypes.c_uint, cip, cip,
                                            cip, ctypes.c_int, cip]
        dll.Tonic_ReadSelection.restype = ctypes.c_int
        dll.Tonic_GetSelectionCount.argtypes = [cvp, ctypes.c_uint]
        dll.Tonic_GetSelectionCount.restype = ctypes.c_int
        dll.Tonic_GetSelectionBounds.argtypes = [cvp, cfp, cfp]
        dll.Tonic_GetSelectionBounds.restype = ctypes.c_int
        dll.Tonic_PickItem.argtypes = [
            cvp, cfp, ctypes.c_int, ctypes.c_int, ctypes.c_float,
            ctypes.c_float, ctypes.c_float, ctypes.c_uint, cip, cup, cip,
            cip, cip]
        dll.Tonic_PickItem.restype = ctypes.c_int
        dll.Tonic_SetGizmo.argtypes = [cvp, ctypes.c_int, cfp, cfp,
                                       ctypes.c_float, ctypes.c_int]
        dll.Tonic_SetGizmo.restype = ctypes.c_int
        dll.Tonic_GetGizmo.argtypes = [cvp, cip, cfp, cfp, cfp, cip]
        dll.Tonic_GetGizmo.restype = ctypes.c_int
        dll.Tonic_SetBrushRing.argtypes = [cvp, cfp, cfp, ctypes.c_float]
        dll.Tonic_SetBrushRing.restype = ctypes.c_int
        dll.Tonic_GetBrushRing.argtypes = [cvp, cip, cfp, cfp, cfp]
        dll.Tonic_GetBrushRing.restype = ctypes.c_int
        dll.Tonic_BeginGesture.argtypes = [cvp, ctypes.c_char_p]
        dll.Tonic_BeginGesture.restype = ctypes.c_int
        dll.Tonic_EndGesture.argtypes = [cvp]
        dll.Tonic_EndGesture.restype = ctypes.c_int
        dll.Tonic_CancelGesture.argtypes = [cvp, cup]
        dll.Tonic_CancelGesture.restype = ctypes.c_int
        dll.Tonic_GetGestureDepth.argtypes = [cvp]
        dll.Tonic_GetGestureDepth.restype = ctypes.c_int
        dll.Tonic_Redo.argtypes = [cvp, cup]
        dll.Tonic_Redo.restype = ctypes.c_int
        dll.Tonic_GetRedoDepth.argtypes = [cvp]
        dll.Tonic_GetRedoDepth.restype = ctypes.c_int
        dll.Tonic_GetUndoLabel.argtypes = [cvp, ctypes.c_int,
                                           ctypes.c_char_p, ctypes.c_int]
        dll.Tonic_GetUndoLabel.restype = ctypes.c_int

    def _bindCommitter(self, dll):
        """The P1 commit pipeline (plan/17 section 3).

        Every handle crosses as c_void_p and every version as c_ulonglong:
        without the argtypes a 64-bit committer pointer passed as a Python
        int is truncated, which is exactly the kind of silent corruption
        the V2 idle pump would hit first.
        """
        cvp = ctypes.c_void_p
        dll.Tonic_CommitterCreate.argtypes = [
            cvp, ctypes.c_char_p, ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_void_p)]
        dll.Tonic_CommitterCreate.restype = ctypes.c_int
        dll.Tonic_CommitterDestroy.argtypes = [cvp]
        dll.Tonic_CommitterDestroy.restype = ctypes.c_int
        dll.Tonic_CommitterEnqueue.argtypes = [
            cvp, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_char_p]
        dll.Tonic_CommitterEnqueue.restype = ctypes.c_int
        dll.Tonic_CommitterSwap.argtypes = [cvp, ctypes.c_char_p,
                                            ctypes.c_int]
        dll.Tonic_CommitterSwap.restype = ctypes.c_int
        dll.Tonic_CommitterCommittedVersion.argtypes = [cvp]
        dll.Tonic_CommitterCommittedVersion.restype = ctypes.c_ulonglong
        dll.Tonic_CommitterPendingVersion.argtypes = [cvp]
        dll.Tonic_CommitterPendingVersion.restype = ctypes.c_ulonglong
        dll.Tonic_CommitterLastSwapMs.argtypes = [cvp]
        dll.Tonic_CommitterLastSwapMs.restype = ctypes.c_double
        dll.Tonic_CommitterPartialMode.argtypes = [cvp]
        dll.Tonic_CommitterPartialMode.restype = ctypes.c_int
        dll.Tonic_CommitterSetSwapBudgetMs.argtypes = [cvp, ctypes.c_double]
        dll.Tonic_CommitterSetSwapBudgetMs.restype = ctypes.c_int
        dll.Tonic_CommitterCancelCooks.argtypes = [cvp]
        dll.Tonic_CommitterCancelCooks.restype = ctypes.c_int
        dll.Tonic_CommitterSetScalpPath.argtypes = [cvp, ctypes.c_char_p]
        dll.Tonic_CommitterSetScalpPath.restype = ctypes.c_int
        dll.Tonic_CommitterDetach.argtypes = [cvp]
        dll.Tonic_CommitterDetach.restype = ctypes.c_int
        dll.Tonic_CommitterReattach.argtypes = [cvp]
        dll.Tonic_CommitterReattach.restype = ctypes.c_int
        # SS-05: a failed build or a refused enqueue, taken once per pump.
        # Optional so a DLL that predates them still loads; the session
        # probes with getattr.
        if hasattr(dll, "Tonic_CommitterTakeDiagnostic"):
            dll.Tonic_CommitterTakeDiagnostic.argtypes = [
                cvp, ctypes.c_char_p, ctypes.c_int]
            dll.Tonic_CommitterTakeDiagnostic.restype = ctypes.c_int
            dll.Tonic_CommitterFailedVersion.argtypes = [cvp]
            dll.Tonic_CommitterFailedVersion.restype = ctypes.c_ulonglong

    def _bindBake(self, dll):
        cvp = ctypes.c_void_p
        dll.Tonic_BakeCreate.argtypes = [cvp, ctypes.c_char_p, ctypes.c_char_p,
                                         ctypes.POINTER(ctypes.c_void_p)]
        dll.Tonic_BakeCreate.restype = ctypes.c_int
        dll.Tonic_BakeDestroy.argtypes = [cvp]
        dll.Tonic_BakeDestroy.restype = ctypes.c_int
        dll.Tonic_BakeSetOptions.argtypes = [cvp, ctypes.c_int, ctypes.c_int]
        dll.Tonic_BakeSetOptions.restype = ctypes.c_int
        dll.Tonic_BakeEnqueue.argtypes = [cvp]
        dll.Tonic_BakeEnqueue.restype = ctypes.c_int
        dll.Tonic_BakeTakeCompleted.argtypes = [
            cvp, ctypes.POINTER(ctypes.c_ulonglong), ctypes.c_char_p,
            ctypes.c_int]
        dll.Tonic_BakeTakeCompleted.restype = ctypes.c_int
        dll.Tonic_BakeSwap.argtypes = [cvp, ctypes.c_ulonglong, ctypes.c_char_p,
                                       ctypes.c_char_p, ctypes.c_char_p]
        dll.Tonic_BakeSwap.restype = ctypes.c_int
        dll.Tonic_BakePendingVersion.argtypes = [cvp]
        dll.Tonic_BakePendingVersion.restype = ctypes.c_ulonglong
        dll.Tonic_BakeCompletedVersion.argtypes = [cvp]
        dll.Tonic_BakeCompletedVersion.restype = ctypes.c_ulonglong
        # The hierarchy-carrying enqueue lives in tonicApiStage.h, and so
        # does its error text: it writes Tonic_StageGetLastError, not the
        # model buffer Tonic_GetLastError reads (SS-05), so the session
        # needs both bound whether or not StageLibrary was ever built.
        if hasattr(dll, "Tonic_BakeEnqueueLevels"):
            dll.Tonic_BakeEnqueueLevels.argtypes = [cvp]
            dll.Tonic_BakeEnqueueLevels.restype = ctypes.c_int
        if hasattr(dll, "Tonic_StageGetLastError"):
            dll.Tonic_StageGetLastError.argtypes = []
            dll.Tonic_StageGetLastError.restype = ctypes.c_char_p

    @property
    def dll(self):
        return self._dll

    def lastError(self):
        text = self._dll.Tonic_GetLastError()
        return text.decode("utf-8") if text else ""


def readRefillDrops(dll, ctx):
    """[(tubeId, reason)] for every tube the last refill skipped.

    A refill returns 0 while ANY tube filled, so a tube whose rings its
    material chart cannot triangulate loses its guides without an error;
    this is the one place that loss is visible. Empty when every tube
    filled, when nothing is bound, or with a DLL predating the entry.
    """
    if dll is None or ctx is None or \
            not hasattr(dll, "Tonic_ReadRefillDrops"):
        return []
    count = ctypes.c_int(0)
    if dll.Tonic_ReadRefillDrops(ctx, None, 0, ctypes.byref(count)) != 0 \
            or count.value <= 0:
        return []
    ids = (ctypes.c_int * count.value)()
    got = ctypes.c_int(0)
    if dll.Tonic_ReadRefillDrops(ctx, ids, count.value,
                                 ctypes.byref(got)) != 0 \
            or got.value > count.value:
        # A refill between the probe and the read grew the list; the ABI
        # left `ids` unwritten. The next dock refresh reads it again.
        return []
    out = []
    for i in range(got.value):
        raw = dll.Tonic_GetRefillDropReason(ctx, i)
        out.append((int(ids[i]),
                    raw.decode("utf-8", "replace") if raw else ""))
    return out
