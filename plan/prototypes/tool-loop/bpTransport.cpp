// bpTransport.cpp -- a pxr_boost::python extension module in the shape USD's
// own modules use.  Demonstrates VtArray transport across the Python/C++
// boundary with USD's native converters (copy-on-write => O(1) per call).
#include "pxr/pxr.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/pyUtils.h"
#include "pxr/external/boost/python.hpp"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
namespace bp = pxr_boost::python;

namespace {

struct Store {
    std::unordered_map<std::string, VtVec3fArray> vtPoints;   // COW store
    std::unordered_map<std::string, std::vector<float>> raw;  // "snapshot" store
    long long generation = 0;
};
Store& G() { static Store s; return s; }

// Store the Vt array by COW assignment: no element copy.
int SetPointsCow(std::string const& path, VtVec3fArray const& pts)
{
    G().vtPoints[path] = pts;
    ++G().generation;
    return int(pts.size());
}

// Store into a plain float vector: a real memcpy of 3*N floats.
int SetPointsMemcpy(std::string const& path, VtVec3fArray const& pts)
{
    auto& v = G().raw[path];
    v.resize(pts.size() * 3);
    std::memcpy(v.data(), pts.cdata(), sizeof(GfVec3f) * pts.size());
    ++G().generation;
    return int(pts.size());
}

// Sparse update from two Vt arrays (brush footprint).
int SetPointsIndexed(std::string const& path, VtIntArray const& idx,
                     VtVec3fArray const& pts)
{
    auto it = G().vtPoints.find(path);
    if (it == G().vtPoints.end()) return -1;
    GfVec3f* d = it->second.data();      // detaches COW once
    const size_t n = it->second.size();
    for (size_t i = 0; i < idx.size(); ++i) {
        const size_t k = size_t(idx[i]);
        if (k < n) d[k] = pts[i];
    }
    ++G().generation;
    return int(idx.size());
}

// Return the stored array: COW, no element copy.
VtVec3fArray GetPointsCow(std::string const& path)
{
    auto it = G().vtPoints.find(path);
    if (it == G().vtPoints.end()) return VtVec3fArray();
    return it->second;
}

// Return a freshly built array (a real allocation + copy of 3N floats).
VtVec3fArray GetPointsCopy(std::string const& path)
{
    auto it = G().vtPoints.find(path);
    if (it == G().vtPoints.end()) return VtVec3fArray();
    VtVec3fArray out(it->second.size());
    std::memcpy(out.data(), it->second.cdata(),
                sizeof(GfVec3f) * it->second.size());
    return out;
}

int Alloc(std::string const& path, int n)
{
    G().vtPoints[path] = VtVec3fArray(size_t(n), GfVec3f(1.0f, 2.0f, 3.0f));
    return n;
}

// Pure call-overhead baseline with a string argument.
int Noop(std::string const& path) { return int(path.size()); }

// Call overhead when the array is passed but immediately ignored.
int TakeAndIgnore(std::string const& path, VtVec3fArray const& pts)
{ (void)path; return int(pts.size()); }

long long GetGeneration() { return G().generation; }

}  // namespace

PXR_BOOST_PYTHON_MODULE(_usdgenTransport)
{
    bp::def("SetPointsCow", SetPointsCow);
    bp::def("SetPointsMemcpy", SetPointsMemcpy);
    bp::def("SetPointsIndexed", SetPointsIndexed);
    bp::def("GetPointsCow", GetPointsCow);
    bp::def("GetPointsCopy", GetPointsCopy);
    bp::def("Alloc", Alloc);
    bp::def("Noop", Noop);
    bp::def("TakeAndIgnore", TakeAndIgnore);
    bp::def("GetGeneration", GetGeneration);
}
