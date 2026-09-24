# Startup scrub for processes that must import this repo's OpenUSD prefix.
#
# nanousd ships an editable install whose import hook (a finder on
# sys.meta_path) redirects `import pxr` to nanousd's own shim modules,
# ahead of every sys.path entry. Any process that needs the prefix's real
# pxr -- usdview, testusdview, plain-python tooling -- then sees stub
# modules whose attributes are all the same placeholder (e.g.
# Usdviewq.Launcher() is None), no matter how PYTHONPATH is ordered.
#
# Python imports this module automatically at startup when its directory
# is on PYTHONPATH (see `site`), before the launched script runs, so the
# launchers put this directory first: only the first sitecustomize on the
# path is honored. The scrub is narrow: only finders whose module or
# class name mentions nanousd are removed, plus any pxr modules such a
# finder already served (the shim serves synthetic modules with no
# __file__, while the real pxr package always has one). Without nanousd
# installed this is a no-op.


def _is_nanousd_finder(finder):
    name = (type(finder).__module__ + "." +
            type(finder).__name__).lower()
    return "nanousd" in name


def _is_shim_module(module):
    filename = getattr(module, "__file__", "") or ""
    return not filename or "nanousd" in filename.lower()


def _scrub_nanousd_import_hook():
    """Drop nanousd's meta-path hook so the prefix's pxr imports."""
    import sys
    scrubbed = [f for f in sys.meta_path if _is_nanousd_finder(f)]
    for finder in scrubbed:
        try:
            sys.meta_path.remove(finder)
        except ValueError:
            pass
    if scrubbed:
        for name in [n for n in list(sys.modules)
                     if n == "pxr" or n.startswith("pxr.")]:
            try:
                if _is_shim_module(sys.modules[name]):
                    del sys.modules[name]
            except KeyError:
                pass
    return bool(scrubbed)


_scrub_nanousd_import_hook()
