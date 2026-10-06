# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Add verified real Noodles captures to the manual UI registry."""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
UI = ROOT / "docs/reference/ui.json"
OPERATORS = ROOT / "docs/reference/operators.json"
MEDIA = ROOT / "docs/reference/media.json"

FIELDS = {
    "UsdGenScatter": "usdGen:density",
    "UsdGenGrow": "usdGen:length",
    "UsdGenGuideInterpolate": "usdGen:influenceRadius",
    "UsdGenReferenceSource": "usdGen:enabled",
    "UsdGenCurveSource": "usdGen:densityMultiplier",
    "UsdGenClump": "usdGen:clump:amount",
    "UsdGenNoise": "usdGen:noise:magnitude",
    "UsdGenLength": "usdGen:length:value",
    "UsdGenWidth": "usdGen:width",
    "UsdGenDirection": "usdGen:amount",
    "UsdGenSmooth": "usdGen:strength",
    "UsdGenResample": "usdGen:cvCount",
    "UsdGenScale": "usdGen:scale",
    "UsdGenCurl": "usdGen:radius",
    "UsdGenBend": "usdGen:angle",
    "UsdGenStraighten": "usdGen:tangentStraightness",
    "UsdGenDisplace": "usdGen:displace:amount",
    "UsdGenWave": "usdGen:amplitudeU",
    "UsdGenPart": "usdGen:part:radius",
    "UsdGenExprOp": "usdGen:expr:source",
    "UsdGenSculptLayer": "usdGen:sculpt:weight",
    "UsdGenDeform": "usdGen:lockRoots",
    "UsdGenCollide": "usdGen:offset",
    "UsdGenWind": "usdGen:constStrength",
    "UsdGenFreeze": "usdGen:enabled",
}

LEAVES = {
    "UsdGenReferenceSource": "source",
    "UsdGenCurveSource": "source",
    "UsdGenExprOp": "expression",
    "UsdGenSculptLayer": "sculpt",
    "UsdGenGuideInterpolate": "interp",
}

SPECIAL_SCENES = {
    "UsdGenPart": "examples/docs/operators/part.usda",
    "UsdGenFreeze": "examples/docs/operators/freeze.usda",
}


def main():
    ui = json.loads(UI.read_text(encoding="utf-8"))
    catalog = json.loads(OPERATORS.read_text(encoding="utf-8"))["operators"]
    media = json.loads(MEDIA.read_text(encoding="utf-8"))["operators"]
    ui["screenshots"] = [row for row in ui["screenshots"]
                         if "operator" not in row]
    for op in catalog:
        op_id = op["id"]
        if op_id not in FIELDS:
            continue
        scene = SPECIAL_SCENES.get(op_id, media[op_id].get("scene"))
        if not scene:
            raise RuntimeError("No scene for " + op_id)
        field = FIELDS[op_id]
        if field not in {p["name"] for p in op["parameters"]}:
            raise RuntimeError("Field not in operator reference: " + field)
        visible = field.removeprefix("usdGen:")
        leaf = LEAVES.get(op_id, op_id.removeprefix("UsdGen").lower())
        parent = "Hair" if op_id in ("UsdGenGuideInterpolate",
                                      "UsdGenDeform") else "Fur"
        selected = "/World/Groom/%s/Ops/%s" % (parent, leaf)
        image = "docs/site/media/ui/operator-%s-noodles.png" % (
            op_id.removeprefix("UsdGen").lower())
        if not (ROOT / image).is_file():
            raise RuntimeError("Missing capture: " + image)
        relation_note = ""
        if op_id in ("UsdGenGuideInterpolate", "UsdGenReferenceSource",
                     "UsdGenCurveSource"):
            relation_note = (" The guide or curve relationship is a graph pin "
                             "already authored in this prepared scene; it is "
                             "not an inline value cell.")
        field_type = next(p["type"] for p in op["parameters"]
                          if p["name"] == field)
        if field_type == "bool":
            value_action = ("In the selected node's usdGen group, click the %s "
                            "checkbox to toggle it." % visible)
        elif field_type == "token":
            value_action = ("In the selected node's usdGen group, click the %s "
                            "value menu and choose an allowed token." % visible)
        else:
            value_action = ("In the selected node's usdGen group, click the %s "
                            "value cell, type the value you want, and press Enter." % visible)
        control_name = "checkbox" if field_type == "bool" else "value cell"
        actions = [
            "Open %s in usdview, then choose Window > Noodles Editor (N)." % scene,
            "In usdview's Scene Graph, select %s; focus Noodles Editor and press A to add the selected prim." % selected,
            "In Window > Layer Editor, choose a persistent edit target before changing a value.",
            "Keep View > Show Attribute Values on. Right-click the Noodles canvas and choose Write Values > Default.",
            value_action,
            "Press Ctrl+S in Noodles Editor to save the selected edit-target layer.",
        ]
        ui["screenshots"].append({
            "id": "operator-%s-noodles" % op_id.removeprefix("UsdGen").lower(),
            "image": image,
            "title": "%s in Noodles Editor" % op["title"],
            "caption": ("Real localized Noodles Editor with %s selected. The "
                        "%s %s is visible in its usdGen group.%s" %
                        (op["title"], visible, control_name, relation_note)),
            "scene": scene,
            "capture": "tools/docs/capture_all_operator_ui.ps1 invokes bin/launch_usdview.ps1 -TestScript tools/docs/capture_operator_noodles_ui.py",
            "ui": "Window > Noodles Editor; View > Show Attribute Values",
            "operator": op_id,
            "selectedPrim": selected,
            "editedField": field,
            "visibleField": visible,
            "actions": actions,
            "captureExercises": ["Scene Graph selection", "Window > Noodles Editor", "A: Add from Prim Tree", "node framing"],
            "editVerification": "Per-operator edit and save steps are source-audited; the ALab Noise walkthrough exercises an actual value edit and Ctrl+S in Qt.",
        })
    UI.write_text(json.dumps(ui, indent=2, ensure_ascii=False) + "\n",
                  encoding="utf-8")
    print("Recorded %d operator UI captures" % len(FIELDS))


if __name__ == "__main__":
    main()
