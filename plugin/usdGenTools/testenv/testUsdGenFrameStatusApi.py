# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Check exact viewer-root selection and bounded native snapshot copying."""
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import Mock, patch

folder = Path(__file__).resolve().parents[1] / "python/usdGenTools"
package = types.ModuleType("frameApiTestPackage")
package.__path__ = [str(folder)]
sys.modules[package.__name__] = package
spec = importlib.util.spec_from_file_location("frameApiTestPackage.frameStatusApi", folder / "frameStatusApi.py")
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)


class Prim:
    def __init__(self, path, typeName, parent=None):
        self.path, self.typeName, self.parent = path, typeName, parent
    def GetTypeName(self):
        return self.typeName
    def GetPath(self):
        return self.path
    def GetParent(self):
        return self.parent
    def IsPseudoRoot(self):
        return self.path == "/"


class FrameStatusApiTests(unittest.TestCase):
    def test_selector_includes_dormant_roots_and_excludes_nested_descriptions(self):
        root = Prim("/", "")
        groom = Prim("/G", "UsdGenGroom", root)
        desc = Prim("/G/D", "UsdGenDescription", groom)
        independent = Prim("/D", "UsdGenDescription", root)
        dormant = Prim("/Dormant", "UsdGenGroom", root)
        stage, renderer = Mock(), Mock()
        stage.Traverse.return_value = [groom, desc, independent, dormant]
        renderer.GetCurrentRendererId.return_value = "HdStormRendererPlugin"
        self.assertEqual(api.selector(stage, renderer), {
            "roots": ["/D", "/Dormant", "/G"], "renderer": "HdStormRendererPlugin"})

    def bound(self, copy):
        native = object.__new__(api.FrameStatusApi)
        native.copy = copy
        native._stage = "stage"
        native._roots = ["/G"]
        native._notice = None
        native._revision = 0
        renderer = Mock()
        renderer.GetCurrentRendererId.return_value = "HdStormRendererPlugin"
        return native, renderer

    def test_resize_race_retries_and_returns_one_complete_json_snapshot(self):
        payload = json.dumps({"state": "ready", "signature": "generation2"}).encode() + b"\0"
        sizes = [8, len(payload), len(payload)]
        def copy(selector, frame, buffer, capacity):
            result = sizes.pop(0)
            if buffer is not None and result <= capacity:
                ctypes.memmove(buffer, payload, len(payload))
            self.assertEqual(json.loads(selector)["roots"], ["/G"])
            self.assertEqual(frame, 2)
            return result
        native, renderer = self.bound(copy)
        self.assertEqual(native.status("stage", renderer, 2)["signature"], "generation2")

    def test_invalid_size_fails_closed(self):
        for size in (0, 32 * 1024 * 1024):
            native, renderer = self.bound(Mock(return_value=size))
            with self.assertRaises(RuntimeError):
                native.status("stage", renderer, 2)

    def test_continuous_resize_is_pending_instead_of_busy_wait(self):
        copy = Mock(side_effect=[1, 2, 3, 4, 5])
        native, renderer = self.bound(copy)
        self.assertEqual(native.status("stage", renderer, 2)["state"], "pending")
        self.assertEqual(copy.call_count, 5)

    def test_missing_render_engine_is_pending(self):
        native, renderer = self.bound(Mock())
        self.assertEqual(native.status("stage", None, 2)["state"], "pending")
        native.copy.assert_not_called()

    def test_zero_roots_requires_no_library_but_fences_frame_and_authored_edits(self):
        native, renderer = self.bound(Mock())
        native._roots = []
        first = native.status("stage", renderer, 1)
        second = native.status("stage", renderer, 2)
        native._revision += 1
        edited = native.status("stage", renderer, 2)
        self.assertEqual(first["state"], "ready")
        self.assertNotEqual(first["signature"], second["signature"])
        self.assertNotEqual(second["signature"], edited["signature"])
        native.copy.assert_not_called()

    def test_explicit_native_disable_uses_authored_frame_fence(self):
        native, renderer = self.bound(Mock())
        with patch.dict(os.environ, {"USDGEN_ENABLE": "0"}):
            self.assertEqual(native.status("stage", renderer, 2)["state"], "ready")
        native.copy.assert_not_called()

    def test_missing_native_with_groom_roots_fails_closed(self):
        native, renderer = self.bound(None)
        with patch.dict(os.environ, {"USDGEN_ENABLE": "1"}), \
                patch.object(api, "_loadLibrary", return_value=(None, "missing library")):
            with self.assertRaisesRegex(RuntimeError, "could not be loaded"):
                native.status("stage", renderer, 2)


if __name__ == "__main__":
    unittest.main()
