#!/usr/bin/env python3
"""launch_usdview.ps1 MoonRay wiring check (docs/moonray-fur.md).

Runs bin/launch_usdview.ps1 -PrintEnv (headless JSON dump of the assembled
viewer environment) and asserts the MoonRay build's render-delegate plugins
come before the OpenUSD prefix on PXR_PLUGINPATH_NAME and its bin/ comes
before the prefix on PATH, so the working hd_moonray shadows the prefix's
unloadable one (first registration of a plugin name wins).

Skips (77) on non-Windows hosts, when powershell is missing, or when no
MoonRay build with valid delegate plugInfos exists beside the repo
(MOONRAY_BUILD override honored, as USD is for the OpenUSD prefix).
"""

import json
import os
import shutil
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_GEN = os.path.dirname(os.path.dirname(_HERE))  # <repo>/tests/checks -> <repo>

FAILURES = []


def fail(message):
    FAILURES.append(message)
    print("FAIL: %s" % message)


def norm(path):
    return os.path.normcase(os.path.normpath(path))


def main():
    if os.name != "nt":
        print("SKIP: launch_usdview.ps1 is a Windows launcher")
        return 77
    powershell = shutil.which("powershell") or shutil.which("pwsh")
    if not powershell:
        print("SKIP: no powershell interpreter")
        return 77
    launcher = os.path.join(_GEN, "bin", "launch_usdview.ps1")
    try:
        proc = subprocess.run(
            [powershell, "-NoProfile", "-NonInteractive",
             "-File", launcher, "-PrintEnv"],
            capture_output=True, text=True, timeout=180)
    except subprocess.TimeoutExpired:
        fail("launch_usdview.ps1 -PrintEnv timed out")
        return 1
    if proc.returncode != 0:
        fail("launch_usdview.ps1 -PrintEnv exited %d: %s" % (
            proc.returncode, (proc.stderr or "").strip()[:400]))
        return 1
    try:
        env = json.loads(proc.stdout)
    except ValueError:
        fail("unparseable -PrintEnv output: %r" % proc.stdout[:300])
        return 1

    parent = os.path.dirname(_GEN)
    moon_build = os.environ.get("MOONRAY_BUILD") or os.path.join(
        parent, "moonray", "build-windows")
    expected_dirs = []
    for leaf in ("hd_moonray", "hd_moonray_debug"):
        plugdir = os.path.join(
            moon_build, "moonray", "hydra", "hdMoonray", "plugin", leaf)
        dll = os.path.join(
            moon_build, "moonray", "hydra", "hdMoonray", leaf + ".dll")
        if (os.path.isfile(os.path.join(plugdir, "plugInfo.json"))
                and os.path.isfile(dll)):
            expected_dirs.append(plugdir)
    if not expected_dirs:
        print("SKIP: no MoonRay build with delegate plugInfos at %s"
              % moon_build)
        return 77
    if not env.get("MoonrayFound"):
        fail("launcher found no MoonRay plugins though %d valid delegate "
             "plugInfo(s) exist under %s" % (len(expected_dirs), moon_build))
        return 1

    prefix = os.environ.get("USD") or os.path.join(
        parent, "usdRig", "usd-install")
    plugin_entries = [norm(p) for p in
                      env.get("PXR_PLUGINPATH_NAME", "").split(";") if p]
    path_entries = [norm(p) for p in env.get("PATH", "").split(";") if p]

    prefix_plugins = norm(os.path.join(prefix, "plugin", "usd"))
    if prefix_plugins not in plugin_entries:
        fail("prefix plugin/usd missing from PXR_PLUGINPATH_NAME")
    else:
        cutoff = plugin_entries.index(prefix_plugins)
        for plugdir in expected_dirs:
            if norm(plugdir) not in plugin_entries:
                fail("MoonRay plugin dir missing from PXR_PLUGINPATH_NAME: "
                     "%s" % plugdir)
            elif plugin_entries.index(norm(plugdir)) > cutoff:
                fail("MoonRay plugin dir sorts after the prefix (shadowing "
                     "lost): %s" % plugdir)
    moon_bin = norm(os.path.join(moon_build, "bin"))
    if moon_bin not in path_entries:
        fail("MoonRay bin/ missing from PATH: %s" % moon_bin)
    else:
        for leaf in ("bin", "lib"):
            anchor = norm(os.path.join(prefix, leaf))
            if anchor in path_entries and \
                    path_entries.index(moon_bin) > path_entries.index(anchor):
                fail("MoonRay bin/ sorts after the prefix %s/ on PATH" % leaf)
    schema_resources = norm(os.path.join(
        _GEN, "build", "usd", "usdGenSchema", "resources"))
    if schema_resources not in plugin_entries:
        fail("usdGen build resources missing from PXR_PLUGINPATH_NAME")

    # The interactive default is Storm and must opt into OpenUSD's exact
    # --allow-async switch. The launcher reports its resolved invocation in
    # -PrintEnv so this remains a headless Windows check.
    launch_args = env.get("UsdviewArgs") or []
    if (env.get("Renderer") != "GL" or
            not env.get("AsyncSceneProcessing") or
            "--allow-async" not in launch_args):
        fail("default launcher invocation does not select Storm with "
             "--allow-async")

    # A MoonRay renderer must retain the complete-operation path. This uses
    # the same headless launcher route as the Storm assertion above.
    try:
        moon_proc = subprocess.run(
            [powershell, "-NoProfile", "-NonInteractive", "-File", launcher,
             "-PrintEnv", "--renderer=HdMoonrayRendererDebugPlugin"],
            capture_output=True, text=True, timeout=180)
        moon_env = json.loads(moon_proc.stdout) if moon_proc.returncode == 0 else {}
    except (subprocess.TimeoutExpired, ValueError):
        moon_env = {}
    moon_args = moon_env.get("UsdviewArgs") or []
    if (moon_env.get("Renderer") != "HdMoonrayRendererDebugPlugin" or
            moon_env.get("AsyncSceneProcessing") or
            "--allow-async" in moon_args):
        fail("MoonRay launcher invocation enables asynchronous scene processing")
    # hdMoonray renders through Arras even locally; without session
    # definitions selecting the renderer fails outright. The caller's
    # value wins, so equality is only asserted when the caller set
    # nothing (this process's own environment is what the launcher
    # subprocess inherits).
    sessions_disk = os.path.join(
        moon_build, "moonray", "hydra", "hdMoonray", "sessions")
    arras = env.get("ARRAS_SESSION_PATH") or ""
    if os.path.isdir(sessions_disk):
        if "ARRAS_SESSION_PATH" not in os.environ and \
                norm(arras) != norm(sessions_disk):
            fail("ARRAS_SESSION_PATH is %r, expected the MoonRay build "
                 "sessions dir: %s" % (arras, sessions_disk))
    elif not arras:
        fail("MoonRay build has no sessions dir and ARRAS_SESSION_PATH"
             " is unset; interactive MoonRay rendering cannot work")
    # Same shape for the RDL DSO set: staged rdl2dso wins, bin/
    # seconds, the caller's value wins over both.
    dso_disk = os.path.join(moon_build, "rdl2dso")
    if not os.path.isdir(dso_disk):
        dso_disk = os.path.join(moon_build, "bin")
        if not os.path.isdir(dso_disk):
            dso_disk = None
    dso = env.get("RDL2_DSO_PATH") or ""
    if dso_disk:
        if "RDL2_DSO_PATH" not in os.environ and \
                norm(dso) != norm(dso_disk):
            fail("RDL2_DSO_PATH is %r, expected the MoonRay DSO dir: "
                 "%s" % (dso, dso_disk))
    elif not dso:
        fail("MoonRay build has no DSO dir and RDL2_DSO_PATH is unset; "
             "hdMoonray cannot create RDL scene objects")

    if FAILURES:
        return 1
    print("check_usdview_env: PASS (%d MoonRay plugin dir(s))"
          % len(expected_dirs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
