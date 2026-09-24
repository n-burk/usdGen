#!/usr/bin/env python3
"""fur_gen.py MoonRay fallback check (docs/moonray-fur.md).

Runs tools/fur_gen.py (stdlib only, no pxr) and asserts the generated
PointInstancer fur scene carries a renderer-agnostic fallback material
next to its Storm glslfx terminal:

  * a universal (unprefixed) outputs:surface on /World/FurLook whose
    target Shader block exists and has info:id "UsdPreviewSurface";
  * that preview surface's diffuseColor connected to a
    UsdPrimvarReader_float3 block reading varname "displayColor", so
    per-instance strand colors survive in MoonRay (hdMoonray resolves
    instance-rate primvars through its RdlInstancerGeometry UserData);
  * the Storm terminal (outputs:glslfx:surface -> UsdGenHairPreview)
    untouched, since Storm prefers its glslfx context over universal.

Connection targets are resolved to their `def Shader` blocks; a dangling
target fails. Exit 0 only if every check passes, one FAIL line per
divergence.
"""

import os
import re
import subprocess
import sys
import tempfile

_HERE = os.path.dirname(os.path.abspath(__file__))
_GEN = os.path.dirname(os.path.dirname(_HERE))  # <repo>/tests/checks -> <repo>

failures = []


def fail(message):
    failures.append(message)
    print("FAIL: %s" % message)


def shader_block(text, material, leaf):
    """Body of `def Shader "<leaf>"` inside `def Material "<material>"`."""
    mat = re.search(
        r'def Material "%s"\s*\{(.*?)\n    \}\n' % re.escape(material),
        text, re.S)
    if not mat:
        return None
    block = re.search(
        r'def Shader "%s"\s*\{(.*?)\n        \}' % re.escape(leaf),
        mat.group(1), re.S)
    return block.group(1) if block else None


def main():
    gen = os.path.join(_GEN, "tools", "fur_gen.py")
    with tempfile.TemporaryDirectory(prefix="fur_gen_material") as tmp:
        out = os.path.join(tmp, "fur.usda")
        proc = subprocess.run(
            [sys.executable, gen, "8", out],
            capture_output=True, text=True, timeout=120)
        if proc.returncode != 0:
            fail("fur_gen.py exited %d: %s" % (
                proc.returncode, (proc.stderr or "").strip()[:300]))
            return 1
        with open(out) as fh:
            text = fh.read()

    if 'rel material:binding = </World/FurLook>' not in text:
        fail("Strand prototype no longer binds /World/FurLook")

    # Storm terminal intact (glslfx context wins over universal in Storm).
    if "outputs:glslfx:surface.connect" not in text:
        fail("Storm outputs:glslfx:surface terminal missing")
    if 'uniform token info:id = "UsdGenHairPreview"' not in text:
        fail("UsdGenHairPreview Storm shader missing")

    # Universal fallback terminal, resolved to its Shader block.
    m = re.search(
        r'^\s*token outputs:surface\.connect\s*=\s*'
        r'</World/FurLook/(\w+)\.outputs:surface>',
        text, re.M)
    if not m:
        fail("universal outputs:surface terminal missing on /World/FurLook")
        return 1
    preview = shader_block(text, "FurLook", m.group(1))
    if preview is None:
        fail("outputs:surface target </World/FurLook/%s> has no Shader block"
             % m.group(1))
        return 1
    if 'uniform token info:id = "UsdPreviewSurface"' not in preview:
        fail("fallback surface is not a UsdPreviewSurface")
    m2 = re.search(
        r'color3f inputs:diffuseColor\.connect\s*='
        r'\s*</World/FurLook/(\w+)\.outputs:result>',
        preview)
    if not m2:
        fail("preview diffuseColor is not connected to a primvar reader")
        return 1
    reader = shader_block(text, "FurLook", m2.group(1))
    if reader is None:
        fail("diffuseColor target </World/FurLook/%s> has no Shader block"
             % m2.group(1))
        return 1
    if 'uniform token info:id = "UsdPrimvarReader_float3"' not in reader:
        fail("diffuseColor reader is not a UsdPrimvarReader_float3")
    if 'token inputs:varname = "displayColor"' not in reader:
        fail('primvar reader does not read varname "displayColor"')
    if "inputs:fallback" not in reader:
        fail("primvar reader has no fallback color")

    if failures:
        return 1
    print("check_fur_gen_material: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
