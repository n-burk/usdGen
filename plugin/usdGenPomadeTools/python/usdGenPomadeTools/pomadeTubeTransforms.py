"""Frozen, component-local transform math for :mod:`pomadeLoopsTube`.

The viewport gizmo reports an *absolute* transform from the press.  The
model ABI, on the other hand, accepts a series of deltas.  This small,
Qt-free adapter keeps those two contracts separate: points are frozen once
at press, every mouse sample is evaluated from that immutable baseline, and
the caller sends only the difference from its previous absolute result.

It deliberately knows nothing about selection or the model.  The loop owns
which center CVs, rings, and section CVs form a transform target; this module
only guarantees stable pivot/frame math for those already-resolved points.
"""
from __future__ import annotations

import math


IDENTITY_FRAME = (1.0, 0.0, 0.0,
                  0.0, 1.0, 0.0,
                  0.0, 0.0, 1.0)


def _asPoint(point):
    return tuple(float(value) for value in point)


def _add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _scale(vector, value):
    return (vector[0] * value, vector[1] * value, vector[2] * value)


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _unit(vector):
    length = math.sqrt(dot(vector, vector))
    return _scale(vector, 1.0 / length) if length > 1e-12 else None


def axes(frame=IDENTITY_FRAME):
    """The normalized U/V/W axes from Pomade's packed nine-float frame."""
    values = tuple(float(value) for value in frame)
    if len(values) != 9:
        raise ValueError("transform frame must contain nine floats")
    result = []
    for base in (0, 3, 6):
        axis = _unit(values[base:base + 3])
        if axis is None:
            raise ValueError("transform frame contains a zero axis")
        result.append(axis)
    return tuple(result)


def rotate(vector, axis, radians):
    """Rotate ``vector`` about ``axis`` with Rodrigues' formula."""
    unit = _unit(axis)
    if unit is None or abs(float(radians)) <= 1e-12:
        return _asPoint(vector)
    cosine, sine = math.cos(float(radians)), math.sin(float(radians))
    parallel = dot(unit, vector) * (1.0 - cosine)
    return _add(_add(_scale(vector, cosine), _scale(_cross(unit, vector), sine)),
                _scale(unit, parallel))


def scaleInFrame(vector, frame, factors):
    """Scale a vector along the U/V/W axes of ``frame``.

    Frames supplied by Pomade are orthonormal.  Reconstructing from their
    dot-products avoids treating a world-axis scale as a chart-axis scale.
    """
    factors = tuple(float(value) for value in factors)
    if len(factors) != 3:
        raise ValueError("transform scale needs three factors")
    u, v, w = axes(frame)
    return _add(_add(_scale(u, dot(vector, u) * factors[0]),
                     _scale(v, dot(vector, v) * factors[1])),
                _scale(w, dot(vector, w) * factors[2]))


def transformedPoint(point, pivot, translation=(0.0, 0.0, 0.0),
                     frame=IDENTITY_FRAME, scale=(1.0, 1.0, 1.0),
                     rotateAxis=None, radians=0.0):
    """Return one absolute host-application TRS result from a frozen point.

    Scale and rotation happen around the press-time pivot, then translation
    is added.  Callers pass a frame-local scale and (when applicable) a
    world rotation axis from the same frozen frame.
    """
    offset = scaleInFrame(_sub(_asPoint(point), _asPoint(pivot)), frame, scale)
    if rotateAxis is not None:
        offset = rotate(offset, rotateAxis, radians)
    return _add(_add(_asPoint(pivot), offset), _asPoint(translation))


class FrozenPoints:
    """Immutable press-time targets and deterministic absolute deltas."""

    def __init__(self, points, pivot, frame=IDENTITY_FRAME):
        self.points = {key: _asPoint(point) for key, point in points.items()}
        self.pivot = _asPoint(pivot)
        self.frame = tuple(float(value) for value in frame)

    def absolute(self, translation=(0.0, 0.0, 0.0),
                 scale=(1.0, 1.0, 1.0), rotateAxis=None, radians=0.0):
        """Map each frozen key to its desired world point at this sample."""
        return {key: transformedPoint(point, self.pivot, translation,
                                      self.frame, scale, rotateAxis, radians)
                for key, point in self.points.items()}

    @staticmethod
    def increments(previous, desired):
        """World deltas taking prior absolute targets to desired targets."""
        return {key: _sub(point, previous.get(key, point))
                for key, point in desired.items()}
