# M0 exit gate evidence — usdcat / extent on `UsdGenDescription`

Gate (`plan/11-roadmap.md` §2.1): *"usdcat --flatten on the M0 fixture resolves
the codeless types and a non-empty extent is obtained on `UsdGenDescription`."*

This page is the verbatim, reproducible record of that gate, refreshed on the
current build tree. It is structured to answer the two things a flattened
`usdcat` on its own **cannot** prove:

1. **Type resolution (SOL C-11).** Flattening prints the *authored* type-name
   tokens `def UsdGenGroom` / `def UsdGenDescription` even when the schema
   plugin is **not** loaded — an unknown authored type still flattens to its
   literal name. So token presence is not proof the codeless schemas
   registered. The proof is a real type-system query:
   `TfType::FindByName("UsdGenGroom"/"UsdGenDescription")` returns a
   non-`Unknown` type **and** `UsdSchemaRegistry::IsConcrete(name)` is true.
   We assert this in the probe, with a negative control (no plugin dir on
   `PXR_PLUGINPATH_NAME`) that reads `known=0 / concrete=0`, and a positive
   control (schema dir on the path) that reads `known=1 / concrete=1`.
2. **dlopen (SOL C-12).** `TF_DEBUG` distinguishes *registration*
   (`Registering shared library plugin …`, emitted when `plug` reads the
   `plugInfo.json` metadata) from *loading* (`Loading plugin 'usdGenSchema'.`,
   emitted when `plug` actually dlopens the `.so`). `usdcat --flatten` only
   **registers** the plugin — it never loads it, because flattening does not
   request an extent. The probe **does** request an extent, and its
   `TF_DEBUG=PLUG_LOAD` log shows the load. That is the faithful stand-in for
   the gate.

`usdcat` itself prints no extent, so the gate is demonstrated as: `usdcat
--flatten` smoke (types flatten) **plus** the probe (types are real registered
concrete types, and a `ComputeExtent` call dlopens `libusdGenSchema.so` and
returns a non-empty 2-point extent).

## Environment (headless aarch64 Linux, g++ 13.3, C++17)

All runs use a clean environment (`env -i`) with exactly these exports, so the
record is copy-paste reproducible. `TF_DEBUG` is **space-separated** in
OpenUSD — a comma list like `PLUG_LOAD,PLUG_REGISTRATION` matches nothing.

```sh
export PXR=/home/burkard/work/OpenUSD_26_08
export REPO=/home/burkard/work/usdGen
export FIX="$REPO/tests/scenes/scene_empty_groom.usda"
export PROBE="$REPO/docs/prework/probes/m0-usdcat-extent/probe_usdcat_extent"
export LD_LIBRARY_PATH="$PXR/lib"
export NEG_PXR_PLUGINPATH_NAME="$PXR/plugin/usd"
export POS_PXR_PLUGINPATH_NAME="$REPO/build/usd/usdGenSchema/resources:$PXR/plugin/usd"
```

## Probe build

The probe links **stock OpenUSD only** (usd / usdGeom + their deps) and **never
links `libusdGenSchema`** — the extent can only come from `Plug` dlopen'ing the
build-tree schema library plugin (`Type:library`, `implementsComputeExtent:true`,
`LibraryPath:../../../libusdGenSchema.so`), which is exactly the chain the gate
is meant to prove.

One compile fix was made to `probe_usdcat_extent.cpp` (only this file was
touched): in 26.08 `UsdStage::Open` has no `SdfAssetPath` overload, so

```cpp
UsdStage::Open(SdfAssetPath(argv[1]))   // before
UsdStage::Open(std::string(argv[1]))    // after
```

Compile command (run from `$REPO/docs/prework/probes/m0-usdcat-extent/`):

```sh
g++ -std=c++17 -O1 -Wno-cpp \
    -I /home/burkard/work/OpenUSD_26_08/include \
    -I /usr/include/python3.12 \
    probe_usdcat_extent.cpp -o probe_usdcat_extent \
    -L /home/burkard/work/OpenUSD_26_08/lib \
    -lusd_usd -lusd_usdGeom \
    -lusd_python -lpython3.12 \
    -lusd_tf -lusd_sdf -lusd_gf -lusd_arch -lusd_kind -lusd_vt \
    -lusd_trace -lusd_work -lusd_js -lusd_plug -lusd_ar -lusd_hio \
    -Wl,-rpath,/home/burkard/work/OpenUSD_26_08/lib
```

