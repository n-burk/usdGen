# The expression library: saved `.se` files the editor can load and save.
#
# SeExpr's ExprBrowser reads a set of library paths and shows their expression
# files in a tree. The same idea here: a read-only "usdGen" library of shipped
# presets, and a writable user library, with `USDGEN_EXPRESSION_PATH` able to
# name others.
#
# No Qt: the paths, the file format and the domain marker are all testable in a
# plain python process, and the browser widget in exprWidgets only presents
# them.

import os
import re
from collections import namedtuple

SUFFIX = ".se"

# Where a user's own expressions go when nothing else says otherwise.
USER_DIRECTORY_NAME = ".usdGenExpressions"

# A path list, the platform's own separator, most-preferred first. Its FIRST
# entry becomes the writable user library, which is what makes the variable
# useful for a studio that keeps its expressions somewhere shared.
PATH_ENVIRONMENT = "USDGEN_EXPRESSION_PATH"

# The evaluation domain a saved expression was written for, so the browser can
# hide the ones that cannot apply where the editor is pointed. A plain comment,
# because a `.se` file is expression source and nothing else.
_DOMAIN_RE = re.compile(r"^[ \t]*#[ \t]*domain[ \t]*:[ \t]*"
                        r"(groom|primitive|point)\b", re.MULTILINE | re.I)

Entry = namedtuple("Entry", "name path library domain")
Library = namedtuple("Library", "label path writable")


def DomainOf(text):
    """The `# domain: point` marker in `text`, or "" when it carries none."""
    match = _DOMAIN_RE.search(text or "")
    return match.group(1).lower() if match else ""


def SetDomain(text, domain):
    """`text` with its domain marker set to `domain` (or removed when empty).

    The marker stays on its own first line, so saving never disturbs the
    expression itself."""
    body = _DOMAIN_RE.sub("", text or "", count=1).lstrip("\n")
    if not domain:
        return body
    return "# domain: %s\n%s" % (domain, body)


def UserDirectory():
    """The library saves go to. It need not exist yet."""
    explicit = os.environ.get(PATH_ENVIRONMENT, "")
    for candidate in explicit.split(os.pathsep):
        if candidate.strip():
            return os.path.abspath(candidate.strip())
    return os.path.join(os.path.expanduser("~"), USER_DIRECTORY_NAME)


def _shippedDirectories():
    """Where the presets that ship with the plugin may sit.

    The package is staged into <build>/python/usdGenTools and the resources
    beside it into <build>/usd/usdGenTools/resources, so one walk up from this
    file finds both that layout and the source tree it was copied from."""
    here = os.path.dirname(os.path.abspath(__file__))
    up = os.path.dirname(os.path.dirname(here))
    return [
        os.path.join(here, "expressions"),
        os.path.join(up, "usd", "usdGenTools", "resources", "expressions"),
        os.path.join(up, "resources", "expressions"),
        os.path.join(os.path.dirname(up), "usdGenTools", "resources",
                     "expressions"),
    ]


def Libraries():
    """Every library to show, most specific first.

    The user library is always offered even when it does not exist yet: it is
    where Save puts things, so naming it is more useful than hiding it."""
    found = []
    seen = set()

    def add(label, path, writable):
        path = os.path.abspath(path)
        key = os.path.normcase(path)
        if key in seen:
            return
        seen.add(key)
        found.append(Library(label, path, writable))

    user = UserDirectory()
    add("user", user, True)
    for extra in os.environ.get(PATH_ENVIRONMENT, "").split(os.pathsep)[1:]:
        if extra.strip():
            add(os.path.basename(extra.strip().rstrip("/\\")) or "path",
                extra.strip(), False)
    for directory in _shippedDirectories():
        if os.path.isdir(directory):
            add("usdGen", directory, False)
            break
    return found


def ListExpressions(libraries=None):
    """Every `.se` file in the libraries, sorted by library then name."""
    found = []
    for library in (libraries if libraries is not None else Libraries()):
        if not os.path.isdir(library.path):
            continue
        for name in sorted(os.listdir(library.path)):
            if not name.endswith(SUFFIX):
                continue
            path = os.path.join(library.path, name)
            if not os.path.isfile(path):
                continue
            found.append(Entry(name[:-len(SUFFIX)], path, library.label,
                               DomainOf(Load(path))))
    return found


def Load(path):
    """The text of a saved expression. "" when it cannot be read."""
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return handle.read()
    except (IOError, OSError, UnicodeDecodeError):
        return ""


def SanitiseName(name):
    """`name` reduced to something that is safe as a file name.

    Whitespace and separators become underscores rather than being dropped, so
    two different names cannot collapse onto one file."""
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", (name or "").strip())
    cleaned = cleaned.strip("._") or "expression"
    if cleaned.endswith(SUFFIX):
        cleaned = cleaned[:-len(SUFFIX)]
    return cleaned


def Save(name, text, domain="", directory=None):
    """Write `text` as <directory>/<name>.se and return the path.

    `directory` defaults to the user library, which is created when it is
    missing. Raises OSError when the write fails; the caller says so in the
    status strip rather than this deciding what that means."""
    directory = directory or UserDirectory()
    if not os.path.isdir(directory):
        os.makedirs(directory)
    path = os.path.join(directory, SanitiseName(name) + SUFFIX)
    body = SetDomain(text, domain)
    if not body.endswith("\n"):
        body += "\n"
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(body)
    return path
