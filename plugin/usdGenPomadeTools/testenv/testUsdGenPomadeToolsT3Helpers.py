#!/usr/bin/env python3
# testUsdGenPomadeToolsT3Helpers -- T1: the Qt-free half of pomadeT3.py
# (TS-01).
#
# pomadeT3.py imports pxr/Qt only inside function bodies, never at module
# scope (the same discipline pomadeViewport.py itself uses), so it loads
# here in a plain interpreter with no pxr, no Qt and no DLL, exactly like
# its testUsdGenPomadeToolsPanels/Tube/Hierarchy siblings.
#
# What this checks without ever needing a live view or a Qt build:
#   * typeKey's key table is a superset of every name pomadeViewport.py's
#     `keyName` can hand back -- read from pomadeViewport.py's SOURCE (a
#     regex over its `named` dict), not by importing it, so this test never
#     needs pxr either. Digits and letters are asserted directly, since
#     keyName reaches those through QKeyEvent.text()/the Key_A..Key_Z
#     range rather than through that dict;
#   * typeKey rejects an unmapped name before it would need Qt;
#   * check()/info()/failureCount()/resetFailures() count the way every
#     existing T3's own copy of check() does;
#   * statusRecorder() accepts both setStatusSink calling conventions,
#     `sink(text)` (today) and `sink(text, level)` (SS-02), and reads back
#     as a plain list of text either way.
import contextlib
import io
import os
import re
import sys

failures = 0


def check(ok, what):
    global failures
    if ok:
        print("ok:   %s" % what)
    else:
        failures += 1
        print("FAIL: %s" % what)


def testenvDir():
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        pass
    argv = list(sys.argv)
    for i, arg in enumerate(argv):
        if arg == "--testScript" and i + 1 < len(argv):
            return os.path.dirname(os.path.abspath(argv[i + 1]))
        if arg.startswith("--testScript="):
            return os.path.dirname(os.path.abspath(arg.split("=", 1)[1]))
    return os.getcwd()


def keyNamesFromSource(here):
    """The string values of pomadeViewport.keyName's `named` dict, read from
    source text so this test never has to import pxr/Qt to get them."""
    path = os.path.normpath(os.path.join(
        here, "..", "python", "usdGenPomadeTools", "pomadeViewport.py"))
    with open(path, "r") as handle:
        text = handle.read()
    # Every entry in `named` is `QtCore.Qt.Key.Key_X: "name"`; this matches
    # each one regardless of dict-literal formatting or ordering.
    return path, sorted(set(re.findall(r'Key\.Key_\w+:\s*"([^"]*)"', text)))


def main():
    here = testenvDir()
    sys.path.insert(0, here)
    import pomadeT3

    sourcePath, keyNames = keyNamesFromSource(here)
    check(bool(keyNames),
          "found pomadeViewport.keyName's named-key table at %s (%d names)"
          % (sourcePath, len(keyNames)))
    missing = [name for name in keyNames if name not in pomadeT3._KEY_TABLE]
    check(not missing,
          "pomadeT3.typeKey covers every name keyName's dict produces "
          "(missing %r)" % (missing,))

    missingDigits = [d for d in "0123456789" if d not in pomadeT3._KEY_TABLE]
    check(not missingDigits,
          "pomadeT3.typeKey covers every digit (missing %r)"
          % (missingDigits,))
    missingLetters = [c for c in "abcdefghijklmnopqrstuvwxyz"
                      if c not in pomadeT3._KEY_TABLE]
    check(not missingLetters,
          "pomadeT3.typeKey covers every lowercase letter (missing %r)"
          % (missingLetters,))
    missingFn = [n for n in range(1, 13)
                if ("f%d" % n) not in pomadeT3._KEY_TABLE]
    check(not missingFn,
          "pomadeT3.typeKey covers f1-f12 (missing %r)" % (missingFn,))

    raised = False
    try:
        pomadeT3.typeKey(None, "not-a-real-key")
    except KeyError:
        raised = True
    except Exception as exc:                     # pragma: no cover
        check(False, "typeKey raised %r, not KeyError, for an unmapped "
                     "name" % (exc,))
        raised = True
    check(raised,
          "typeKey rejects an unmapped name before it would need Qt "
          "(no pxr/Qt import happened: we are still running)")

    # pomadeT3.check() prints the literal word "FAIL:" on a failing
    # assertion, on purpose (every T3 script relies on that for ctest's
    # FAIL_REGULAR_EXPRESSION). Exercising that path here would trip this
    # test's own FAIL_REGULAR_EXPRESSION, so the deliberate failure below
    # is muted rather than left on stdout.
    pomadeT3.resetFailures()
    pomadeT3.check(True, "a passing assertion")
    with contextlib.redirect_stdout(io.StringIO()):
        pomadeT3.check(False, "a deliberately failing assertion")
    check(pomadeT3.failureCount() == 1,
          "pomadeT3.check()/failureCount() count exactly the failing calls "
          "(got %d)" % pomadeT3.failureCount())
    pomadeT3.resetFailures()
    check(pomadeT3.failureCount() == 0, "resetFailures() zeroes the counter")

    recorder = pomadeT3.statusRecorder()
    recorder("plain text")
    recorder("levelled text", "warning")
    check(list(recorder) == ["plain text", "levelled text"],
          "statusRecorder() reads back as a plain list of text either way "
          "(%r)" % (list(recorder),))
    check(recorder.entries == [("plain text", None),
                               ("levelled text", "warning")],
          "and keeps the full (text, level) pairs on .entries (%r)"
          % (recorder.entries,))

    # StderrCapture works at the file-descriptor level, where Tf prints
    # its warnings (fprintf(stderr)), and still sees Python's own stderr.
    with pomadeT3.StderrCapture() as capture:
        os.write(2, b"pomadeT3 capture probe (fd)" + os.linesep.encode())
        print("pomadeT3 capture probe (python)", file=sys.stderr)
    check(capture.count("pomadeT3 capture probe") == 2,
          "StderrCapture counts fd-2 and sys.stderr text (%r)"
          % (capture.text,))

    print("testUsdGenPomadeToolsT3Helpers: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
