#!/usr/bin/env python3
"""Bake real usdGen curves and render them with the local MoonRay delegate.

The stage compiler/scheduler in bake_groom.cpp generates the USD curves;
this driver only prepares the viewer environment, runs the baker and records
frames. Run from any directory after building usdGen and the docs baker.
"""

# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from PIL import Image, ImageStat

ROOT = Path(__file__).resolve().parents[2]
SCRATCH = ROOT / "renders" / "docs" / "pathtrace"
MEDIA = ROOT / "docs" / "site" / "media"
RENDERER = "HdMoonrayRendererDebugPlugin"


def viewer_environment() -> dict[str, str]:
    launcher = ROOT / "bin" / "launch_usdview.ps1"
    process = subprocess.run(
        ["powershell", "-NoProfile", "-File", str(launcher), "-PrintEnv",
         "--renderer", RENDERER], cwd=ROOT, text=True, capture_output=True,
        check=True,
    )
    settings = json.loads(process.stdout)
    if not settings["MoonrayFound"]:
        raise RuntimeError("MoonRay debug Hydra delegate is unavailable")
    env = os.environ.copy()
    for key in ("PATH", "PYTHONPATH", "PXR_PLUGINPATH_NAME", "ARRAS_SESSION_PATH",
                "RDL2_DSO_PATH"):
        value = settings.get(key)
        if value:
            env[key] = value
    env["USDGENPOMADE_TEST_TUBE"] = "0"
    return env


