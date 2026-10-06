# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Headless verification that Houdini loads the usdGen plugins.

Runs under hython via ``launch_houdini_usdgen.ps1 -Mode check`` (the
wrapper sets PATH and HOUDINI_USD_DSO_PATH first). Optional argv[1] is the
usdGen install prefix; otherwise it is resolved from this script's location
(``bin/`` next to ``build-houdini/install``).

Checks, in order:
  0. The interpreter is Houdini's Python 3.13 with no hostile
     PATH/PYTHONPATH/PXR_PLUGINPATH_NAME entry (a stock cp310 pxr package,
     python310.dll, a stock-build DLL dir, or anything MoonRay)
     surviving the wrapper scrub.
  1. The install's USD plugin directories are on HOUDINI_USD_DSO_PATH.
  2. The ``usdGenSchema``, ``usdGenImaging``, ``usdGenPomade`` and
     ``usdGenShaders`` plugins are registered and load (loading pulls in
     the usdGen DLLs and their USD linkage).
  3. The ``UsdGenGroom`` and ``UsdGenDescription`` schema types are known.
  4. A UsdGenGroom prim can be authored on an in-memory stage.

Exits 0 with an OK summary, or 1 with the first failure.
"""

import glob
import os
import sys

_PLUGINS = ("usdGenSchema", "usdGenImaging", "usdGenPomade", "usdGenShaders")


def _fail(message):
    print("CHECK FAILED: %s" % message)
    return 1


def main(argv):
    here = os.path.dirname(os.path.abspath(__file__))
    if len(argv) > 1:
        install = os.path.abspath(argv[1])
    else:
        install = os.path.normpath(
            os.path.join(here, "..", "build-houdini", "install"))
    usd_dir = os.path.join(install, "lib", "usd")
    if not os.path.isfile(os.path.join(
            usd_dir, "usdGenSchema", "resources", "plugInfo.json")):
        return _fail("no usdGenSchema plugInfo under %s" % usd_dir)

    # 0. The interpreter is Houdini's own 3.13 and no hostile entry
    # survived the wrapper's scrub: a stock (cp310) pxr on PYTHONPATH
    # fails with "DLL load failed while importing _tf", so fail fast
    # here if any such segment is still present. The install's own
    # directories (prepended after the scrub) and Houdini's bin on PATH
    # (ships python313.dll) are exempt.
    install_norm = os.path.normcase(os.path.abspath(install))
    hfs_raw = (os.environ.get("HOUDINI_ROOT") or os.environ.get("HFS") or "")
    if not hfs_raw and "hython" in os.path.basename(sys.executable).lower():
        hfs_raw = os.path.dirname(
            os.path.dirname(os.path.abspath(sys.executable)))
    hfs = os.path.normcase(os.path.abspath(hfs_raw)) if hfs_raw else ""
    # The combined launcher prepends a second install's lib dir too; its
    # prefix arrives via the environment (batch `set` exports to children).
    extra_roots = [os.path.normcase(os.path.abspath(os.environ[v]))
                   for v in ("INSTALL", "RIGINSTALL", "GENINSTALL",
                             "MAYAINSTALL")
                   if os.environ.get(v)]

    def _under(path, root):
        return bool(root) and (path == root or
                               path.startswith(root + os.sep))

    if sys.version_info[:2] != (3, 13):
        return _fail("expected Houdini Python 3.13, got %s"
                      % sys.version.split()[0])
    # hython sets PYTHONHOME to its own python313 at startup; only a
    # foreign value (e.g. a CPython 3.10 home) is hostile.
    home = os.environ.get("PYTHONHOME")
    if home and not _under(os.path.normcase(os.path.abspath(home)), hfs):
        return _fail("PYTHONHOME points outside Houdini (%r); the wrapper "
                      "must clear it" % home)

    for var in ("PATH", "PYTHONPATH", "PXR_PLUGINPATH_NAME"):
        for entry in os.environ.get(var, "").split(";"):
            entry = entry.strip().strip('"')
            if not entry:
                continue
            seg = os.path.normcase(os.path.abspath(entry))
            if _under(seg, install_norm):
                continue
            if any(_under(seg, root) for root in extra_roots):
                continue
            if var == "PATH" and _under(seg, hfs):
                continue
            if glob.glob(os.path.join(entry, "python3*.dll")):
                return _fail("%s still carries %s (ships python3*.dll); "
                             "the wrapper scrub missed it" % (var, entry))
            if os.path.isfile(os.path.join(entry, "pxr", "__init__.py")):
                return _fail("%s still carries %s (ships a pxr package); "
                             "the wrapper scrub missed it" % (var, entry))
            for leaf in ("usd_usd.dll", "rigExec.dll", "usdGen*.dll",
                         "usdMayaRig.dll", "hdMoonray*.dll"):
                if glob.glob(os.path.join(entry, leaf)):
                    return _fail("%s still carries %s (ships %s); "
                                 "the wrapper scrub missed it"
                                 % (var, entry, leaf))
            if "moonray" in seg:
                return _fail("%s still carries %s (MoonRay); "
                             "the wrapper scrub missed it" % (var, entry))
    print("OK: interpreter is 3.13 with no hostile environment entry")

    # 1. The wrapper's plugin path survived into this process. Houdini
    # discovers USD plugins through HOUDINI_USD_DSO_PATH, not
    # PXR_PLUGINPATH_NAME, so that is what is checked.
    plugin_path = os.environ.get("HOUDINI_USD_DSO_PATH", "")
    entries = [os.path.normcase(os.path.abspath(e)) for e in
               plugin_path.split(";") if e and e != "&"]
    want = [os.path.normcase(os.path.abspath(os.path.join(
                usd_dir, name, "resources")))
            for name in _PLUGINS]
    if not all(w in entries for w in want):
        return _fail("HOUDINI_USD_DSO_PATH lacks %s (got %r)"
                     % (usd_dir, plugin_path))
    print("OK: HOUDINI_USD_DSO_PATH carries the install")

    # 2. Every USD plugin registers and loads.
    from pxr import Plug, Tf, Usd
    if hfs and not _under(os.path.normcase(os.path.abspath(Tf.__file__)),
                          hfs):
        return _fail("pxr resolves to %s, outside HOUDINI_ROOT" % Tf.__file__)
    print("OK: pxr resolves inside the Houdini install")
    registry = Plug.Registry()
    for name in _PLUGINS:
        plugin = registry.GetPluginWithName(name)
        if not plugin:
            return _fail("plugin %r is not registered" % name)
        if not plugin.Load():
            return _fail("plugin %r is registered but would not load" % name)
        print("OK: plugin %s loads from %s" % (name, plugin.path))

    # 3. The schema types resolve.
    for type_name in ("UsdGenGroom", "UsdGenDescription"):
        found = Tf.Type.FindByName(type_name)
        if not found:
            return _fail("TfType %s is unknown after loading plugins"
                         % type_name)
    print("OK: TfType UsdGenGroom/UsdGenDescription resolve")

    # 4. A groom prim authors onto a stage.
    stage = Usd.Stage.CreateInMemory()
    prim = stage.DefinePrim("/Groom", "UsdGenGroom")
    if not prim.IsValid() or prim.GetTypeName() != "UsdGenGroom":
        return _fail("could not define a UsdGenGroom prim")
    print("OK: UsdGenGroom prim authors (valid=%s)" % prim.IsValid())

    print("ALL USDGEN HOUDINI CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
