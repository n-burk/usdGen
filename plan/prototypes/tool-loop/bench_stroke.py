"""One brush stroke, end to end, with the recommended transport.
  press: read evaluated CVs zero-copy;  move x60: kernel on the footprint +
  sparse push;  release: author one Vt array to the stage.
"""
import ctypes, gc, os, sys, time
import numpy as np
from pxr import Vt, Gf, Sdf, Usd, UsdGeom

N = int(sys.argv[1]) if len(sys.argv) > 1 else 100000
M = int(sys.argv[2]) if len(sys.argv) > 2 else 2000      # brush footprint CVs
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _usdgenTransport as BP
import _usdgenPb as PB
lib = ctypes.CDLL(os.path.join(HERE, "libUsdGenTransportC.so"))
lib.UsdGenImaging_Resize.argtypes = [ctypes.c_char_p, ctypes.c_int]
lib.UsdGenImaging_SetLiveOverrideIndexed.argtypes = [
    ctypes.c_char_p, ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_float), ctypes.c_int]

SPATH = "/Groom/guides"; PATH = SPATH.encode()
BP.Alloc(SPATH, N); PB.Alloc(SPATH, N); lib.UsdGenImaging_Resize(PATH, N)
print("N=%d CVs, footprint M=%d CVs" % (N, M))

def bench(label, fn, reps=51):
    fn()
    ts = []
    for _ in range(reps):
        t0 = time.perf_counter(); fn(); ts.append(time.perf_counter()-t0)
    ts.sort()
    print("  %-58s best %8.2f us  median %8.2f us"
          % (label, ts[0]*1e6, ts[len(ts)//2]*1e6), flush=True)

# ---- lifetime safety of the zero-copy view -------------------------------
vt = BP.GetPointsCow(SPATH)
view = np.asarray(vt)
first = float(view[0][0]); del vt; gc.collect()
print("zero-copy view survives dropping the Vt python object:",
      float(view[0][0]) == first, "value", float(view[0][0]))

# ---- press ---------------------------------------------------------------
def press():
    a = BP.GetPointsCow(SPATH)        # COW handle, must stay alive
    return a, np.asarray(a)
bench("press: GetPointsCow + np.asarray (zero copy)", press)
hold, base = press()

idx = np.random.default_rng(0).choice(N, size=M, replace=False).astype(np.int32)
idx.sort()
delta = np.array([0.01, 0.0, 0.0], dtype=np.float32)

def kernel_numpy():
    sub = base[idx] + delta                      # gather + displace
    return np.ascontiguousarray(sub, dtype=np.float32)
bench("move: numpy gather+displace over the footprint", kernel_numpy)
sub = kernel_numpy()

def push_ctypes():
    return lib.UsdGenImaging_SetLiveOverrideIndexed(
        PATH, idx.ctypes.data_as(ctypes.POINTER(ctypes.c_int)),
        sub.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), M)
bench("move: ctypes sparse push of the footprint", push_ctypes)

def move_full():
    s = kernel_numpy()
    return lib.UsdGenImaging_SetLiveOverrideIndexed(
        PATH, idx.ctypes.data_as(ctypes.POINTER(ctypes.c_int)),
        s.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), M)
bench("MOVE TOTAL (kernel + sparse push)", move_full)

# pure python fallback, no numpy
base_list = [tuple(map(float, p)) for p in base[:0]] # not materialised
import array as _array
flat = _array.array('f', np.asarray(base).ravel().tolist())
idx_list = idx.tolist()
def kernel_pure():
    out = _array.array('f', [0.0]) * (3*M)
    for j, k in enumerate(idx_list):
        b = 3*k; o = 3*j
        out[o] = flat[b] + 0.01; out[o+1] = flat[b+1]; out[o+2] = flat[b+2]
    return out
bench("move: PURE PYTHON gather+displace over the footprint (no numpy)",
      kernel_pure, reps=21)

# ---- release -------------------------------------------------------------
stage = Usd.Stage.CreateInMemory()
curves = UsdGeom.BasisCurves.Define(stage, SPATH)
pattr = curves.CreatePointsAttr()
def release_vt():
    a = BP.GetPointsCow(SPATH)            # C++ hands back a Vt array, COW
    with Sdf.ChangeBlock():
        pattr.Set(a)
bench("release: C++ VtArray -> attr.Set inside one ChangeBlock", release_vt)

def release_numpy():
    a = PB.GetPointsNumpyView(SPATH)
    with Sdf.ChangeBlock():
        pattr.Set(Vt.Vec3fArray.FromBuffer(a))
bench("release: numpy view -> Vt.FromBuffer -> attr.Set", release_numpy, reps=11)

# ---- freeze: author N curves' points + counts + widths -------------------
counts = Vt.IntArray(N // 10, 10)
widths = Vt.FloatArray(N, 0.01)

def freeze_usd_no_block():
    st = Usd.Stage.CreateInMemory()
    c = UsdGeom.BasisCurves.Define(st, "/Frozen")
    c.CreateCurveVertexCountsAttr(counts)
    c.CreatePointsAttr(BP.GetPointsCow(SPATH))
    c.CreateWidthsAttr(widths)
    c.CreateTypeAttr(UsdGeom.Tokens.cubic)
    c.CreateBasisAttr(UsdGeom.Tokens.bspline)
    return st
bench("freeze: Usd API, no ChangeBlock (Define + 5 attrs)",
      freeze_usd_no_block, reps=21)

def freeze_usd_define_then_block():
    st = Usd.Stage.CreateInMemory()
    c = UsdGeom.BasisCurves.Define(st, "/Frozen")     # MUST be outside
    with Sdf.ChangeBlock():
        c.CreateCurveVertexCountsAttr(counts)
        c.CreatePointsAttr(BP.GetPointsCow(SPATH))
        c.CreateWidthsAttr(widths)
    return st
bench("freeze: Define outside, 3 attrs inside one ChangeBlock",
      freeze_usd_define_then_block, reps=21)

def freeze_sdf_block():
    st = Usd.Stage.CreateInMemory()
    lyr = st.GetRootLayer()
    with Sdf.ChangeBlock():
        prim = Sdf.CreatePrimInLayer(lyr, "/Frozen")
        prim.specifier = Sdf.SpecifierDef
        prim.typeName = "BasisCurves"
        for name, tn, val in (("curveVertexCounts", Sdf.ValueTypeNames.IntArray, counts),
                              ("points", Sdf.ValueTypeNames.Point3fArray,
                               BP.GetPointsCow(SPATH)),
                              ("widths", Sdf.ValueTypeNames.FloatArray, widths)):
            a = Sdf.AttributeSpec(prim, name, tn)
            a.default = val
    return st
bench("freeze: pure Sdf API entirely inside one ChangeBlock",
      freeze_sdf_block, reps=21)

st = freeze_usd_no_block()
OUT = os.path.join(HERE, "frozen.usdc")
def save_usdc():
    st.GetRootLayer().Export(OUT)
bench("freeze: Export() the frozen layer to .usdc", save_usdc, reps=5)
print("frozen.usdc bytes:", os.path.getsize(OUT))

# Does Define inside a ChangeBlock really fail?  (documented trap)
try:
    st2 = Usd.Stage.CreateInMemory()
    with Sdf.ChangeBlock():
        UsdGeom.BasisCurves.Define(st2, "/Nope")
    print("Define inside ChangeBlock: SUCCEEDED (unexpected)")
except Exception as e:
    print("Define inside ChangeBlock FAILS:", str(e).strip().splitlines()[-1][:150])
