#!/usr/bin/env python3
"""Present native WidthBlend, Part and Freeze kernel fixtures in a USD studio.

The curve stages originate from tools/docs/utility_fixtures.cpp.  This script
adds only camera, lights, displayColor material and neutral backdrop; it never
edits fixture curve positions, widths or diagnostic primvars.
"""

# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT

from __future__ import annotations

import json
import math
import argparse
from pathlib import Path
import shutil
import sys


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "examples" / "docs" / "fixtures"
MEDIA = ROOT / "docs" / "site" / "media"
REGISTRY = ROOT / "docs" / "reference" / "media.json"

CASES = {
    "width-blend": {"id": "UsdGenWidthBlend", "groups": ["Width_A", "Width_Blend", "Width_B"],
                    "eye": (0, -1.05, 0.48), "focal": 49,
                    "caption": "Actual WidthBlend kernel output, left to right: 0.0014 narrow input, 0.0037 blend at weight 0.5, and 0.0060 broad input. Curve positions are identical."},
    "part": {"id": "UsdGenPart", "groups": ["Part_ID_Diagnostic"],
             "eye": (0, -0.26, 0.66), "focal": 55,
             "caption": "Actual cooked partId plane seen from above: blue and orange mark the two assigned sides, gray marks unassigned strands. Part leaves strand points unchanged."},
    "freeze": {"id": "UsdGenFreeze", "groups": ["Freeze_Updated_Live_Input", "Freeze_Held_Output"],
               "eye": (0, -0.94, 0.48), "focal": 52,
               "caption": "Actual two-cook Freeze result: the live input updates while the right-hand frozen curves retain their captured shape."},
}

PROVENANCE = {
    "width-blend": "A native graph fixture executed the untyped WidthBlend kernel on two input curve buffers at weight 0.5. The studio layer adds only camera, lights and displayColor material.",
    "part": "A native graph fixture executed the typed Part kernel and exported cooked partId values. Diagnostic colors map those IDs; original strand positions are unchanged. The studio layer adds only camera, lights and material.",
    "freeze": "A native graph fixture cooked the typed Freeze kernel twice in one session, changed its upstream input before the second cook, and exported the updated live and held buffers. The studio layer adds only camera, lights and material.",
}


def camera_matrix(eye, target=(0, 0, .08)):
    def unit(v):
        m = math.sqrt(sum(x*x for x in v))
        return tuple(x/m for x in v)
    def cross(a,b):
        return (a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])
    back=unit(tuple(eye[i]-target[i] for i in range(3)))
    right=unit(cross((0,0,1),back))
    up=cross(back,right)
    rows=[(*right,0),(*up,0),(*back,0),(*eye,1)]
    return "( " + ", ".join("("+", ".join(f"{v:.8f}" for v in row)+")" for row in rows)+" )"


