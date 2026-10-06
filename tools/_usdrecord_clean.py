#!/usr/bin/env python3
"""Run usdrecord with this checkout's OpenUSD and usdGen plugins.

Python -S avoids third-party .pth hooks that can replace the real pxr module.
This entry point is also used by render_operator_examples.py.
"""

# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT

import os
from pathlib import Path
import runpy
import sys
import sysconfig


ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("USDGEN_BUILD", ROOT / "build"))
USD = Path(os.environ.get("USD", ROOT.parent / "usdRig" / "usd-install"))


def main():
    usd_python = USD / "lib" / "site-packages"
    if not usd_python.exists():
        raise RuntimeError(f"Missing OpenUSD Python modules: {usd_python}")
    sys.path.insert(0, str(usd_python))
    # Python -S omits site-packages completely; add it without processing .pth.
    site_packages = Path(sysconfig.get_paths()["purelib"])
    if site_packages.exists():
        sys.path.append(str(site_packages))

    plugin_dirs = [BUILD / "usd" / leaf / "resources" for leaf in (
        "usdGenSchema", "usdGenImaging", "usdGenShaders", "usdGenTools",
        "usdGenPomade", "usdGenPomadeTools")]
    plugin_dirs += [USD / "plugin" / "usd", USD / "lib" / "usd"]
    os.environ["PXR_PLUGINPATH_NAME"] = os.pathsep.join(
        str(p) for p in plugin_dirs if p.exists())
    os.environ["PATH"] = os.pathsep.join((str(BUILD), str(USD / "bin"),
                                             str(USD / "lib"), os.environ["PATH"]))
    os.environ.setdefault("HDX_MSAA_SAMPLE_COUNT", "16")
    os.environ["USDGENPOMADE_TEST_TUBE"] = "0"

    sys.argv = [str(USD / "bin" / "usdrecord"), *sys.argv[1:]]
    runpy.run_path(sys.argv[0], run_name="__main__")


if __name__ == "__main__":
    main()
