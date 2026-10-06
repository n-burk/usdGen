# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Read native frame readiness; never wait for a cook or mutate the stage."""
import ctypes
import json
import os

from .exprApi import _loadLibrary


def selector(stage, renderer):
    roots = []
    types = {"UsdGenGroom", "UsdGenDescription"}
    for prim in stage.Traverse():
        if prim.GetTypeName() not in types:
            continue
        parent = prim.GetParent()
        nested = False
        while parent and not parent.IsPseudoRoot():
            if parent.GetTypeName() in types:
                nested = True
                break
            parent = parent.GetParent()
        if not nested:
            roots.append(str(prim.GetPath()))
    return {"renderer": str(renderer.GetCurrentRendererId()), "roots": sorted(roots)}


class FrameStatusApi:
    def __init__(self):
        self._stage = None
        self._roots = None
        self._notice = None
        self._revision = 0
        self.lib = self.copy = None

    def _bind(self):
        self.lib, detail = _loadLibrary()
        if self.lib is None:
            raise RuntimeError("usdGenImaging could not be loaded: %s" % detail)
        try:
            self.copy = self.lib.usdGenImaging_copy_playback_status_json
        except AttributeError:
            raise RuntimeError("usdGenImaging does not expose frame readiness; rebuild the plugin.")
        self.copy.restype = ctypes.c_size_t
        self.copy.argtypes = [ctypes.c_char_p, ctypes.c_double,
                              ctypes.c_void_p, ctypes.c_size_t]

    def status(self, stage, renderer, frame):
        if renderer is None:
            return {"state": "pending", "error": ""}
        if stage != self._stage:
            if self._notice is not None:
                self._notice.Revoke()
            from pxr import Tf, Usd
            self._stage, self._roots = stage, None
            self._notice = Tf.Notice.Register(Usd.Notice.ObjectsChanged,
                                              self._changed, stage)
        if self._roots is None:
            self._roots = selector(stage, renderer)["roots"]
        rendererId = str(renderer.GetCurrentRendererId())
        disabled = os.environ.get("USDGEN_ENABLE", "1").strip().lower() in ("0", "false")
        if not self._roots or disabled:
            # No generated geometry is expected. Authored edits still fence
            # the framebuffer, and the UI additionally requires a converged
            # draw/presentation at this exact requested time.
            return {"state": "ready", "ready": True, "error": "",
                    "signature": json.dumps(["authored", id(stage), self._revision,
                                             rendererId, frame, disabled])}
        if self.copy is None:
            self._bind()
        request = json.dumps({"renderer": rendererId,
                              "roots": self._roots}, separators=(",", ":")).encode("utf-8")
        required = self.copy(request, frame, None, 0)
        for _ in range(4):
            if required == 0 or required > 16 * 1024 * 1024:
                raise RuntimeError("Native frame readiness returned an invalid response size.")
            buffer = ctypes.create_string_buffer(required)
            returned = self.copy(request, frame, buffer, required)
            if returned > required:
                required = returned
                continue
            if returned == 0:
                raise RuntimeError("Native frame readiness failed.")
            result = json.loads(buffer.value.decode("utf-8"))
            if result.get("state") == "ready" and not result.get("signature"):
                raise RuntimeError("Native readiness has no frame identity.")
            return result
        return {"state": "pending", "error": ""}

    def _changed(self, notice, sender):
        self._revision += 1
        # Only namespace changes affect root binding; time changes and
        # ordinary parameter edits need no repeated full-stage traversal.
        if notice.GetResyncedPaths():
            self._roots = None