def wrapper(name, cfg):
    bindings="\n".join(f'''    over "{group}" (prepend apiSchemas = ["MaterialBindingAPI"]) {{
        rel material:binding = </World/Looks/Hair>
    }}''' for group in cfg["groups"])
    return f'''#usda 1.0
# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
(
    defaultPrim = "World"
    upAxis = "Z"
    subLayers = [@./{name}-fixture.usda@]
)
over "World" {{
    def Camera "FixtureCam" {{
        matrix4d xformOp:transform = {camera_matrix(cfg['eye'])}
        uniform token[] xformOpOrder = ["xformOp:transform"]
        float2 clippingRange = (0.01, 50)
        float focalLength = {cfg['focal']}
        float horizontalAperture = 36
        float verticalAperture = 24
    }}
    def DistantLight "Key" {{
        color3f inputs:color = (1, 0.92, 0.81)
        float inputs:intensity = 1.5
        bool inputs:normalize = true
        float3 xformOp:rotateXYZ = (36, -28, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }}
    def DistantLight "Fill" {{
        color3f inputs:color = (0.76, 0.89, 1)
        float inputs:intensity = 0.65
        bool inputs:normalize = true
        float3 xformOp:rotateXYZ = (-36, 48, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }}
    def DomeLight "Sky" {{
        asset inputs:texture:file = @../../maps/StinsonBeach.hdr@
        float inputs:intensity = 0.35
    }}
    def Scope "Looks" {{
        def Material "Hair" {{
            token outputs:surface.connect = </World/Looks/Hair/Surface.outputs:surface>
            def Shader "Surface" {{
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor.connect = </World/Looks/Hair/Color.outputs:result>
                color3f inputs:diffuseColor = (0.68, 0.53, 0.35)
                float inputs:roughness = 0.83
                token outputs:surface
            }}
            def Shader "Color" {{
                uniform token info:id = "UsdPrimvarReader_float3"
                token inputs:varname = "displayColor"
                color3f inputs:fallback = (0.68, 0.53, 0.35)
                color3f outputs:result
            }}
        }}
        def Material "Backdrop" {{
            token outputs:surface.connect = </World/Looks/Backdrop/Surface.outputs:surface>
            def Shader "Surface" {{
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0.055, 0.066, 0.076)
                float inputs:roughness = 0.9
                token outputs:surface
            }}
        }}
    }}
    def Mesh "Backdrop" (prepend apiSchemas = ["MaterialBindingAPI"]) {{
        rel material:binding = </World/Looks/Backdrop>
        uniform token subdivisionScheme = "none"
        bool doubleSided = true
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-3, 0.42, -3), (3, 0.42, -3), (3, 0.42, 3), (-3, 0.42, 3)]
    }}
{bindings}
}}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", choices=tuple(CASES), action="append",
                        help="Render only a named native fixture (repeatable)")
    args = parser.parse_args()
    sys.path.insert(0, str(ROOT / "tools" / "docs"))
    from render_pathtraced import render, viewer_environment
    FIXTURES.mkdir(parents=True, exist_ok=True)
    MEDIA.mkdir(parents=True, exist_ok=True)
    scratch = ROOT / "renders" / "docs" / "pathtrace"
    scratch.mkdir(parents=True, exist_ok=True)
    env = viewer_environment()
    for name in (args.name or CASES):
        cfg = CASES[name]
        generated = ROOT / "renders" / "docs" / f"{name}-native.usda"
        if not generated.exists():
            raise FileNotFoundError(f"Run usdGenUtilityFixtures first: {generated}")
        raw = generated.read_text(encoding="utf-8")
        if not raw.startswith("#usda 1.0"):
            raise ValueError(f"Invalid fixture header: {generated}")
        raw = raw.replace("#usda 1.0", "#usda 1.0\n# Copyright (c) 2026 Nick Burkard\n# SPDX-License-Identifier: MIT", 1)
        fixture = FIXTURES / f"{name}-fixture.usda"
        fixture.write_text(raw, encoding="utf-8")
        studio = FIXTURES / f"{name}-studio.usda"
        studio.write_text(wrapper(name, cfg), encoding="utf-8")
        target = MEDIA / ("part-native.png" if name == "part" else f"{name}.png")
        scratch_image = scratch / f"{name}-native-output.png"
        render(studio, "/World/FixtureCam", scratch_image, 1200, env)
        shutil.copyfile(scratch_image, target)
        registry = json.loads(REGISTRY.read_text(encoding="utf-8"))
        record = registry["operators"][cfg["id"]]
        record.update({
            "status": "native fixture",
            "image": target.relative_to(ROOT).as_posix(),
            "diagram": (MEDIA / f"{name}.svg").relative_to(ROOT).as_posix(),
            "caption": cfg["caption"],
            "renderer": "MoonRay / HdMoonrayRendererDebugPlugin",
            "scene": studio.relative_to(ROOT).as_posix(),
            "source": "tools/docs/utility_fixtures.cpp",
            "generator": "tools/render_utility_fixtures.py",
            "provenance": PROVENANCE[name],
        })
        REGISTRY.write_text(json.dumps(registry, indent=2)+"\n", encoding="utf-8")


if __name__ == "__main__":
    main()
