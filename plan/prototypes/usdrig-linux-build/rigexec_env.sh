# Source this to use the out-of-tree RigExec build on this machine.
#   . /tmp/claude-1000/-home-burkard-work-usdRig/887eb74a-2f4d-45ff-88d7-6c9ab67fd9a7/scratchpad/probes/B-build/rigexec_env.sh
export USD=/home/burkard/work/OpenUSD_26_08
export RIG=/home/burkard/work/usdRig
export RIGBUILD=/tmp/claude-1000/-home-burkard-work-usdRig/887eb74a-2f4d-45ff-88d7-6c9ab67fd9a7/scratchpad/usdRigBuild
export PY_SITE="$USD/lib/python3.12/site-packages"

export PATH="$RIGBUILD:$USD/bin:/home/burkard/.venv/bin:$PATH"
export LD_LIBRARY_PATH="$USD/lib:$RIGBUILD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONPATH="$RIGBUILD/python:$RIG/plugin/rigExecUsdview:$RIG/plugin/museAssistant:$PY_SITE${PYTHONPATH:+:$PYTHONPATH}"
export PXR_PLUGINPATH_NAME="$RIGBUILD/usd/rigExecSchema/resources:$RIGBUILD/usd/rigExecImaging/resources:$RIG/plugin/rigExecUsdview${PXR_PLUGINPATH_NAME:+:$PXR_PLUGINPATH_NAME}"
