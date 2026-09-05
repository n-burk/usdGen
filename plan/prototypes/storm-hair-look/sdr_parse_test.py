import sys, os
from pxr import Sdr, Sdf, Usd, UsdShade, Tf
here = os.path.dirname(os.path.abspath(__file__))
path = os.path.join(here, "usdGenHairPreview.glslfx")

reg = Sdr.Registry()
node = reg.GetShaderNodeFromAsset(
    Sdf.AssetPath(path),
    {},                 # metadata
    "",                 # subIdentifier
    "glslfx")           # sourceType
print("node:", node)
if not node:
    print("FAIL: no node")
    sys.exit(1)
print("  identifier :", node.GetIdentifier())
print("  sourceType :", node.GetSourceType())
print("  context    :", node.GetContext())
print("  family     :", node.GetFamily())
print("  metadata   :", dict(node.GetMetadata()))
print("  sourceURI  :", node.GetResolvedImplementationURI())
print("  inputs (%d):" % len(node.GetShaderInputNames()))
for n in node.GetShaderInputNames():
    p = node.GetShaderInput(n)
    print("    %-20s type=%-8s arraySize=%-2d default=%s" % (
        n, p.GetType(), p.GetArraySize(), p.GetDefaultValue()))
print("  outputs:", list(node.GetShaderOutputNames()))
print("  primvars metadata:", node.GetMetadata().get("primvars"))

# Also exercise the sourceCode path (info:glslfx:sourceCode)
src = open(path).read()
node2 = reg.GetShaderNodeFromSourceCode(src, "glslfx", {})
print("sourceCode node:", node2 and node2.GetIdentifier(),
      "inputs:", node2 and len(node2.GetShaderInputNames()))

# HioGlslfx round trip: can we get the composed surfaceShader GLSL back out?
from pxr import Hio
g = Hio.Glslfx(path)
print("HioGlslfx valid:", g.IsValid())
