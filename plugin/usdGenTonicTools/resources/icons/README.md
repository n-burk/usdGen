# Tonic icons

Naming: `<prefix>_<snake_case>.png` — see `manifest.txt` in this directory
for the generated-glyph name list and prompts, and the "Icon manifest"
table at the foot of the tonic backlog for the full name list (including
the usdRig-derived names below, which are not in `manifest.txt`), what
each icon is used for, and the glyph it should read as.

Prefixes: `mode_` (mode shelf), `sub_<mode>_` (sub-mode shelves),
`comp_` (tube component picks), `tool_` (Q/W/E/R manipulator tools),
`shape_` (selection shape), `orient_` (transform orientation),
`act_` (dock action buttons), `toggle_` (dock visibility toggles),
`status_` (status-strip pills and warning severities), `pivot_` (pivot
point mode).

Two families of source art live here side by side:

- Generated glyphs (`mode_*`, `sub_*`, `comp_*`, `shape_*`): 256x256 RGBA,
  pure white glyph, transparent background, 2-3 px stroke at 256 px, alpha
  bbox coverage 40-95% (the generator's own reject-and-regenerate target).
  Produced by `bin/gen_tonic_icons.sh` (added by IC-02; codex `gpt-6-sol`
  image generation + PIL post-process, run sequentially -- never in
  parallel, since concurrent codex runs share one
  `~/.codex/generated_images` output directory and pick up each other's
  images), parametrised by the `name|prompt` lines in `manifest.txt`. It
  finds this directory from its own location and needs the `codex` CLI on
  `PATH`; it stops with a message when codex is missing.
- usdRig manipulator/action art (`tool_select`, `tool_move`, `tool_rotate`,
  `tool_scale`, `act_undo`, `act_redo`, `act_settings`, `orient_world`,
  `orient_tube`, `pivot_centre`, `pivot_each`): 128x128 white line art,
  reused as-is so Tonic's manipulator toolbar reads as the same family as
  usdRig's. They were copied in from usdRig's `rigExecUsdview/icons`
  plugin directory (`select.png`, `move.png`, `rotate.png`, `scale.png`,
  `undo.png`, `redo.png`, `settings.png`, `global.png`, `local.png`,
  `pivot.png` and `groupCentre.png`/`groupEach.png`), normalised into this
  directory's naming and size by usdRig's `tools/bakeGizmoIcons.py`; the
  copies here are the source of truth for this repo. `tonicIcons.
  loadIcon` accepts either 128 or 256 px source art and tints/scales it
  the same way.

`testUsdGenTonicToolsIcons` (IC-02) is strict: every name in
`tonicModes.ICONS`/`tonicPanels.ACTION_ICONS` and every name in
`manifest.txt` must resolve to a PNG here, 128 or 256 px RGBA, alpha-bbox
coverage 15-97% (wider than the generator's 40-95% target so the thinner
usdRig line art still passes) -- no more `SKIP:` lines.
