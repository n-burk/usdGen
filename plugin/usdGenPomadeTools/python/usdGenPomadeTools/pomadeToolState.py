# usdGenPomadeTools.pomadeToolState -- Qt-free mutable state (plan/17 P0).
#
# Mirrors the UsdGenToolState conventions from plan/08-tools.md section 1.3:
# one dataclass, created in the container's __init__, reachable from every
# module through the container. Everything else is derived from the stage or
# the PomadeModel and is re-read, never cached.
from __future__ import annotations

import dataclasses


@dataclasses.dataclass
class PomadeToolState:
    lib:            object = None        # ctypes handle, pomadeLib.Library()
    activated:      bool = False
    stageCacheId:   int = -1
    groomRoot:      str = ""
    generation:     int = 0              # last model version seen
    topoGeneration: int = 0              # last topology version seen
    undoStack:      object = None        # PomadeUndo stack (P6); None in P2
    frameKey:       object = None        # currentFrameChanged connection
    activeMode:     str = ""             # "" = no mode; one of pomadeModes ids
    activeLevel:    int = 1              # hierarchy focus level (P4)
    showAmplifiedHair: bool = False      # run the usdGen cook on commits
    outputEnabled: bool = False          # committed Output description exists
    outputDensityMultiplier: float = 1.0
    outputStrandWidth: float = 0.01
    context:        str = "interactive"  # "interactive" | "render"
    panels:         dict = dataclasses.field(default_factory=dict)
    viewportFailed: bool = False
    # Viewport controller (plan/18 V2). `workspaceOpen` gates the
    # application-level key filter -- the hotkeys exist only while the tool
    # is live -- and `lastMoveMs` is what the V5 fallback ladder will
    # trigger on, measured now so the number is never guessed.
    workspaceOpen:  bool = False
    lastMoveMs:     float = 0.0
    ladderStep:     int = 0              # 0 = full fidelity (V5 steps it)
    # FB-02: the dock's "Ladder enabled" box and the per-move budget the
    # ladder steps on. Both are read at the next press, never mid-drag.
    ladderEnabled:  bool = True
    moveBudgetMs:   float = 8.0          # pomadeLadder.MOVE_BUDGET_MS
    # Viewport navigation (FB-03). "maya" leaves the camera on usdview's
    # Alt+LMB/MMB/RMB; "blender" also orbits on plain MMB, pans on
    # Shift+MMB and dollies on Ctrl+MMB. Either way a plain MMB/RMB press
    # over an open workspace never picks a prim or opens its context menu.
    navigationStyle: str = "maya"
    # Graph mode (P2, plan/17 section 5.1).
    graphSubMode:   str = "region"       # one of GRAPH_SUBMODES
    snapRadiusPx:   float = 8.0          # on-screen weld/snap distance
    # MD-04: the click tolerance Hierarchy and Fill pick tubes with (their
    # `[`/`]` in Hierarchy). Separate from snapRadiusPx, which is Graph's
    # weld distance: one number used to drive both.
    pickRadiusPx:   float = 8.0
    mirrorX:        bool = False         # symmetric node creation
    mapVersion:     int = 0              # last map version enqueued
    bakedVersion:   int = 0              # last map version swapped
    # Tube mode (P3, plan/17 section 5.2).
    tubeSubMode:    str = ""             # "" = none; one of TUBE_SUBMODES
    selectionShape: str = "box"          # "box" | "lasso" in Tube edits
    tubeSelectionKind: str = "tube"      # tube, center, ring or section
    transformTool:  str = "move"         # select, move, rotate or scale
    # GZ-05 / parity G16: which way the Move/Rotate/Scale handles point --
    # "world" (identity), "screen" (camera plane) or "tube" (the owner
    # tube's root-normal frame).  Ring/Section keep the ring's own frame.
    # This is the LIVE tool's value: pomadeGizmoSettings.SwitchTool banks it
    # per tool on a tool change (Move/Scale start World, Rotate Tube).
    transformOrientation: str = "world"
    # Parity G20: pomadeGizmoSettings.GizmoSettings (per-tool step snap,
    # free rotate, prevent negative scale; manipulator size, grid size).
    # None until pomadeGizmoSettings.settingsFor(state) first creates it, so
    # this dataclass keeps importing with nothing but the standard library.
    gizmoSettings:  object = None
    softCenter:     float = 0.0          # soft-selection center in t
    softRadius:     float = 0.0          # 0 = exact CV only
    displaySegments: int = 1             # tessellation spans (ladder step 2)
    showGeneratedCurves: bool = True     # persistent guide-curve visibility
    # Fill mode (P3, plan/17 section 5.3).
    fillSubMode:    str = ""             # "" = none; one of FILL_SUBMODES
    previewFraction: float = 0.25        # live drag density
    freezeRoots:    bool = False         # keep roots on density changes
    # Hierarchy mode (P4, plan/17 section 5.4).
    hierarchySubMode: str = ""           # "" = none; one of HIERARCHY_SUBMODES
    subdivideCount: int = 4              # spinner 2..8
    splitMode:      str = "kmeans"       # "kmeans" | "edge"
    focusNames:     tuple = ()           # L1..focus tube names (breadcrumb)
    # Active-cut navigation is a per-branch frontier, owned by the model.
    # `activeLevel` remains a compatibility/display-style value; these ids
    # identify the expanded parent that supplies the current editable
    # frontier and its root-first ancestry for the breadcrumb.
    activeCutEnabled: bool = False
    focusParentId: int = -1
    focusAncestorIds: tuple = ()
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
    sculptStrength: float = 1.0          # comb push / smooth / twist scale