Note: `-lusd_python -lpython3.12` and `-I /usr/include/python3.12` are required
only because `usd/stage.h` → `ar/resolverContext.h` → `tf/pyLock.h` pulls in
the boost-python shim headers in this build (the link fails with undefined
`pxr_boost::python::converter::registry::lookup` without them); no Python is
used at runtime.

## Run 0 — `ldd` proves the probe has no `libusdGen*` dependency (SOL C-13)

```sh
ldd "$PROBE"
```

Verbatim output (addresses vary per run; the dependency set does not):

```
	linux-vdso.so.1 (0x0000f6ddc748c000)
	libusd_usd.so => /home/burkard/work/OpenUSD_26_08/lib/libusd_usd.so
	libusd_usdGeom.so => /home/burkard/work/OpenUSD_26_08/lib/libusd_usdGeom.so
	libusd_python.so => /home/burkard/work/OpenUSD_26_08/lib/libusd_python.so
	libusd_tf.so => /home/burkard/work/OpenUSD_26_08/lib/libusd_tf.so
	libusd_sdf.so => /home/burkard/work/OpenUSD_26_08/lib/libusd_sdf.so
	libusd_vt.so => /home/burkard/work/OpenUSD_26_08/lib/libusd_vt.so
	libstdc++.so.6 => /lib/aarch64-linux-gnu/libstdc++.so.6
	libgcc_s.so.1 => /lib/aarch64-linux-gnu/libgcc_s.so.1
	libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6
	/lib/ld-linux-aarch64.so.1 (0x0000f6ddc7458000)
	libusd_kind.so => /home/burkard/work/OpenUSD_26_08/lib/libusd_kind.so
	libusd_pcp.so => /home/burkard/work/OpenUSD_26_08/lib/libusd_pcp.so
	libusd_ar.so => /home/burkard/work/OpenUSD_26_08/lib/libusd_ar.so
	libusd_plug.so => /home/burkard/work/OpenUSD_26_08/lib/libusd_plug.so
	libusd_ts.so => /home/burkard/work/OpenUSD_26_08/lib/libusd_ts.so
	libusd_work.so => /home/burkard/work/OpenUSD_26_08/lib/libusd_work.so
	libusd_gf.so => /home/burkard/work/OpenUSD_26_08/lib/libusd_gf.so
	libusd_trace.so => /home/burkard/work/OpenUSD_26_08/lib/libusd_trace.so
	libusd_js.so => /home/burkard/work/OpenUSD_26_08/lib/libusd_js.so
	libusd_arch.so => /home/burkard/work/OpenUSD_26_08/lib/libusd_arch.so
	libtbb.so.2 => /home/burkard/work/OpenUSD_26_08/lib/libtbb.so.2
	libpython3.12.so.1.0 => /lib/aarch64-linux-gnu/libpython3.12.so.1.0
	libm.so.6 => /lib/aarch64-linux-gnu/libm.so.6
	libz.so.1 => /lib/aarch64-linux-gnu/libz.so.1
	libexpat.so.1 => /lib/aarch64-linux-gnu/libexpat.so.1
```

```sh
grep -ci usdgen <(ldd "$PROBE")
0
```

Only stock `libusd_*` / `libtbb` / system / `libpython3.12` — **zero**
`libusdGen*` entries. Any extent returned below must therefore come from a
runtime dlopen of the schema plugin, not a link-time dependency.

## Run 1 — negative control: probe, no plugin dir (expect: types unknown, no extent, exit 1)

```sh
env -i PATH="$PATH" LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
  PXR_PLUGINPATH_NAME="$NEG_PXR_PLUGINPATH_NAME" \
  "$PROBE" "$FIX"; echo "exit=$?"
```

Verbatim output:

```
Coding Error: in _ComputeExtentFromPlugins at line 227 of /home/burkard/work/OpenUSD/pxr/usd/usdGeom/boundableComputeExtent.cpp -- Invalid UsdGeomBoundable 'UsdGenDescription' prim </Groom/Description> on stage with rootLayer @/home/burkard/work/usdGen/tests/scenes/scene_empty_groom.usda@, sessionLayer @anon:0xae8f7828b410:scene_empty_groom-session.usda@
prim type = UsdGenDescription
type-check UsdGenGroom          TfType::FindByName known=0  UsdSchemaRegistry::IsConcrete=0
type-check UsdGenDescription    TfType::FindByName known=0  UsdSchemaRegistry::IsConcrete=0
ComputeExtent ok=0 size=0
FAIL: no extent returned (plugin did not fire?)
exit=1
```

