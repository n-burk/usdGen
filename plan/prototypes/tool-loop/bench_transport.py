"""Per-call cost of every viable Python<->C++ array transport for the usdGen
brush/freeze tool loop.  Run with:
  PYTHONPATH=<usd site-packages>:. python3 bench_transport.py [N]
"""
import ctypes, os, sys, time, array
import numpy as np
from pxr import Vt, Gf, Sdf, Usd, UsdGeom

N = int(sys.argv[1]) if len(sys.argv) > 1 else 100000     # CVs
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _usdgenTransport as BP
import _usdgenPb as PB

lib = ctypes.CDLL(os.path.join(HERE, "libUsdGenTransportC.so"))
lib.UsdGenImaging_Resize.argtypes = [ctypes.c_char_p, ctypes.c_int]
lib.UsdGenImaging_SetLiveOverride.argtypes = [
    ctypes.c_char_p, ctypes.POINTER(ctypes.c_float), ctypes.c_int]
lib.UsdGenImaging_SetLiveOverrideIndexed.argtypes = [
    ctypes.c_char_p, ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_float), ctypes.c_int]
lib.UsdGenImaging_ReadCurvesPtr.argtypes = [
    ctypes.c_char_p, ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
    ctypes.POINTER(ctypes.c_int)]
lib.UsdGenImaging_ReadCurvesCopy.argtypes = [
    ctypes.c_char_p, ctypes.POINTER(ctypes.c_float), ctypes.c_int]
lib.UsdGenImaging_Noop.argtypes = [
    ctypes.c_char_p, ctypes.POINTER(ctypes.c_float), ctypes.c_int]
lib.UsdGenImaging_GetGeneration.restype = ctypes.c_longlong

REPS = int(os.environ.get("REPS", "21"))
ROWS = []
def t(group, label, fn, reps=REPS):
    fn()                                   # warm
    ts = []
    for _ in range(reps):
        t0 = time.perf_counter(); r = fn(); ts.append(time.perf_counter() - t0)
    ts.sort()
    best, med = ts[0], ts[len(ts)//2]
    ROWS.append((group, label, best*1e6, med*1e6))
    print("%-10s %-62s best %9.2f us   median %9.2f us"
          % (group, label, best*1e6, med*1e6), flush=True)
    return r

print("N CVs = %d  (%d floats, %.2f MB)" % (N, 3*N, 3*N*4/1e6))
PATH = b"/Groom/guides"
SPATH = "/Groom/guides"
lib.UsdGenImaging_Resize(PATH, N)
BP.Alloc(SPATH, N)
PB.Alloc(SPATH, N)

# ---------------------------------------------------------------- source data
np_pts   = np.ones((N, 3), dtype=np.float32)
arr_pts  = array.array('f', [1.0]) * (3*N)
vt_pts   = Vt.Vec3fArray(N, Gf.Vec3f(1, 2, 3))
list_pts = [(1.0, 2.0, 3.0)] * N

# ---------------------------------------------------------------- reference
t("ref", "numpy memcpy of the same buffer (np.copy)", lambda: np_pts.copy())
t("ref", "ctypes call overhead, no data (Noop)",
  lambda: lib.UsdGenImaging_Noop(PATH, None, N))
t("ref", "pybind11 call overhead, no data (Noop)", lambda: PB.Noop(SPATH))
t("ref", "pxr_boost call overhead, no data (Noop)", lambda: BP.Noop(SPATH))

# ---------------------------------------------------------------- PUSH py->c++
np_ptr = np_pts.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
t("push", "ctypes: numpy .ctypes.data_as + C memcpy",
  lambda: lib.UsdGenImaging_SetLiveOverride(
      PATH, np_pts.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), N))
t("push", "ctypes: cached float* + C memcpy",
  lambda: lib.UsdGenImaging_SetLiveOverride(PATH, np_ptr, N))
c_arr = (ctypes.c_float * (3*N)).from_buffer(arr_pts)
t("push", "ctypes: array.array from_buffer (zero copy) + C memcpy",
  lambda: lib.UsdGenImaging_SetLiveOverride(
      PATH, (ctypes.c_float * (3*N)).from_buffer(arr_pts), N))
t("push", "ctypes: cached array.array ptr + C memcpy",
  lambda: lib.UsdGenImaging_SetLiveOverride(PATH, c_arr, N))

def vt_to_ptr_np():
    a = np.frombuffer(memoryview(vt_pts), dtype=np.float32)  # zero copy, RO
    return lib.UsdGenImaging_SetLiveOverride(
        PATH, ctypes.cast(a.ctypes.data, ctypes.POINTER(ctypes.c_float)), N)
t("push", "ctypes: Vt -> np.frombuffer(memoryview) -> ptr + C memcpy",
  vt_to_ptr_np)
t("push", "ctypes: Vt -> from_buffer_copy (python-side copy) + C memcpy",
  lambda: lib.UsdGenImaging_SetLiveOverride(
      PATH,
      ctypes.cast((ctypes.c_float*(3*N)).from_buffer_copy(memoryview(vt_pts)),
                  ctypes.POINTER(ctypes.c_float)), N))

t("push", "pybind11: py::array_t<float> in, C memcpy",
  lambda: PB.SetPointsNumpy(SPATH, np_pts))
t("push", "pybind11: py::array_t<float> in, nothing copied",
  lambda: PB.TakeNumpy(SPATH, np_pts))
t("push", "pybind11: numpy float64 in (forcecast) + C memcpy",
  lambda: PB.SetPointsNumpy(SPATH, np_pts.astype(np.float64)))
t("push", "pybind11: py::object -> pxr_boost extract VtArray& (COW store)",
  lambda: PB.SetPointsVtViaBoost(SPATH, vt_pts))

