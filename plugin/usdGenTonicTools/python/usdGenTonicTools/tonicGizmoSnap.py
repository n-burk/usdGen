# usdGenTonicTools.tonicGizmoSnap -- world-grid snapping for the `X` hold
# (tonic_gizmo_parity G11).
#
# Vendored from usdRig `plugin/rigExecUsdview/gizmoSnap.py` at 4fe4142
# (2026-09-03), adapted for Tonic; keep in sync. Only the grid half is
# here: ConstrainToHandle, DirectionIsWorldAligned, _PlaneBasis and
# GridPoint, rule for rule. Point / edge / surface snapping (NearestPoint,
# NearestSegment, the pick resolver) is rig-geometry specific and is left
# for parity G12.
#
# What changed in the port: Gf.Vec3d arithmetic is spelled over plain
# 3-tuples, because the Tonic T0/T1 tests import this package with no pxr
# (the same reason tonicGizmoScreen is tuple maths). A handle is anything
# with RigExec's `kind`, `worldAxis` and `worldNormal` attributes -- a
# tonicGizmoScreen.Handle, or the stand-in TubeLoop builds for its
# root-normal constraint.
from __future__ import annotations

from . import tonicGizmoScreen

_AXES = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))


def _add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _mul(v, s):
    return (v[0] * s, v[1] * s, v[2] * s)


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _length(v):
    return _dot(v, v) ** 0.5


def _vec(v):
    return (float(v[0]), float(v[1]), float(v[2]))


def _normalized(v):
    length = _length(v)
    return _mul(v, 1.0 / length) if length > 0.0 else v


def ConstrainToHandle(handle, pivot0, worldPoint, ctrl=False):
    """Constrain a world target to what the handle may move along.

    Axis (no Ctrl) slides along its line, a plane -- or an axis with
    Ctrl, which the Ctrl+axis drag also treats as a plane -- drops the
    normal component, and the centre (or anything else) lands unchanged.
    """
    kind = getattr(handle, "kind", None)
    origin = _vec(pivot0)
    target = _vec(worldPoint)
    if kind == "axis" and not ctrl:
        axis = getattr(handle, "worldAxis", None)
        if axis is None:
            return target
        direction = _vec(axis)
        if _length(direction) < 1e-12:
            return target
        direction = _normalized(direction)
        return _add(origin, _mul(direction,
                                 _dot(_sub(target, origin), direction)))
    if kind == "plane" or (kind == "axis" and ctrl):
        if kind == "plane":
            raw = getattr(handle, "worldNormal", None)
        else:
            raw = getattr(handle, "worldAxis", None)
        if raw is None:
            return target
        normal = _vec(raw)
        if _length(normal) < 1e-12:
            return target
        normal = _normalized(normal)
        return _sub(target, _mul(normal, _dot(_sub(target, origin), normal)))
    return target


def DirectionIsWorldAligned(direction, tol=1e-6):
    """Index of the world axis `direction` lies along, or None.

    The sign is ignored. GridPoint calls it on the axis for an axis
    handle and on the plane normal otherwise; None means there is no
    grid coordinate in that frame, so the caller degrades to quantising
    the travel from the pivot.
    """
    axis = _vec(direction)
    if _length(axis) < 1e-12:
        return None
    unit = _normalized(axis)
    for index, candidate in enumerate(_AXES):
        # Within tol of +axis or -axis alike: a -X handle still snaps the
        # world X coordinate.
        if (_length(_sub(unit, candidate)) <= tol or
                _length(_add(unit, candidate)) <= tol):
            return index
    return None


def _PlaneBasis(normal):
    """Deterministic orthonormal (u, v) spanning the plane.

    e is the world axis least parallel to n (ties X before Y before Z), u
    is e with the normal removed, v completes the frame.
    """
    dots = [abs(_dot(axis, normal)) for axis in _AXES]
    # min keeps the first minimal entry, which is the X-before-Y-before-Z
    # tie-break.
    pick = min(range(3), key=lambda i: dots[i])
    along = _sub(_AXES[pick], _mul(normal, _dot(_AXES[pick], normal)))
    if _length(along) < 1e-12:
        return None, None
    unit = _normalized(along)
    return unit, _normalized(_cross(normal, unit))


def GridPoint(handle, pivot0, unsnapped, gridSize, ctrl=False):
    """Snap the unsnapped pivot to the world grid.

    Only the components the handle may change move: an axis rounds along
    itself, a plane (or Ctrl+axis) rounds the two in-plane world
    components, the centre rounds all three, halves away from zero
    through tonicGizmoScreen.SnapAbsolute. A handle pointing nowhere near
    a world axis has no grid coordinate in its frame, so it quantises the
    travel from the pivot on the same directions instead.
    """
    position = _vec(unsnapped)
    if gridSize is None or gridSize <= 0.0:
        return position
    origin = _vec(pivot0)
    snap = tonicGizmoScreen.SnapAbsolute
    kind = getattr(handle, "kind", None)
    if kind == "axis" and not ctrl:
        raw = getattr(handle, "worldAxis", None)
        if raw is None or _length(_vec(raw)) < 1e-12:
            return snap(position, gridSize)
        axis = _normalized(_vec(raw))
        if DirectionIsWorldAligned(axis) is not None:
            along = _dot(position, axis)
            snapped = snap(along, gridSize)
            return _add(position, _mul(axis, snapped - along))
        travel = _dot(_sub(position, origin), axis)
        return _add(origin, _mul(axis, snap(travel, gridSize)))
    if kind == "plane" or (kind == "axis" and ctrl):
        if kind == "plane":
            raw = getattr(handle, "worldNormal", None)
        else:
            raw = getattr(handle, "worldAxis", None)
        if raw is None or _length(_vec(raw)) < 1e-12:
            return snap(position, gridSize)
        normal = _normalized(_vec(raw))
        facing = DirectionIsWorldAligned(normal)
        if facing is not None:
            kept = [position[0], position[1], position[2]]
            for index in range(3):
                if index != facing:
                    kept[index] = snap(kept[index], gridSize)
            return (kept[0], kept[1], kept[2])
        across, other = _PlaneBasis(normal)
        if across is None:
            return snap(position, gridSize)
        travel = _sub(position, origin)
        return _add(_add(origin, _mul(across, snap(_dot(travel, across),
                                                   gridSize))),
                    _mul(other, snap(_dot(travel, other), gridSize)))
    return snap(position, gridSize)