The fixture's authored `def UsdGenGroom` / `def UsdGenDescription` tokens are
still present in the layer (note the `Coding Error` names the prim's type
`UsdGenDescription` straight from the file), yet with no plugin dir on
`PXR_PLUGINPATH_NAME` both types are **unknown** and **non-concrete**, and no
extent can be computed. This is the control that makes Run 2 meaningful.
(The session address in the `Coding Error` line is per-process.)

## Run 2 — positive control: probe, schema plugin dir on path (expect: types known+concrete, non-empty extent, exit 0)

```sh
env -i PATH="$PATH" LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
  PXR_PLUGINPATH_NAME="$POS_PXR_PLUGINPATH_NAME" \
  "$PROBE" "$FIX"; echo "exit=$?"
```

Verbatim output:

```
prim type = UsdGenDescription
type-check UsdGenGroom          TfType::FindByName known=1  UsdSchemaRegistry::IsConcrete=1
type-check UsdGenDescription    TfType::FindByName known=1  UsdSchemaRegistry::IsConcrete=1
ComputeExtent ok=1 size=2
extent = [(-0.500 -0.500 -0.500) .. (0.500 0.500 0.500)] empty=0
PASS: non-empty extent + codeless types registered & concrete
exit=0
```

With the schema plugin dir on the path, both codeless types are registered
concrete types (`UsdGenGroom` bases `UsdGeomImageable`, `UsdGenDescription`
bases `UsdGeomBoundable`), and the extent is the deterministic 1×1×1
unit-centred box that `usdGen::_ComputeDescriptionExtent`
(`libs/usdGenSchema/usdGenSchema.cpp`) emits when a `UsdGenDescription` has
no boundable descendants — the M0 fixture's description has none (its
`usdGen:surface` relation points to the mesh, which is not a child prim).

## Run 3 — `usdcat --flatten` smoke (expect: types flatten to real names, exit 0)

```sh
env -i PATH="$PATH" LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
  PXR_PLUGINPATH_NAME="$POS_PXR_PLUGINPATH_NAME" \
  "$PXR/bin/usdcat" --flatten "$FIX"; echo "exit=$?"
```

Verbatim output:

```
#usda 1.0
(
    defaultPrim = "Groom"
    doc = """Generated from Composed Stage of root layer /home/burkard/work/usdGen/tests/scenes/scene_empty_groom.usda
"""
    metersPerUnit = 1
    upAxis = "Y"
)

def UsdGenGroom "Groom"
{
    int usdGen:schemaVersion = 1
    uniform token usdGen:sessionId = "default"

    def UsdGeomMesh "Surface"
    {
        int[] faceVertexCounts = [3, 3, 3, 3]
        int[] faceVertexIndices = [0, 1, 2, 2, 3, 0, 0, 3, 1, 1, 3, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
        uniform token subdivisionScheme = "none"
    }

    def UsdGenDescription "Description"
    {
        rel usdGen:surface = </Groom/Surface>
        uniform int usdGen:tileTarget = 64
    }
}
exit=0
```

```
grep -n "def UsdGen" <(env -i PATH="$PATH" LD_LIBRARY_PATH="$LD_LIBRARY_PATH" PXR_PLUGINPATH_NAME="$POS_PXR_PLUGINPATH_NAME" "$PXR/bin/usdcat" --flatten "$FIX")
10:def UsdGenGroom "Groom"
23:    def UsdGenDescription "Description"
```

Both codeless types flatten to their declared type names (schema metadata
intact), not to anonymous defs. Note this run alone is **not** proof the
schemas registered (Run 1 shows tokens flatten even when unknown) — it is the
flattening smoke that the gate names.

## Run 4 — dlopen proof via `TF_DEBUG` (expect: probe *loads* the plugin, usdcat only *registers* it)

`TF_DEBUG` messages go to **stdout** by default (`TF_DEBUG_OUTPUT_FILE=stderr`
switches them). Symbol lists are space-separated.

### 4a — probe with `TF_DEBUG="PLUG_LOAD PLUG_REGISTRATION"` (expect a `Loading` line)

