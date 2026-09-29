# Showcase examples

Compelling demo scenes for talks and first-look renders. CPU lane unless noted.
Pomade scenes use **Pomade** schemas and `usdGen:pomade:scaleFactor` only
(never Tonic / MayaScaleFactor).

| Scene | What it shows |
|---|---|
| [`pomade-plait-braid.usda`](pomade-plait-braid.usda) | Classic three-strand **Pomade plait**: tubes + woven guides + GuideInterpolate amplification |
| [`scatter-grow-styled.usda`](scatter-grow-styled.usda) | Dense Scatter → Grow → Noise → Length → Width groom on a unit plane |

Also see the main examples tree for the committer braid hierarchy
(`../pomade-braid-hierarchy.usda`) and ponytail (`../pomade-ponytail.usda`).

## View

```sh
source bin/_env.sh
"$PY" "$USD/bin/usdview" examples/showcase/pomade-plait-braid.usda
"$PY" "$USD/bin/usdview" examples/showcase/scatter-grow-styled.usda
```

`bin/_env.sh` keeps an `USD` you already exported. Point it at an OpenUSD 26.08 prefix.

## Record (Storm / GL)

Needs an OpenUSD build with imaging and GL (`usdrecord` plus Storm). If GL
is missing, the checked-in USDA is still the review artifact.

```sh
source bin/_env.sh
mkdir -p renders/showcase
usdrecord --renderer Storm --camera /World/Cam \
    examples/showcase/pomade-plait-braid.usda \
    renders/showcase/pomade-plait-braid.png
usdrecord --renderer Storm --camera /World/Cam \
    examples/showcase/scatter-grow-styled.usda \
    renders/showcase/scatter-grow-styled.png
```

## Regenerate

```sh
python examples/tools/gen_showcase_plait_braid.py
```

The plait USDA is also checked in so CI and offline review do not need a
live Pomade DLL. Interactive Pomade authoring is covered in
[`docs/pomade-tool.md`](../../docs/pomade-tool.md).
