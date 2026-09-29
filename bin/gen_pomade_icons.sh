#!/bin/bash
# bin/gen_pomade_icons.sh -- generate Pomade dock/shelf glyph PNGs with codex
# image generation (IC-02).
#
# Needs the `codex` CLI on PATH with image generation enabled for the
# account. It is parametrised by a manifest file of "name|prompt" lines and
# generates SEQUENTIALLY, one codex run at a time. Running several
# codex processes in parallel is a real bug we hit: every run reads
# ~/.codex/generated_images (a directory shared by the whole account, not
# per-run), so concurrent runs each pick up whichever PNG landed there
# most recently -- including another run's image -- and silently produce
# duplicate or mismatched glyphs. Do not add "&"/xargs -P/wait batching
# back into this script.
#
# usage: gen_pomade_icons.sh [manifest_file] [icons_out_dir]
#   manifest_file  "name|prompt" lines, one glyph per line, '#' comments
#                  and blank lines skipped. Default (relative to the repo
#                  root, found from this script's own location):
#                  plugin/usdGenPomadeTools/resources/icons/manifest.txt
#   icons_out_dir  where the finished PNGs are written. Default:
#                  plugin/usdGenPomadeTools/resources/icons
#
# A name whose PNG already exists in icons_out_dir is skipped -- rerun
# safely to fill in only the names still missing (e.g. after adding rows
# to the manifest), or delete a PNG first to force a regenerate.
set -euo pipefail

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SELF_DIR/.." && pwd)"

MANIFEST="${1:-$REPO_ROOT/plugin/usdGenPomadeTools/resources/icons/manifest.txt}"
OUT_DIR="${2:-$REPO_ROOT/plugin/usdGenPomadeTools/resources/icons}"

if ! command -v codex >/dev/null 2>&1; then
  echo "gen_pomade_icons: the codex CLI is not on PATH; install it (with image generation enabled) to regenerate icons" >&2
  exit 2
fi
if [ ! -f "$MANIFEST" ]; then
  echo "gen_pomade_icons: no manifest at $MANIFEST" >&2
  exit 2
fi

WORK_ROOT="$(mktemp -d)"

STYLE="flat, single solid white glyph on a fully transparent background, no border, no shadow, no gradient, no text, centred, bold simple strokes readable at 24 px, in the style of Maya/Blender toolbar icons"

mkdir -p "$OUT_DIR"

gen_one() {
  local name="$1" desc="$2"
  local work="$WORK_ROOT/work_$name"
  mkdir -p "$work"
  (
    cd "$work"
    codex exec --skip-git-repo-check -m gpt-6-sol -s danger-full-access --enable image_generation \
      "Use your image generation tool to generate ONE icon: $desc, line art glyph. Style: $STYLE. Then post-process with Python PIL: open the generated PNG (it lands under ~/.codex/generated_images/), convert to RGBA, make every non-transparent pixel pure white keeping its alpha, crop to the alpha bounding box with 8% padding, resize to 256x256 with LANCZOS on a transparent canvas preserving aspect, and save it in the cwd as $name.png. Print the final alpha bounding box coverage fraction." < /dev/null > log.txt 2>&1
  )
  if [ -f "$work/$name.png" ]; then
    cp "$work/$name.png" "$OUT_DIR/$name.png"
    echo "generated: $name"
  else
    echo "FAILED (no PNG, see $work/log.txt): $name" >&2
  fi
}

count=0
skipped=0
failed=0
while IFS='|' read -r name desc; do
  case "$name" in
    ""|"#"*) continue ;;
  esac
  if [ -f "$OUT_DIR/$name.png" ]; then
    skipped=$((skipped + 1))
    continue
  fi
  count=$((count + 1))
  if ! gen_one "$name" "$desc"; then
    failed=$((failed + 1))
    continue
  fi
  if [ ! -f "$OUT_DIR/$name.png" ]; then
    failed=$((failed + 1))
  fi
done < "$MANIFEST"

echo "gen_pomade_icons: generated $count, skipped $skipped (already present), failed $failed"
[ "$failed" -eq 0 ]
