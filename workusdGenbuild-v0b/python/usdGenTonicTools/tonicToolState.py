# usdGenTonicTools.tonicToolState -- Qt-free mutable state (plan/17 P0).
#
# Mirrors the UsdGenToolState conventions from plan/08-tools.md section 1.3:
# one dataclass, created in the container's __init__, reachable from every
# module through the container. Everything else is derived from the stage or
# the TonicModel and is re-read, never cached.
from __future__ import annotations

import dataclasses


@dataclasses.dataclass
class TonicToolState:
    lib:            object = None        # ctypes handle, tonicLib.Library()
    activated:      bool = False
    stageCacheId:   int = -1
    groomRoot:      str = ""
    generation:     int = 0              # last model version seen
    topoGeneration: int = 0              # last topology version seen
    undoStack:      object = None        # TonicUndo stack (P6); None in P2
    frameKey:       object = None        # currentFrameChanged connection
    activeMode:     str = ""             # "" = no mode; one of tonicModes ids
    activeLevel:    int = 1              # hierarchy focus level (P4)
    showAmplifiedHair: bool = False      # run the usdGen cook on commits
    context:        str = "interactive"  # "interactive" | "render"
    panels:         dict = dataclasses.field(default_factory=dict)
    viewportFailed: bool = False
    # Graph mode (P2, plan/17 section 5.1).
    graphSubMode:   str = ""             # "" = none; one of GRAPH_SUBMODES
    snapRadiusPx:   float = 8.0          # on-screen weld/snap distance
    mirrorX:        bool = False         # symmetric node creation
    mapVersion:     int = 0              # last map version enqueued
    bakedVersion:   int = 0              # last map version swapped
    # Tube mode (P3, plan/17 section 5.2).
    tubeSubMode:    str = ""             # "" = none; one of TUBE_SUBMODES
    softCenter:     float = 0.0          # soft-selection center in t
    softRadius:     float = 0.0          # 0 = exact CV only
    displaySegments: int = 1             # tessellation spans (ladder step 2)
    # Fill mode (P3, plan/17 section 5.3).
    fillSubMode:    str = ""             # "" = none; one of FILL_SUBMODES
    previewFraction: float = 0.25        # live drag density
    freezeRoots:    bool = False         # keep roots on density changes
    # Hierarchy mode (P4, plan/17 section 5.4).
    hierarchySubMode: str = ""           # "" = none; one of HIERARCHY_SUBMODES
    subdivideCount: int = 4              # spinner 2..8
    splitMode:      str = "kmeans"       # "kmeans" | "edge"
    focusNames:     tuple = ()           # L1..focus tube names (breadcrumb)
    lockParents:    bool = False         # child edits refresh ancestors
    lockChildren:   bool = False         # parent edits re-derive descendants
    tubeLocks:      dict = dataclasses.field(default_factory=dict)
    soloLevel:      int = -1             # -1 = off; else the soloed level
    showMaxLevel:   int = -1             # -1 = all; else show levels <= n
    hiddenLevels:   set = dataclasses.field(default_factory=set)
    # Sculpt mode (P4, plan/17 section 5.5).
    sculptSubMode:  str = ""             # "" = none; one of SCULPT_SUBMODES
    brushRadiusPx:  float = 24.0         # screen falloff radius
    brushTRadius:   float = 0.0          # 0 = no t bound
    sculptPreserveLength: bool = True    # length-preserving default
    sculptMirrorX:  bool = False         # symmetric strokes across x = 0
