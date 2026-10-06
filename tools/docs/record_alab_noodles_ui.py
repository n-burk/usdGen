# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Register the real Qt ALab Noodles edit walkthrough screenshots."""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
UI = ROOT / "docs/reference/ui.json"
SCENE = "examples/alab/stoat-groom.usda"
CAPTURE = "bin/launch_usdview.ps1 -TestScript tools/docs/capture_alab_noodles_ui.py"
PRIM = "/World/Groom/BrownBody/Ops/noise"
FIELD = "usdGen:noise:magnitude"

RECORDS = [
    {
        "id": "alab-05-select-noise-in-noodles",
        "title": "Select the coat Noise operator",
        "caption": "With BrownBody/Ops/noise selected in usdview's Scene Graph, Noodles Editor shows the prepared operator and its magnitude value, 0.0238. The capture script exercised Add from Prim Tree (A).",
        "ui": "usdview Scene Graph; Window > Noodles Editor; A: Add from Prim Tree",
    },
    {
        "id": "alab-08-layer-editor",
        "title": "Choose a persistent edit target",
        "caption": "Layer Editor lists the prepared scratch layer stoat-ui-edit.usda. Clicking its row sets that persistent layer as the edit target before changing magnitude; the checked-in example remains untouched.",
        "ui": "Window > Layer Editor; Edit Target",
    },
    {
        "id": "alab-06-edit-noise-magnitude",
        "title": "Edit coat Noise magnitude",
        "caption": "In the selected Noise node's usdGen group, a real value-cell click, typing 0.031, and Enter changed usdGen:noise:magnitude from 0.0238 to 0.031 in the scratch edit layer.",
        "ui": "Noodles Editor; usdGen > noise:magnitude value cell",
    },
    {
        "id": "alab-07-save-noise-layer",
        "title": "Save the edited coat layer",
        "caption": "After Ctrl+S in Noodles Editor, magnitude remains 0.031. The capture script verified that the selected scratch layer was saved and no longer dirty; the original scene was not rewritten.",
        "ui": "Noodles Editor; Ctrl+S Save Layer",
    },
]


def main():
    data = json.loads(UI.read_text(encoding="utf-8"))
    ids = {record["id"] for record in RECORDS}
    data["screenshots"] = [r for r in data["screenshots"] if r["id"] not in ids]
    for record in RECORDS:
        image = "docs/site/media/ui/%s.png" % record["id"]
        if not (ROOT / image).is_file():
            raise FileNotFoundError(image)
        data["screenshots"].append({
            **record,
            "image": image,
            "scene": SCENE,
            "capture": CAPTURE,
            "selectedPrim": PRIM,
            "editedField": FIELD,
            "captureExercises": [
                "Scene Graph selection and A: Add from Prim Tree",
                "Layer Editor scratch edit-target selection",
                "Noise magnitude value-cell click, type 0.031, Enter",
                "Ctrl+S saved the scratch layer",
            ],
        })
    UI.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n",
                  encoding="utf-8")
    print("Recorded %d ALab Noodles captures" % len(RECORDS))


if __name__ == "__main__":
    main()