```sh
env -i PATH="$PATH" LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
  PXR_PLUGINPATH_NAME="$POS_PXR_PLUGINPATH_NAME" \
  TF_DEBUG="PLUG_LOAD PLUG_REGISTRATION" \
  "$PROBE" "$FIX"; echo "exit=$?"
```

The log is 116 lines: 53 stock `Registering …` metadata lines, then the
probe's own output. The load-relevant lines, verbatim, with line numbers in
the combined log:

```
1:    Registering shared library plugin 'usdGenSchema' at '/home/burkard/work/usdGen/build/libusdGenSchema.so'.
109:  Loading plugin 'sdf'.
110:  prim type = UsdGenDescription
111:  type-check UsdGenGroom          TfType::FindByName known=1  UsdSchemaRegistry::IsConcrete=1
112:  type-check UsdGenDescription    TfType::FindByName known=1  UsdSchemaRegistry::IsConcrete=1
113:  Loading plugin 'usdGenSchema'.
114:  ComputeExtent ok=1 size=2
115:  extent = [(-0.500 -0.500 -0.500) .. (0.500 0.500 0.500)] empty=0
116:  PASS: non-empty extent + codeless types registered & concrete
exit=0
```

Line 1 is the **registration** (metadata read at discovery; the plugin is not
yet dlopen'd). Line 113 is the **load**: `plug` dlopens
`libusdGenSchema.so` immediately before the extent succeeds, because
`ComputeExtent` resolves the registered callback from the library
(`pxr/base/plug/plugin.cpp`). Combined with Run 0 (`ldd` shows the probe does
not link `libusdGenSchema` statically), this is the direct proof that the
extent path fired through a runtime dlopen of the schema plugin.

### 4b — `usdcat --flatten` with the same `TF_DEBUG` (expect **no** load of usdGenSchema)

```sh
env -i PATH="$PATH" LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
  PXR_PLUGINPATH_NAME="$POS_PXR_PLUGINPATH_NAME" \
  TF_DEBUG="PLUG_LOAD PLUG_REGISTRATION" \
  "$PXR/bin/usdcat" --flatten "$FIX"; echo "exit=$?"
```

The log (138 lines) contains the same `Registering shared library plugin
'usdGenSchema' at '/home/burkard/work/usdGen/build/libusdGenSchema.so'.` at
line 1, but the only `Loading plugin …` line in the whole log is
`Loading plugin 'sdf'.` — there is **no** `Loading plugin 'usdGenSchema'`.

`usdcat --flatten` **registers** `usdGenSchema` (reads its `plugInfo.json`)
but **never loads** it: flattening does not request an extent, so there is no
reason to dlopen the library. This confirms the SOL C-12 point — a
`Registering …` line, and even a successful flatten, do **not** by themselves
prove the schema library was loaded. The load happens only in Run 4a, when a
`ComputeExtent` call forces `plug` to resolve the registered callback.

## Result

| Run | Purpose | Expected | Observed | Exit |
|-----|---------|----------|----------|------|
| 0 | probe is stock-only | no `libusdGen*` in `ldd` | `grep -ci usdgen` → 0 | 0 |
| 1 | negative control | types unknown, no extent | `known=0/IsConcrete=0` ×2, `ok=0` | 1 |
| 2 | positive control | types known+concrete, extent | `known=1/IsConcrete=1` ×2, 2-pt extent, PASS | 0 |
| 3 | usdcat smoke | types flatten to real names | `def UsdGenGroom`, `def UsdGenDescription` | 0 |
| 4a | probe dlopen proof | `Loading plugin 'usdGenSchema'` | present, immediately before extent | 0 |
| 4b | usdcat dlopen contrast | registers, does not load usdGenSchema | `Registering …` yes, `Loading …` only `sdf` | 0 |

GATE usdcat-extent: **PASS** — the codeless types resolve to registered,
concrete types only when the schema plugin dir is on `PXR_PLUGINPATH_NAME`
(Runs 1 vs 2), and a `ComputeExtent` on `UsdGenDescription` dlopens
`libusdGenSchema.so` (Run 4a, `ldd` Run 0) and returns a non-empty 2-point
extent. `usdcat --flatten` alone (Runs 3/4b) flattens the type names but does
not itself load the schema library, which is why the probe is the load-bearing
half of the gate.
