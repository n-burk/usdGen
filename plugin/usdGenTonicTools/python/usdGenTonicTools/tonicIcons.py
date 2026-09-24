# usdGenTonicTools.tonicIcons -- icon loading for the Tonic dock/shelves.
#
# Sibling of usdGenTools.brushPanels/brushPalette (plugin/usdGenTools):
# the PNGs are white glyphs on a transparent background, tinted per state
# at load time, so one glyph serves normal/hover/checked/disabled without
# separate art. Unlike the brush palette's icons the Tonic set mixes two
# source sizes -- 256 px generated glyphs (IC-02) and 128 px usdRig
# manipulator art reused so the tool row reads as one family -- so the
# loader scales whatever it finds rather than assuming 256.
#
# Qt is only needed for loadIcon()/_tinted(); iconDir()/iconPath()/iconFor()
# stay import-safe without a QApplication so the T1 icon test (plain
# python + PIL) can exercise the name tables and the files on disk without
# a display.
from __future__ import annotations

import os


def _searchDirs():
    """Candidate icons directories, most specific first.

    `USDGENTONIC_ICONS` overrides everything (a working-copy override or a
    test fixture). Then the source tree's
    plugin/usdGenTonicTools/resources/icons, reached as
    <package>/../../resources/icons from this file. Then the staged
    build/usd/usdGenTonicTools/resources/icons, reached the same way a
    package staged at build/python/usdGenTonicTools finds build/usd/...
    (CMakeLists.txt's `_usdgen_tonic_tools_resources`)."""
    dirs = []
    env = os.environ.get("USDGENTONIC_ICONS")
    if env:
        dirs.append(env)
    here = os.path.dirname(os.path.abspath(__file__))
    grandparent = os.path.dirname(os.path.dirname(here))
    dirs.append(os.path.join(grandparent, "resources", "icons"))
    dirs.append(os.path.join(grandparent, "usd", "usdGenTonicTools",
                              "resources", "icons"))
    return dirs


def iconDir():
    """The first candidate directory that exists, or None.

    Callers that need one fixed root (a directory listing, the CMake
    acceptance check) use this; iconPath() below still searches every
    candidate per name, so a name missing from the first existing
    directory can still resolve from a later one."""
    for directory in _searchDirs():
        if directory and os.path.isdir(directory):
            return directory
    return None


def iconPath(name):
    """The PNG for icon `name`, or None when no candidate directory has it."""
    if not name:
        return None
    for directory in _searchDirs():
        if not directory:
            continue
        path = os.path.join(directory, name + ".png")
        if os.path.isfile(path):
            return path
    return None


# -- tinting (Qt) -----------------------------------------------------------

COLORS = {
    "icon": "#c9c9ce",
    "iconOn": "#9fcbff",
    "iconOff": "#56565c",
}

_PIXMAPS = {}


def _pixmap(name):
    """The source QPixmap for icon `name` at its native size, or None.

    Native size is 128 or 256 px depending on which art family supplied
    it (see resources/icons/README.md); callers scale as needed, so this
    never assumes one fixed dimension."""
    if name in _PIXMAPS:
        return _PIXMAPS[name]
    pix = None
    path = iconPath(name)
    if path is not None:
        from pxr.Usdviewq.qt import QtGui
        loaded = QtGui.QPixmap(path)
        if not loaded.isNull():
            pix = loaded
    _PIXMAPS[name] = pix
    return pix


def _tinted(pixmap, color):
    from pxr.Usdviewq.qt import QtCore, QtGui
    out = QtGui.QPixmap(pixmap.size())
    out.fill(QtCore.Qt.GlobalColor.transparent)
    painter = QtGui.QPainter(out)
    painter.drawPixmap(0, 0, pixmap)
    painter.setCompositionMode(
        QtGui.QPainter.CompositionMode.CompositionMode_SourceIn)
    painter.fillRect(out.rect(), QtGui.QColor(color))
    painter.end()
    return out


def loadIcon(name, onName=None):
    """A tinted QIcon (normal / hover / checked / disabled), or None.

    The PNGs are white glyphs: Off tints neutral, On (checked) tints
    accent, Disabled dims. `onName` gives the checked state its own
    glyph. None when the PNG is missing anywhere on the search path: the
    caller falls back to text, same contract as brushPalette.loadIcon."""
    from pxr.Usdviewq.qt import QtGui
    off = _pixmap(name)
    if off is None:
        return None
    on = _pixmap(onName) if onName else off
    if on is None:
        on = off
    Mode, State = QtGui.QIcon.Mode, QtGui.QIcon.State
    icon = QtGui.QIcon()
    icon.addPixmap(_tinted(off, COLORS["icon"]), Mode.Normal, State.Off)
    icon.addPixmap(_tinted(off, "#ffffff"), Mode.Active, State.Off)
    icon.addPixmap(_tinted(on, COLORS["iconOn"]), Mode.Normal, State.On)
    icon.addPixmap(_tinted(on, COLORS["iconOn"]), Mode.Active, State.On)
    icon.addPixmap(_tinted(off, COLORS["iconOff"]), Mode.Disabled,
                   State.Off)
    icon.addPixmap(_tinted(on, COLORS["iconOff"]), Mode.Disabled, State.On)
    return icon


def iconFor(kind, id):
    """The icon name for a (kind, id) pair via tonicModes.ICONS, or None.

    Import is local to dodge a cycle: tonicModes is Qt-free and does not
    import this module, so tonicIcons owns the direction of the edge."""
    from . import tonicModes
    return tonicModes.ICONS.get((kind, id))