t("push", "pxr_boost: VtVec3fArray in, COW store (no element copy)",
  lambda: BP.SetPointsCow(SPATH, vt_pts))
t("push", "pxr_boost: VtVec3fArray in, memcpy into float store",
  lambda: BP.SetPointsMemcpy(SPATH, vt_pts))
t("push", "pxr_boost: VtVec3fArray in, ignored (conversion only)",
  lambda: BP.TakeAndIgnore(SPATH, vt_pts))
t("push", "pxr_boost: python LIST of N tuples in (auto sequence convert)",
  lambda: BP.TakeAndIgnore(SPATH, list_pts), reps=3)

# ---------------------------------------------------------------- PULL c++->py
outp = ctypes.POINTER(ctypes.c_float)()
outn = ctypes.c_int()
def pull_ctypes_as_array():
    lib.UsdGenImaging_ReadCurvesPtr(PATH, ctypes.byref(outp), ctypes.byref(outn))
    return np.ctypeslib.as_array(outp, shape=(outn.value, 3))
a = t("pull", "ctypes: ReadCurvesPtr -> np.ctypeslib.as_array (zero copy)",
      pull_ctypes_as_array)
assert a.shape == (N, 3)
dst = np.empty((N, 3), dtype=np.float32)
t("pull", "ctypes: ReadCurvesCopy into a preallocated numpy buffer",
  lambda: lib.UsdGenImaging_ReadCurvesCopy(
      PATH, dst.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), N))
t("pull", "pybind11: numpy view over C storage (capsule, zero copy)",
  lambda: PB.GetPointsNumpyView(SPATH))
t("pull", "pybind11: numpy copy out",
  lambda: PB.GetPointsNumpyCopy(SPATH))
t("pull", "pxr_boost: VtVec3fArray out, COW (no element copy)",
  lambda: BP.GetPointsCow(SPATH))
t("pull", "pxr_boost: VtVec3fArray out, real element copy",
  lambda: BP.GetPointsCopy(SPATH))
t("pull", "pybind11: list-of-lists out (usdRig _rigexec.cpp route)",
  lambda: PB.GetPointsAsListOfLists(SPATH), reps=3)

# ---------------------------------------------------------------- conversions
t("conv", "np.asarray(Vt.Vec3fArray) (buffer protocol, zero copy?)",
  lambda: np.asarray(vt_pts))
z = np.asarray(vt_pts)
print("        np.asarray(Vt) shares memory:",
      z.__array_interface__['data'][0] ==
      np.frombuffer(memoryview(vt_pts), dtype=np.float32).__array_interface__['data'][0],
      "writeable:", z.flags.writeable, "shape", z.shape, "dtype", z.dtype)
t("conv", "Vt.Vec3fArray.FromBuffer(numpy float32 (N,3))",
  lambda: Vt.Vec3fArray.FromBuffer(np_pts))
t("conv", "Vt.Vec3fArray.FromBuffer(numpy float64 (N,3))",
  lambda: Vt.Vec3fArray.FromBuffer(np_pts.astype(np.float64)), reps=5)
t("conv", "Vt.Vec3fArray(list of N tuples)", lambda: Vt.Vec3fArray(list_pts),
  reps=3)
t("conv", "list(Vt.Vec3fArray) -> N Gf.Vec3f objects",
  lambda: list(vt_pts), reps=3)
t("conv", "np.array(list of N tuples)", lambda: np.array(list_pts, dtype=np.float32),
  reps=3)
t("conv", "memoryview(Vt.Vec3fArray) alone", lambda: memoryview(vt_pts))

# ---------------------------------------------------------------- sparse (brush)
M = 2000
idx_np = np.arange(M, dtype=np.int32)
sub_np = np.ones((M, 3), dtype=np.float32)
idx_vt = Vt.IntArray(list(range(M)))
sub_vt = Vt.Vec3fArray(M, Gf.Vec3f(1, 2, 3))
t("sparse", "ctypes: indexed update of %d CVs" % M,
  lambda: lib.UsdGenImaging_SetLiveOverrideIndexed(
      PATH, idx_np.ctypes.data_as(ctypes.POINTER(ctypes.c_int)),
      sub_np.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), M))
t("sparse", "pxr_boost: indexed update of %d CVs" % M,
  lambda: BP.SetPointsIndexed(SPATH, idx_vt, sub_vt))

# ---------------------------------------------------------------- stage write
stage = Usd.Stage.CreateInMemory()
curves = UsdGeom.BasisCurves.Define(stage, "/Groom/guides")
pattr = curves.CreatePointsAttr()
t("stage", "attr.Set(Vt.Vec3fArray) into an in-memory root layer",
  lambda: pattr.Set(vt_pts))
def set_in_block():
    with Sdf.ChangeBlock():
        pattr.Set(vt_pts)
t("stage", "attr.Set inside Sdf.ChangeBlock", set_in_block)
t("stage", "attr.Set(numpy (N,3) float32) directly", lambda: pattr.Set(np_pts))
t("stage", "attr.Set(Vt.Vec3fArray.FromBuffer(numpy))",
  lambda: pattr.Set(Vt.Vec3fArray.FromBuffer(np_pts)))
t("stage", "attr.Get() -> Vt.Vec3fArray", lambda: pattr.Get())
t("stage", "attr.Set(vt, time=1.0) time sample", lambda: pattr.Set(vt_pts, 1.0))

print()
print("| group | route | best us | median us |")
print("|---|---|---|---|")
for g, l, b, m in ROWS:
    print("| %s | %s | %.2f | %.2f |" % (g, l, b, m))
