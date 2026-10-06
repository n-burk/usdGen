# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Headless regressions for stale-frame rejection and movie timing."""
import importlib.util
from pathlib import Path
import unittest

source = Path(__file__).resolve().parents[1] / "python/usdGenTools/playbackState.py"
spec = importlib.util.spec_from_file_location("playbackState", source)
state = importlib.util.module_from_spec(spec)
spec.loader.exec_module(state)


class PlaybackStateTests(unittest.TestCase):
    def test_requires_draw_and_swap_for_exact_current_frame(self):
        fence = state.PresentationFence()
        fence.drawnFrame(1, "a", "a", True)
        self.assertFalse(fence.ready(1, "a"))
        fence.swapped(1, "a")
        self.assertTrue(fence.ready(1, "a"))
        self.assertFalse(fence.ready(2, "a"))
        self.assertFalse(fence.ready(1, "b"))

    def test_late_publication_during_draw_or_before_swap_is_rejected(self):
        fence = state.PresentationFence()
        fence.drawnFrame(1, "a", "b", True)
        fence.swapped(1, "b")
        self.assertFalse(fence.ready(1, "b"))
        fence.drawnFrame(1, "b", "b", True)
        fence.swapped(1, "c")
        self.assertFalse(fence.ready(1, "c"))

    def test_pending_native_or_unconverged_renderer_is_rejected(self):
        fence = state.PresentationFence()
        for before, after, converged in ((None, None, True),
                                         (None, "a", True), ("a", "a", False)):
            fence.drawnFrame(1, before, after, converged)
            fence.swapped(1, after)
            self.assertFalse(fence.ready(1, after))

    def test_invalidate_prevents_previous_frame_capture(self):
        fence = state.PresentationFence()
        fence.drawnFrame(1, "a", "a", True)
        fence.swapped(1, "a")
        fence.invalidate()
        self.assertFalse(fence.ready(1, "a"))

    def test_movie_uses_each_inclusive_timeline_sample_once(self):
        frames = state.movieFrames([1, 1.5, 2], 48)
        self.assertEqual(frames, (1, 1.5, 2))
        self.assertEqual(len(frames) / 48, 0.0625)

    def test_bad_timeline_is_rejected(self):
        for frames, fps in (([], 24), ([1], 0), ([1], float("nan")),
                            ([1, 1], 24), ([2, 1], 24), ([float("inf")], 24)):
            with self.assertRaises(ValueError):
                state.movieFrames(frames, fps)


if __name__ == "__main__":
    unittest.main()
