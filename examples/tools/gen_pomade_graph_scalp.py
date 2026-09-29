GX, GZ = 4, 4


def pid(ix, iz):
    return ix * (GZ + 1) + iz


pts = [(ix, 0, iz) for ix in range(GX + 1) for iz in range(GZ + 1)]
faces = []
for ix in range(GX):
    for iz in range(GZ):
        faces.append((pid(ix, iz), pid(ix, iz + 1),
                      pid(ix + 1, iz + 1), pid(ix + 1, iz)))

with open("examples/pomade-graph-scalp.usda", "w", newline="\n") as f:
    f.write("#usda 1.0\n")
    f.write("# Pomade P2 Graph-mode fixture: "
            "a 4x4 quad scalp in the XZ plane.\n\n")
    f.write('def Mesh "Scalp"\n{\n')
    f.write("    float3[] extent = "
            "[(-0.5, -0.5, -0.5), (4.5, 0.5, 4.5)]\n")
    f.write("    int[] faceVertexCounts = [%s]\n"
            % ", ".join(["4"] * len(faces)))
    f.write("    int[] faceVertexIndices = [%s]\n"
            % ", ".join(str(v) for q in faces for v in q))
    f.write("    point3f[] points = [%s]\n"
            % ", ".join("(%g, %g, %g)" % p for p in pts))
    f.write("}\n")
    f.write('\ndef Scope "Groom"\n{\n')
    f.write('    def Scope "Hair"\n    {\n    }\n}\n')
print("wrote examples/pomade-graph-scalp.usda")
