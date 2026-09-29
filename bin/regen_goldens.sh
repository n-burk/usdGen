#!/usr/bin/env bash
# regen_goldens.sh — regenerate T2 render goldens (plan/10 §5.4).
#
# Refuses to run on a dirty worktree: goldens are only valid as a function of
# committed code (the sidecar records the HEAD sha). Renders the golden-sized
# frame via the real test in USDGEN_REGEN_GOLDEN=1 mode and records a metadata
# sidecar (host, GL string, sha) next to the PNG.
set -euo pipefail
export USDGEN_REGEN_GOLDEN=1

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
    echo "regen_goldens.sh: worktree is dirty — commit first (goldens must" \
         "correspond to committed code, plan/10 §5.4)." >&2
    exit 1
fi

# shellcheck disable=SC1091
set -a; . bin/_env.sh; set +a

mkdir -p tests/golden
ctest --test-dir build -R '^testUsdGenStormLook$' --output-on-failure >/tmp/usdgen_golden_run.log 2>&1 || true
grep -q 'GOLDEN WRITTEN' /tmp/usdgen_golden_run.log || {
    echo "regen_goldens.sh: test did not report GOLDEN WRITTEN — see" \
         "/tmp/usdgen_golden_run.log" >&2
    exit 1
}

python3 - "$ROOT" <<'PY'
import hashlib, json, os, subprocess, sys, datetime
root = sys.argv[1]
png = os.path.join(root, "tests/golden/stormLook_A.png")
sha = hashlib.sha256(open(png, "rb").read()).hexdigest()
log = open("/tmp/usdgen_golden_run.log").read()
def grab(key):
    for line in log.splitlines():
        if line.startswith(key + "="):
            return line.split("=", 1)[1].strip()
    return None
side = png + ".json"
json.dump({
    "golden": "stormLook_A.png",
    "git_sha": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
    "host": "redacted",
    "gpu": grab("GL_RENDERER") or "unknown",
    "gl_version": grab("GL_VERSION"),
    "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "command": "bin/regen_goldens.sh (testUsdGenStormLook, complexity 1.2, 256x256)",
    "tolerance": {"mean_abs_max": 2/255, "frac_gt_8_255_max": 0.005},
    "png_sha256": sha,
}, open(side, "w"), indent=2)
print("wrote", side)
PY
echo "goldens regenerated from $(git rev-parse --short HEAD)"
