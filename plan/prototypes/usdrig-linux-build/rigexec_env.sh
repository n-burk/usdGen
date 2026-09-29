# Source this to point a shell at an out-of-tree rig build.
# Set USD, RIG, and RIGBUILD first. VENV is optional.
#   USD=/path/to/OpenUSD RIG=/path/to/rig RIGBUILD=/path/to/rig-build . rigexec_env.sh
: "${USD:?Set USD to the OpenUSD install prefix}"
: "${RIG:?Set RIG to the rig project root}"
: "${RIGBUILD:?Set RIGBUILD to the rig build directory}"
export PY_SITE="${PY_SITE:-$USD/lib/python3.12/site-packages}"

export PATH="$RIGBUILD:$USD/bin${VENV:+:$VENV/bin}:$PATH"
export LD_LIBRARY_PATH="$USD/lib:$RIGBUILD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONPATH="$RIGBUILD/python:$RIG/plugin/rigExecUsdview:$RIG/plugin/museAssistant:$PY_SITE${PYTHONPATH:+:$PYTHONPATH}"
export PXR_PLUGINPATH_NAME="$RIGBUILD/usd/rigExecSchema/resources:$RIGBUILD/usd/rigExecImaging/resources:$RIG/plugin/rigExecUsdview${PXR_PLUGINPATH_NAME:+:$PXR_PLUGINPATH_NAME}"
