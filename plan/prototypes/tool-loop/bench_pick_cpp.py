import ctypes, os, sys, time
import numpy as np
from pxr import Gf
N = int(sys.argv[1]) if len(sys.argv) > 1 else 100000
HERE = os.path.dirname(os.path.abspath(__file__))
lib = ctypes.CDLL(os.path.join(HERE, "libUsdGenTransportC.so"))
lib.UsdGenImaging_Resize.argtypes = [ctypes.c_char_p, ctypes.c_int]
lib.UsdGenImaging_SetLiveOverride.argtypes = [
    ctypes.c_char_p, ctypes.POINTER(ctypes.c_float), ctypes.c_int]
lib.UsdGenImaging_PickCV.argtypes = [
    ctypes.c_char_p, ctypes.POINTER(ctypes.c_float), ctypes.c_int, ctypes.c_int,
    ctypes.c_float, ctypes.c_float, ctypes.c_float,
    ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_float)]
lib.UsdGenImaging_FootprintCV.argtypes = [
    ctypes.c_char_p, ctypes.POINTER(ctypes.c_float), ctypes.c_int, ctypes.c_int,
    ctypes.c_float, ctypes.c_float, ctypes.c_float,
    ctypes.POINTER(ctypes.c_int), ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
W, H = 1920, 1080
rng = np.random.default_rng(0)
pts = (rng.random((N, 3), dtype=np.float32) * 2.0 - 1.0).astype(np.float32)
PATH = b"/Groom/guides"
lib.UsdGenImaging_Resize(PATH, N)
lib.UsdGenImaging_SetLiveOverride(PATH, pts.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), N)
cam = Gf.Camera(); cam.SetPerspectiveFromAspectRatioAndFieldOfView(W/float(H), 60.0, Gf.Camera.FOVHorizontal)
cam.transform = Gf.Matrix4d(1).SetTranslate(Gf.Vec3d(0, 0, 5))
fr = cam.frustum
vp = (np.array(fr.ComputeViewMatrix(), dtype=np.float64).reshape(4,4) @
      np.array(fr.ComputeProjectionMatrix(), dtype=np.float64).reshape(4,4)).astype(np.float32)
vpp = vp.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
oi, od, oc = ctypes.c_int(), ctypes.c_float(), ctypes.c_int()
buf = (ctypes.c_int * 65536)()
def bench(l, f, r=101):
    f(); ts=[]
    for _ in range(r):
        t0=time.perf_counter(); f(); ts.append(time.perf_counter()-t0)
    ts.sort(); print("  %-52s best %8.2f us  median %8.2f us" % (l, ts[0]*1e6, ts[len(ts)//2]*1e6), flush=True)
print("N = %d CVs" % N)
bench("C++ PickCV nearest within 24 px (single thread)",
      lambda: lib.UsdGenImaging_PickCV(PATH, vpp, W, H, W*0.5, H*0.5, 24.0,
                                       ctypes.byref(oi), ctypes.byref(od)))
print("        idx=%d dist=%.3f px" % (oi.value, od.value))
bench("C++ FootprintCV within 240 px (single thread)",
      lambda: lib.UsdGenImaging_FootprintCV(PATH, vpp, W, H, W*0.5, H*0.5, 240.0,
                                            buf, 65536, ctypes.byref(oc)))
print("        footprint =", oc.value)