def baker_path() -> Path:
    candidates = [
        ROOT / "renders" / "docs" / "baker-build" / "Release" / "usdGenBakeGroom.exe",
        ROOT / "renders" / "docs" / "baker-build" / "usdGenBakeGroom.exe",
        ROOT / "renders" / "docs" / "baker-build" / "usdGenBakeGroom",
        ROOT / "build" / "usdGenBakeGroom.exe",
        ROOT / "build" / "usdGenBakeGroom",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise FileNotFoundError("Build tools/docs/CMakeLists.txt to get usdGenBakeGroom")


def run_logged(command: list[str], log: Path, env: dict[str, str]) -> None:
    with log.open("w", encoding="utf-8") as stream:
        completed = subprocess.run(command, cwd=ROOT, env=env, stdout=stream,
                                   stderr=subprocess.STDOUT)
    if completed.returncode:
        lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
        raise RuntimeError(f"{Path(command[0]).name} failed ({completed.returncode}): "
                           + "\n".join(lines[-14:]))


def bake(scene: Path, descriptions: list[str], material: str | None,
         target: Path, env: dict[str, str], time_code: float | None = None) -> None:
    previous = scene.resolve()
    for index, description in enumerate(descriptions):
        output = target if index == len(descriptions) - 1 else target.with_name(
            f"{target.stem}-{index}.usdc")
        command = [str(baker_path()), str(previous), description, str(output)]
        if material:
            command.append(material)
        if time_code is not None:
            if not material:
                command.append("-")
            command.append(str(time_code))
        run_logged(command, output.with_suffix(".bake.log"), env)
        print(f"baked {description}: {output}", flush=True)
        previous = output


def render(stage: Path, camera: str, target: Path, width: int,
           env: dict[str, str], time_code: float | None = None) -> None:
    recorder_name = shutil.which("usdrecord", path=env.get("PATH"))
    if not recorder_name:
        raise FileNotFoundError("usdrecord is unavailable; set USD for the viewer launcher")
    recorder = Path(recorder_name)
    # On Windows shutil.which can prefer usdrecord.CMD, while the executable
    # content we pass to Python is the adjacent extensionless script.
    if recorder.suffix.lower() == ".cmd" and recorder.with_suffix("").exists():
        recorder = recorder.with_suffix("")
    command = [sys.executable, str(recorder), "--renderer", RENDERER,
               "--imageWidth", str(width), "--disableCameraLight", "--camera",
               camera]
    if time_code is None:
        output_arg = target
        command.append("--defaultTime")
    else:
        # usdrecord requires a frame-number placeholder with --frames. The
        # named output is normalized back to target for docs media consumers.
        output_arg = target.with_name(f"{target.stem}-frame-###.###{target.suffix}")
        command.extend(["--frames", str(time_code)])
    command.extend([str(stage), str(output_arg)])
    run_logged(command, target.with_suffix(".render.log"), env)
    if time_code is not None:
        recorded = target.with_name(
            f"{target.stem}-frame-{time_code:07.3f}{target.suffix}")
        if not recorded.exists():
            raise RuntimeError(f"usdrecord did not write frame {time_code}: {recorded}")
        recorded.replace(target)
    if not target.exists() or target.stat().st_size < 1024:
        raise RuntimeError(f"Renderer wrote no image: {target}")
    with Image.open(target) as image:
        rgb = image.convert("RGB")
        sample = rgb.resize((min(256, rgb.width), min(256, rgb.height)))
        count = sample.width * sample.height
        error_pixels = sum(1 for r, g, b in sample.getdata()
                           if r > 180 and b > 180 and g < 95)
        if error_pixels / count > 0.005:
            raise RuntimeError(f"MoonRay error material visible in {target} "
                               f"({error_pixels / count:.1%} magenta pixels)")
        if max(ImageStat.Stat(sample).stddev) < 5.0:
            raise RuntimeError(f"Nearly flat renderer output: {target}")
    print(f"rendered {target}", flush=True)


def one(scene: Path, descriptions: list[str], material: str | None,
        camera: str, output: Path, width: int, env: dict[str, str],
        time_code: float | None = None, studio_special: bool = False) -> None:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    baked = SCRATCH / f"{scene.stem}-baked.usdc"
    temporary = SCRATCH / f"{scene.stem}-moonray.png"
    bake(scene, descriptions, material, baked, env, time_code)
    render_stage = baked
    if studio_special:
        render_stage = SCRATCH / f"{scene.stem}-studio.usdc"
        run_logged([sys.executable, str(ROOT / "tools" / "docs" / "operator_studio.py"),
                    str(baked), str(render_stage)],
                   render_stage.with_suffix(".studio.log"), env)
    render(render_stage, camera, temporary, width, env, time_code)
    shutil.copyfile(temporary, output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene", nargs="?", type=Path)
    parser.add_argument("--description", action="append", default=[])
    parser.add_argument("--material")
    parser.add_argument("--camera", default="/World/Cam")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--width", type=int, default=1200)
    parser.add_argument("--time", type=float,
                        help="time code sampled by the usdGen cook (for example, Deform frame 24)")
    parser.add_argument("--studio-special", action="store_true",
                        help="subdue guide-example lights and add neutral ground")
    parser.add_argument("--batch-operators", action="store_true")
    parser.add_argument("--names", nargs="*", help="limit operator batch to these stems")
    args = parser.parse_args()
    if args.width < 100:
        parser.error("--width must be at least 100")
    env = viewer_environment()
    if args.batch_operators:
        source = ROOT / "examples" / "docs" / "operators"
        # Part, Collide and Wind use tailored diagnostic comparison fixtures.
        # Their generic patch views would erase those captures when
        # regenerating the manual media.
        stages = [path for path in sorted(source.glob("*.usda"))
                  if path.stem not in {"part", "collide", "wind"}]
        if args.names:
            names = set(args.names)
            stages = [path for path in stages if path.stem in names]
        if not stages:
            parser.error("no operator stages selected")
        failures = []
        for index, stage in enumerate(stages, 1):
            print(f"[{index}/{len(stages)}] {stage.stem}", flush=True)
            try:
                one(stage, ["/World/Groom/Fur"], "/World/Looks/Fur",
                    "/World/Cam", MEDIA / f"{stage.stem}.png", args.width, env)
            except Exception as exc:
                failures.append((stage.stem, str(exc)))
                print(f"FAILED {stage.stem}: {exc}", file=sys.stderr, flush=True)
        if failures:
            print(f"{len(failures)} operator render(s) failed", file=sys.stderr)
            return 1
        return 0
    if not args.scene or not args.description or not args.output:
        parser.error("scene, --description and --output are required")
    one(args.scene, args.description, args.material, args.camera, args.output,
        args.width, env, args.time, args.studio_special)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
