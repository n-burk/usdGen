#!/usr/bin/env python3
# testUsdGenTonicToolsIcons -- T1: every icon name the dock name tables
# reference (tonicModes.ICONS, tonicPanels.ACTION_ICONS), and every name
# in resources/icons/manifest.txt, resolves to a valid
# white-glyph-on-transparent PNG that tonicIcons.loadIcon can tint (IC-02;
# IC-01 landed the SKIP-for-missing version this replaces).
#
# Plain python + PIL, no Qt, no pxr: tonicModes/tonicPanels/tonicIcons all
# stay import-safe without a display (tonicIcons defers its
# `from pxr.Usdviewq.qt import ...` into the functions that actually build
# a QIcon), so this loads them the same file-path way
# testUsdGenTonicToolsPanels.py does and never touches Qt.
#
# Strict (IC-02): every name is a hard FAIL, not a SKIP, when its PNG is
# missing or malformed. Coverage band is 15-97%, not the generator's
# tighter 40-95%: the usdRig-derived manipulator/action art
# (tool_select/move/rotate/scale, act_undo/redo/settings, orient_world/
# tube, pivot_centre/each) is thinner line art reused as-is, and this
# test must still pass it.
import importlib.util
import os
import sys

SRC = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "python", "usdGenTonicTools"))
MANIFEST = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "resources", "icons", "manifest.txt"))
COVERAGE_MIN = 0.15
COVERAGE_MAX = 0.97
# The committed provenance: the regeneration script and the icon README.
PROVENANCE = (
    os.path.normpath(os.path.join(os.path.dirname(MANIFEST), "README.md")),
    MANIFEST,
    os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "..", "..", "..", "bin",
                                  "gen_tonic_icons.sh")),
)
# A drive-letter path (not the "s:/" of "https://"), a Windows user
# profile or a per-session temp folder: meaningless on any other machine.
MACHINE_PATH = r"(?<![A-Za-z])[A-Za-z]:[\\/]|[\\/]Users[\\/]|AppData|scratchpad"


def _load(name):
    spec = importlib.util.spec_from_file_location(
        name, os.path.join(SRC, "%s.py" % name))
    module = importlib.util.module_from_spec(spec)
    # tonicPanels imports sibling helpers with a package/file-path
    # fallback; the bare-name registration satisfies that fallback for a
    # file-path load, same trick testUsdGenTonicToolsPanels.py needs.
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


failures = 0


def check(ok, what):
    global failures
    if ok:
        print("ok:   %s" % what)
    else:
        failures += 1
        print("FAIL: %s" % what)


def checkProvenance():
    """The committed script and README name no machine-specific path."""
    import re
    pattern = re.compile(MACHINE_PATH)
    for path in PROVENANCE:
        if not os.path.isfile(path):
            check(False, "%s exists" % path)
            continue
        with open(path, encoding="utf-8") as handle:
            hits = [(number, line.strip())
                    for number, line in enumerate(handle, 1)
                    if pattern.search(line)]
        check(not hits, "%s names no machine-specific path (%r)"
              % (os.path.basename(path), hits[:3]))


def main():
    checkProvenance()
    try:
        from PIL import Image
    except ImportError as exc:
        print("SKIP: testUsdGenTonicToolsIcons needs Pillow (%s)" % exc)
        return 77

    tonicModes = _load("tonicModes")
    # tonicPanels imports these siblings with a package/file-path fallback
    # ("from . import tonicFill, ..." then "import tonicFill, ..."); a
    # file-path load needs them pre-registered under their bare names
    # first, same as testUsdGenTonicToolsPanels.py.
    for sibling in ("tonicFill", "tonicGraph", "tonicHierarchy",
                    "tonicSculpt", "tonicTube"):
        _load(sibling)
    tonicPanels = _load("tonicPanels")
    tonicIcons = _load("tonicIcons")

    # One combined name -> its "used by" labels, deduped: several
    # (kind, id) pairs / action ids can share one glyph (Subdivide's
    # sub-mode and its action button both point at sub_hier_subdivide).
    names = {}
    for (kind, itemId), name in tonicModes.ICONS.items():
        names.setdefault(name, []).append("%s/%s" % (kind, itemId))
    for actionId, name in tonicPanels.ACTION_ICONS.items():
        names.setdefault(name, []).append("action:%s" % actionId)

    check(bool(names), "tonicModes.ICONS/tonicPanels.ACTION_ICONS is "
          "non-empty")

    manifestNames = []
    if not os.path.isfile(MANIFEST):
        check(False, "manifest.txt exists (%s)" % MANIFEST)
    else:
        with open(MANIFEST, "r") as handle:
            for lineNo, line in enumerate(handle, 1):
                line = line.rstrip("\n")
                if not line.strip() or line.lstrip().startswith("#"):
                    continue
                check("|" in line,
                      "manifest.txt line %d has a name|prompt separator "
                      "(%r)" % (lineNo, line))
                if "|" not in line:
                    continue
                manifestNames.append(line.split("|", 1)[0].strip())
        check(bool(manifestNames), "manifest.txt is non-empty")
    for name in manifestNames:
        names.setdefault(name, []).append("manifest.txt")

    found = 0
    for name in sorted(names):
        label = "%s (%s)" % (name, ", ".join(names[name]))
        path = tonicIcons.iconPath(name)
        if path is None:
            check(False, "%s has a PNG" % label)
            continue
        found += 1
        with Image.open(path) as raw:
            check(raw.mode == "RGBA",
                  "%s is RGBA (got %s)" % (label, raw.mode))
            image = raw.convert("RGBA")
        w, h = image.size
        check(w == h and w in (128, 256),
              "%s is square, 128 or 256 px (got %dx%d)" % (label, w, h))
        bbox = image.getchannel("A").getbbox()
        if bbox is None:
            check(False, "%s has a non-empty alpha channel" % label)
            continue
        bx0, by0, bx1, by1 = bbox
        coverage = ((bx1 - bx0) * (by1 - by0)) / float(w * h)
        check(COVERAGE_MIN <= coverage <= COVERAGE_MAX,
              "%s alpha-bbox coverage in [%.0f%%, %.0f%%] (got %.1f%%)"
              % (label, COVERAGE_MIN * 100.0, COVERAGE_MAX * 100.0,
                 coverage * 100.0))

    print("testUsdGenTonicToolsIcons: %d name(s), %d icon(s) checked, "
          "%d failure(s)" % (len(names), found, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
