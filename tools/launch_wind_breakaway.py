#!/usr/bin/env python3
"""Start one guarded Wind pair render outside the caller's Windows job."""

# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
SCRATCH = ROOT / "renders" / "docs" / "wind-variants"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", choices=("clumped", "unclumped", "both"),
                        required=True)
    parser.add_argument("--baker", type=Path, required=True)
    parser.add_argument("--runtime-dir", type=Path, required=True)
    parser.add_argument("--pin-viewer-runtime", action="store_true")
    args = parser.parse_args()
    baker = args.baker.resolve(strict=True)
    runtime = args.runtime_dir.resolve(strict=True)
    SCRATCH.mkdir(parents=True, exist_ok=True)
    command = [sys.executable, str(ROOT / "tools" / "render_wind_variants.py"),
               "--variant", args.variant, "--reuse-bakes", "--width", "800",
               "--baker", str(baker), "--runtime-dir", str(runtime)]
    if args.pin_viewer_runtime:
        command.append("--pin-viewer-runtime")
    stdout_path = SCRATCH / f"wind-{args.variant}-job.stdout.log"
    stderr_path = SCRATCH / f"wind-{args.variant}-job.stderr.log"
    with (stdout_path.open("w", encoding="utf-8") as stdout,
          stderr_path.open("w", encoding="utf-8") as stderr):
        child = subprocess.Popen(
            command, cwd=ROOT, stdin=subprocess.DEVNULL,
            stdout=stdout, stderr=stderr, close_fds=True,
            creationflags=(subprocess.CREATE_BREAKAWAY_FROM_JOB |
                           subprocess.CREATE_NO_WINDOW),
        )
    record = {
        "variant": args.variant,
        "pid": child.pid,
        "startedUtc": datetime.now(timezone.utc).isoformat(),
        "command": command,
        "baker": str(baker),
        "runtimeDir": str(runtime),
        "pinViewerRuntime": args.pin_viewer_runtime,
        "stdout": str(stdout_path),
        "stderr": str(stderr_path),
        "checkpoints": [str(SCRATCH / f"wind-{variant}-state.json") for variant in
                        (("clumped", "unclumped") if args.variant == "both"
                         else (args.variant,))],
    }
    target = SCRATCH / f"wind-{args.variant}-launch.json"
    temporary = target.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    temporary.replace(target)
    print(f"Launched Wind {args.variant} as breakaway PID {child.pid}; "
          f"checkpoint: {record['checkpoints']}")


if __name__ == "__main__":
    main()
