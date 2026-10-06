# usdGen shared environment (plan/10 §3.6).
# Sourced, never executed:  source bin/_env.sh
#
# Contract: after sourcing in a clean shell, `usdcat` is on PATH, and USD,
# GEN, GENBUILD and PY are exported. Every path resolves from this file's
# own location so the checkout can live anywhere. Any of GEN / GENBUILD /
# USD / PY / VENV already set in the environment is kept (override).
#
# Note: this host has no system OpenUSD (only the sibling build at
# ../OpenUSD_26_08), so the USD default is that sibling prefix.

_ENV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"

export GEN="${GEN:-$(cd "$_ENV_DIR/.." && pwd)}"
export GENBUILD="${GENBUILD:-$GEN/build}"
export USD="${USD:-$(cd "$GEN/.." && pwd)/OpenUSD_26_08}"

# OpenUSD's tools are python scripts; PY is their interpreter.
# VENV is optional. When it is unset, PY defaults to python3 on PATH.
if [ -n "${VENV:-}" ]; then
    export PY="${PY:-$VENV/bin/python3}"
else
    export PY="${PY:-$(command -v python3 || true)}"
fi

# usdGen binaries/tests (GENBUILD) and the OpenUSD tools (usdcat, usdview,
# usdrecord, ...) must both be launchable.
export PATH="$GENBUILD:$USD/bin:$PATH"

# Loader path: OpenUSD libraries first, then the usdGen build tree.
export LD_LIBRARY_PATH="$USD/lib:$GENBUILD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# macOS twin (usdRig set only DYLD_ and broke Linux); Linux uses LD_.
export DYLD_LIBRARY_PATH="$LD_LIBRARY_PATH"

# Python tools: usdGen build-tree python package + OpenUSD's site-packages.
PY_SITE="$(ls -d "$USD"/lib/python*/site-packages 2>/dev/null | head -1 || true)"
if [ -z "$PY_SITE" ] && [ -d "$USD/lib/python" ]; then
    PY_SITE="$USD/lib/python"
fi
export PYTHONPATH="$GENBUILD/python${PY_SITE:+:$PY_SITE}${PYTHONPATH:+:$PYTHONPATH}"

# Plugin discovery: build-tree usdGen resources win (the generated,
# LibraryPath-carrying plugInfo.json), then the stock OpenUSD plugin roots.
# In 26.08 plugins live under both $USD/plugin/usd and $USD/lib/usd.
_PXR=""
for _d in \
    "$GENBUILD/usd/usdGenSchema/resources" \
    "$GENBUILD/usd/usdGenImaging/resources" \
    "$GENBUILD/usd/usdGenShaders/resources" \
    "$GENBUILD/usd/usdGenTools/resources" \
    "$GENBUILD/usd/usdGenPomade/resources" \
    "$GENBUILD/usd/usdGenPomadeTools/resources" \
    "$GENBUILD/usd/usdNoodles/resources" \
    "$GENBUILD/python/usdgen" \
    "$USD/plugin/usd" \
    "$USD/lib/usd"
do
    if [ -d "$_d" ]; then
        _PXR="${_PXR:+$_PXR:}$_d"
    fi
done
export PXR_PLUGINPATH_NAME="$_PXR${PXR_PLUGINPATH_NAME:+:$PXR_PLUGINPATH_NAME}"

# Required for out-of-tree builds (plan/10 §3.6): the imaging delegate .so.
if [ -f "$GENBUILD/libusdGenImaging.so" ]; then
    export USDGEN_IMAGING_DLL="$GENBUILD/libusdGenImaging.so"
fi

usdgen_require_usd() {
    [ -x "$USD/bin/$1" ] || {
        echo "usdGen: OpenUSD tool '$1' not found under $USD/bin." >&2
        echo "set USD=/path/to/USD-install and re-source bin/_env.sh" >&2
        return 1
    }
}

usdgen_require_python() {
    [ -x "$PY" ] || { echo "usdGen: python3 not found at $PY; set VENV." >&2; return 1; }
}

unset _ENV_DIR _d _PXR PY_SITE
