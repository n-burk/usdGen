# tonicTestPackage -- import the Qt-free half of usdGenTonicTools in a T0.
#
# The package's __init__ is the usdview plugin container: it imports pxr.Tf
# and pxr.Usdviewq.plugin at module top, because usdview discovers
# `usdGenTonicTools.UsdGenTonicToolsPluginContainer` by attribute on the
# imported package and the class cannot subclass PluginContainer without it.
# A T0 that runs under a plain interpreter has no pxr, so `import
# usdGenTonicTools.tonicLoops` used to raise ImportError and the test
# skipped -- three of them did, silently, for as long as the loops existed.
#
# The modules under test do not need the container; they need the PACKAGE,
# because they import their siblings relatively (`from . import tonicLib`).
# So this helper binds a package object for the name without executing
# __init__.py: a namespace-style module whose __path__ is the source
# directory. Relative imports then resolve exactly as they do in usdview,
# the pxr import never happens, and no module under test is loaded twice
# under two names (which a per-file loader would do).
#
# Inside usdview the real package is already imported; install() sees it in
# sys.modules and leaves it alone.
import importlib
import importlib.machinery
import importlib.util
import os
import sys

PACKAGE = "usdGenTonicTools"

SOURCE_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "python", PACKAGE))


def install():
    """Bind `usdGenTonicTools` to the source tree without its __init__.

    Returns the package module. Idempotent, and a no-op when the real
    package (or a previous install) is already bound.
    """
    existing = sys.modules.get(PACKAGE)
    if existing is not None:
        return existing
    if not os.path.isdir(SOURCE_DIR):
        raise ImportError("no usdGenTonicTools source at %s" % SOURCE_DIR)
    spec = importlib.machinery.ModuleSpec(PACKAGE, None, is_package=True)
    spec.submodule_search_locations = [SOURCE_DIR]
    package = importlib.util.module_from_spec(spec)
    sys.modules[PACKAGE] = package
    return package


def load(*names):
    """install(), then import each named submodule; returns them in order."""
    install()
    return tuple(importlib.import_module("%s.%s" % (PACKAGE, name))
                 for name in names)
