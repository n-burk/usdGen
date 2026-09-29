# Brush tool state: the whole of the palette's mutable state in one object.
#
# plan/08-tools.md section 1.3 (UsdGenToolState): created once, in the
# container's __init__, reachable from every module; anything else is derived
# from the stage and re-read, never cached. pxr-free and Qt-free.
#
# Hotkey note: the shelf brushes claim no digit keys (the Tonic workspace
# owns 1-6). The viewport claims the host-application brush keys -- F /
# Shift+F / Ctrl+F drag-adjust and [ / ] steps (brushPanels.HOTKEYS) --
# ONLY while the pointer is over the StageView, a surface is bound,
# strokes are armed, no gesture is live and no text field has focus, so
# usdview's own F (Frame Selected) keeps working everywhere else. Escape
# is claimed only while our own gesture or adjust is live.

import dataclasses


# Shelf brushes: (id, label). The id selects the stroke's dab mode.
BRUSHES = (
    ("paint", "Paint"),
    ("add", "Add"),
    ("smooth", "Smooth"),
    ("erase", "Erase"),
)

BRUSH_IDS = tuple(b[0] for b in BRUSHES)

# Dab mode per shelf brush (brushMap.py spellings). Erase blends toward
# the map's default value.
BRUSH_MODES = {
    "paint": "set",
    "add": "add",
    "smooth": "smooth",
    "erase": "erase",
}

# Bind resolution bounds (texels per face edge, powers of two).
RESOLUTION_MIN = 4
RESOLUTION_MAX = 256


@dataclasses.dataclass
class BrushToolState:
    binding: object = None       # brushAuthor.Binding, or None if unbound
    activeBrush: str = "paint"   # one of BRUSH_IDS
    maskPreset: str = "density"  # one of brushAuthor.MASK_PRESET_IDS
    activeDescription: str = ""  # Sdf path of the description painted to, or ""
    radiusWorld: float = 0.15    # dab radius in world units (outer ring)
    strength: float = 0.5        # dab strength in [0, 1]
    hardness: float = 0.5        # inner-radius fraction: weight 1 inside
                                 # hardness*radius, falloff to the rim
    falloff: str = "smooth"      # constant | linear | smooth
    value: float = 1.0           # Set target / Add offset
    channel: int = -1            # -1 = every channel, else one channel
    colorMap: str = "heat"       # map overlay ramp: heat | gray
    valueRange: object = (0.0, 1.0)  # overlay normalisation (lo, hi)
    previewMap: bool = True      # the map overlay is shown on the surface
    liveGroom: bool = True       # re-cook the groom from session scratch
                                 # while dragging (throttled + trailing flush)
    strokesArmed: bool = True    # the viewport filter captures strokes
    resolutionAuto: bool = True  # bind picks the suggested texel density
    resolution: int = 32         # manual bind resolution (power of two)
    undoStack: object = None     # brushAuthor.UndoStack, made by the loop
    gesture: object = None       # the live BrushGesture, or None
