#!/usr/bin/env bash
# T1 schema metadata check. Needs OpenUSD's Python bindings (pxr). CI builds
# the prefix with --no-python, so this harness skips (ctest 77) instead of
# failing when pxr cannot be imported.
set -u
_schema_check_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

pick_python() {
    if [ -n "${PY:-}" ] && [ -x "$PY" ]; then
        printf '%s' "$PY"
        return 0
    fi
    if command -v python3 >/dev/null 2>&1; then
        command -v python3
        return 0
    fi
    return 1
}

if ! PY="$(pick_python)"; then
    echo "SKIP [setup] python interpreter not found"
    exit 77
fi

# Workstation env (plugin path, USD prefix). Do not let a missing sibling
# OpenUSD prefix abort the skip path: CI has no /home/burkard/.venv and no
# ../OpenUSD_26_08.
set +e
# shellcheck disable=SC1091
source "$_schema_check_root/bin/_env.sh"
set -u
if [ -n "${PY:-}" ] && [ -x "$PY" ]; then
    :
elif ! PY="$(pick_python)"; then
    echo "SKIP [setup] python interpreter not found"
    exit 77
fi

if ! "$PY" -c "from pxr import Sdf, Usd" >/dev/null 2>&1; then
    echo "SKIP [setup] cannot import pxr (OpenUSD python bindings unavailable)"
    exit 77
fi

exec "$PY" "$_schema_check_root/tests/checks/check_expression_schema.py"
