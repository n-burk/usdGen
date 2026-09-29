#!/usr/bin/env python3
"""docs/pomade-tool.md hotkey coverage (DOC-01).

Every key the Pomade tool binds must appear in the manual as a code span
(`Ctrl+Shift+A`, `F10`, `[` ...). The keys are read from the shipped
tables, never typed here:

  * pomadeModes.MODES and the *_SUBMODES tables (the mode and sub-mode
    shelves), pomadeDockIds.TRANSFORM_TOOLS / GIZMO_TOGGLES (Q/W/E/R, L, P);
  * pomadeModes.HotkeyAction, probed over every key and modifier
    combination, for the global keys (undo/redo, selection commands,
    Delete, Backspace, Enter, brackets, Shift+D ...);
  * the key spellings inside pomadeModes.HINTS (the dock's instruction
    lines), so a hint can never advertise a key the manual does not;
  * pomadeViewport's HOLD_KEYS (J/X) and SIZE_KEYS (+/=/-), read from its
    source with ast because that module imports Qt-side siblings.

Plain python, no pxr and no Qt: the modules load by file path.
"""

import ast
import importlib.util
import os
import re
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(_HERE))
_SRC = os.path.join(_ROOT, "plugin", "usdGenPomadeTools", "python",
                    "usdGenPomadeTools")
_MANUAL = os.path.join(_ROOT, "docs", "pomade-tool.md")

failures = 0


def check(ok, what):
    global failures
    if ok:
        print("ok:   %s" % what)
    else:
        failures += 1
        print("FAIL: %s" % what)


def _load(name):
    spec = importlib.util.spec_from_file_location(
        name, os.path.join(_SRC, "%s.py" % name))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


# HotkeyAction's lower-case key names -> the manual's spelling.
_KEY_WORDS = {"escape": "Escape", "enter": "Enter", "backspace": "Backspace",
              "delete": "Delete", "up": "Up", "down": "Down"}


def spell(key, modifiers=()):
    """'ctrl'+'shift'+'a' -> 'Ctrl+Shift+A' (the manual's order)."""
    key = str(key)
    word = _KEY_WORDS.get(key.lower(), key.upper() if len(key) == 1
                          else key[:1].upper() + key[1:])
    prefix = "".join(name + "+" for name in ("Ctrl", "Shift", "Alt")
                     if name.lower() in modifiers)
    return prefix + word


def tableKeys(modes, dockIds):
    keys = {}
    for mode in modes.MODES:
        keys.setdefault(mode.hotkey, "mode %s" % mode.id)
    for table in (modes.GRAPH_SUBMODES, modes.TUBE_SUBMODES,
                  modes.FILL_SUBMODES, modes.HIERARCHY_SUBMODES,
                  modes.SCULPT_SUBMODES):
        for sub in table:
            keys.setdefault(sub.hotkey, "sub-mode %s" % sub.id)
    for tool in dockIds.TRANSFORM_TOOLS:
        keys.setdefault(tool.hotkey, "tool %s" % tool.id)
    for toggleId, hotkey in dockIds.GIZMO_TOGGLES:
        keys.setdefault(hotkey, "gizmo toggle %s" % toggleId)
    return keys


def actionKeys(modes):
    """Every (key, modifiers) HotkeyAction answers with a non-shelf action."""
    universe = (list("abcdefghijklmnopqrstuvwxyz0123456789") +
                ["escape", "enter", "backspace", "delete", "up", "down",
                 "[", "]"])
    combos = [(), ("shift",), ("ctrl",), ("ctrl", "shift")]
    keys = {}
    for key in universe:
        for mods in combos:
            action = modes.HotkeyAction(key, frozenset(mods), True, False)
            if action is None:
                continue
            if action[0] in (modes.ACTION_MODE, modes.ACTION_SUBMODE):
                continue               # the shelf tables cover these
            if mods and modes.HotkeyAction(key, frozenset(), True,
                                           False) == action:
                continue               # Escape cancels whatever is held
            keys.setdefault(spell(key, mods), action[0])
    return keys


_HINT_TOKEN = re.compile(
    r"(?:(?:Ctrl|Shift|Alt)\+)+(?:Down|Up|[A-Z])(?:/(?:Down|Up|[A-Z]))*"
    r"|F\d+-F\d+|\b[QWER](?:/[QWER])+\b|\bEsc\b|\bEnter\b|\bBackspace\b"
    r"|\bDelete\b|\[ \]|\bF\+drag")


def hintKeys(modes):
    """The keys the dock's instruction lines name, expanded."""
    keys = {}
    for (modeId, subId), text in modes.HINTS.items():
        where = "hint %s/%s" % (modeId, subId or "*")
        for token in _HINT_TOKEN.findall(text):
            if token == "Esc":
                out = ["Escape"]
            elif token == "[ ]":
                out = ["[", "]"]
            elif token == "F+drag":
                out = ["F"]
            elif re.match(r"F\d+-F\d+$", token):
                lo, hi = (int(v) for v in re.findall(r"\d+", token))
                out = ["F%d" % n for n in range(lo, hi + 1)]
            elif "/" in token:
                # "Ctrl+Down/Up" -> Ctrl+Down, Ctrl+Up; "Q/W/E/R" -> Q ...
                parts = token.split("/")
                if "+" in parts[0]:
                    prefix = parts[0].rsplit("+", 1)[0] + "+"
                    out = [parts[0]] + [prefix + p for p in parts[1:]]
                else:
                    out = parts
            else:
                out = [token]
            for key in out:
                keys.setdefault(key, where)
    return keys


def viewportKeys():
    """HOLD_KEYS and SIZE_KEYS from pomadeViewport.py's source."""
    tree = ast.parse(open(os.path.join(_SRC, "pomadeViewport.py"),
                          encoding="utf-8").read())
    keys = {}
    for node in tree.body:
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        name = getattr(node.targets[0], "id", "")
        if name not in ("HOLD_KEYS", "SIZE_KEYS"):
            continue
        value = ast.literal_eval(node.value)
        for key in value:
            keys.setdefault(key.upper(), "pomadeViewport.%s" % name)
    return keys


def main():
    if not os.path.isfile(_MANUAL):
        print("FAIL: %s is missing" % _MANUAL)
        return 1
    manual = open(_MANUAL, encoding="utf-8").read()
    spans = set(re.findall(r"`([^`\n]+)`", manual))
    modes = _load("pomadeModes")
    dockIds = _load("pomadeDockIds")

    groups = (("shelf and tool tables", tableKeys(modes, dockIds)),
              ("HotkeyAction", actionKeys(modes)),
              ("pomadeModes.HINTS", hintKeys(modes)),
              ("viewport holds and size keys", viewportKeys()))
    for title, keys in groups:
        check(bool(keys), "%s yields keys to check" % title)
        missing = sorted(key for key in keys if key not in spans)
        for key in missing:
            print("  missing `%s` (%s)" % (key, keys[key]))
        check(not missing, "every %s key appears in docs/pomade-tool.md "
              "(%d keys)" % (title, len(keys)))

    # The one selection-modifier table the loops share (pomadeLoops.
    # selectModeFor) is spelled out in the dock hint; the manual must
    # carry the same line.
    flat = " ".join(manual.split())
    check(modes.SELECTION_HINT in flat,
          "the manual quotes the selection-modifier line")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
