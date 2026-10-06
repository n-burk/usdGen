# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Qt-free frame presentation fence shared by playback and playblast."""

import math


class PresentationFence:
    def __init__(self):
        self.drawn = None
        self.presented = None

    def invalidate(self):
        self.drawn = self.presented = None

    def drawnFrame(self, frame, before, after, converged):
        self.drawn = ((frame, after) if before is not None and
                      before == after and converged else None)
        self.presented = None

    def swapped(self, frame, signature):
        candidate = (frame, signature)
        self.presented = (candidate if signature is not None and
                          candidate == self.drawn else None)

    def ready(self, frame, signature):
        return signature is not None and self.presented == (frame, signature)


def movieFrames(samples, fps):
    """Each displayed timeline sample is one movie frame at timeline FPS."""
    frames = tuple(float(frame) for frame in samples)
    if not math.isfinite(fps) or fps <= 0:
        raise ValueError("Timeline FPS must be positive and finite.")
    if not frames or any(not math.isfinite(frame) for frame in frames):
        raise ValueError("The timeline has no valid frames.")
    if any(b <= a for a, b in zip(frames, frames[1:])):
        raise ValueError("Timeline frames must increase.")
    return frames
