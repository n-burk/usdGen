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
        dll.Tonic_Undo.argtypes = [ctypes.c_void_p]
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
        self._bindBake(dll)
        dll.Tonic_CommitterSetScalpPath.argtypes = [ctypes.c_void_p,
                                                    ctypes.c_char_p]
        dll.Tonic_CommitterSetScalpPath.restype = ctypes.c_int
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
        dll.Tonic_GetGuideCounts.argtypes = [cvp, cip, cip]
        dll.Tonic_GetGuideCounts.restype = ctypes.c_int
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
        cip = ctypes.POINTER(ctypes.c_int)
        dll.Tonic_GetModelId.argtypes = [cvp]
        dll.Tonic_GetModelId.restype = ctypes.c_int
        dll.Tonic_Activate.argtypes = [cvp]
        dll.Tonic_Activate.restype = ctypes.c_int
        dll.Tonic_Deactivate.argtypes = [cvp]
        dll.Tonic_Deactivate.restype = ctypes.c_int
        dll.Tonic_Publish.argtypes = [cvp, ctypes.c_uint]
        dll.Tonic_Publish.restype = ctypes.c_int
        dll.Tonic_SetLevelDisplay.argtypes = [
            cvp, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        dll.Tonic_SetLevelDisplay.restype = ctypes.c_int
        dll.Tonic_GetLevelDisplay.argtypes = [cvp, ctypes.c_int, cip, cip]
        dll.Tonic_GetLevelDisplay.restype = ctypes.c_int
        dll.Tonic_SetFocusLevel.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_SetFocusLevel.restype = ctypes.c_int
        dll.Tonic_GetFocusLevel.argtypes = [cvp]
        dll.Tonic_GetFocusLevel.restype = ctypes.c_int
        dll.Tonic_GetPublishedLevelInfo.argtypes = [
            cvp, ctypes.c_int, cip, cip, cip]
        dll.Tonic_GetPublishedLevelInfo.restype = ctypes.c_int

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

    @property
    def dll(self):
        return self._dll

    def lastError(self):
        text = self._dll.Tonic_GetLastError()
        return text.decode("utf-8") if text else ""
