#!/usr/bin/env python3
"""Copy evaluation metadata from source schema specs into generated specs."""
import sys
from pxr import Sdf

source = Sdf.Layer.FindOrOpen(sys.argv[1])
generated = Sdf.Layer.FindOrOpen(sys.argv[2])
if source is None or generated is None:
    raise SystemExit("cannot open schema layers")
for prim in source.rootPrims:
    dst = generated.GetPrimAtPath(prim.path)
    if dst is None:
        continue
    for prop in prim.properties:
        evaluation = prop.customData.get("usdGen", {}).get("evaluation")
        target = dst.properties.get(prop.name)
        if evaluation and target is not None:
            data = dict(target.customData)
            usdgen = dict(data.get("usdGen", {}))
            usdgen["evaluation"] = evaluation
            data["usdGen"] = usdgen
            target.customData = data
generated.Save()
