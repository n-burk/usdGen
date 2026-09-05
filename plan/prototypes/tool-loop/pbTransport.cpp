// pbTransport.cpp -- pybind11 transport routes, incl. a hybrid that extracts a
// pxr Vt array out of a plain py::object using pxr_boost::python's registry.
#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>

#include "pxr/pxr.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/external/boost/python.hpp"

#include <array>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace py = pybind11;
PXR_NAMESPACE_USING_DIRECTIVE
namespace bp = pxr_boost::python;

namespace {
std::unordered_map<std::string, std::vector<float>> gRaw;
std::unordered_map<std::string, VtVec3fArray> gVt;
long long gGen = 0;

void Alloc(std::string const& p, int n)
{
    gRaw[p].assign(size_t(n) * 3, 1.0f);
    gVt[p] = VtVec3fArray(size_t(n), GfVec3f(1.0f, 2.0f, 3.0f));
}

// numpy in, memcpy into the store (a real publish).
int SetPointsNumpy(std::string const& p,
                   py::array_t<float, py::array::c_style | py::array::forcecast> a)
{
    auto buf = a.request();
    auto& v = gRaw[p];
    v.resize(size_t(buf.size));
    std::memcpy(v.data(), buf.ptr, sizeof(float) * size_t(buf.size));
    ++gGen;
    return int(buf.size / 3);
}

// numpy in, pointer taken but nothing copied (call + conversion overhead only).
int TakeNumpy(std::string const& p,
              py::array_t<float, py::array::c_style | py::array::forcecast> a)
{ (void)p; return int(a.size()); }

// numpy out, zero copy view over the store (lifetime tied to a capsule).
py::array_t<float> GetPointsNumpyView(std::string const& p)
{
    auto& v = gRaw.at(p);
    return py::array_t<float>({py::ssize_t(v.size() / 3), py::ssize_t(3)},
                              {py::ssize_t(12), py::ssize_t(4)},
                              v.data(), py::capsule(v.data(), [](void*) {}));
}

// numpy out, real copy.
py::array_t<float> GetPointsNumpyCopy(std::string const& p)
{
    auto& v = gRaw.at(p);
    py::array_t<float> out({py::ssize_t(v.size() / 3), py::ssize_t(3)});
    std::memcpy(out.mutable_data(), v.data(), sizeof(float) * v.size());
    return out;
}

// The usdRig _rigexec.cpp route: VtVec3fArray -> vector<array<double,3>>
// -> a Python list of lists.
std::vector<std::array<double, 3>> GetPointsAsListOfLists(std::string const& p)
{
    const VtVec3fArray& a = gVt.at(p);
    std::vector<std::array<double, 3>> out;
    out.reserve(a.size());
    for (const auto& q : a)
        out.push_back({double(q[0]), double(q[1]), double(q[2])});
    return out;
}

// Hybrid: a pxr Vt array arrives as an opaque py::object; extract it with
// pxr_boost::python's converter registry (no element copy: VtArray is COW).
int SetPointsVtViaBoost(std::string const& p, py::object obj)
{
    bp::object o(bp::handle<>(bp::borrowed(obj.ptr())));
    bp::extract<VtVec3fArray&> x(o);
    if (!x.check()) return -1;
    gVt[p] = x();          // COW assign
    ++gGen;
    return int(gVt[p].size());
}

int Noop(std::string const& p) { return int(p.size()); }
long long GetGeneration() { return gGen; }
}

PYBIND11_MODULE(_usdgenPb, m)
{
    m.def("Alloc", &Alloc);
    m.def("SetPointsNumpy", &SetPointsNumpy);
    m.def("TakeNumpy", &TakeNumpy);
    m.def("GetPointsNumpyView", &GetPointsNumpyView);
    m.def("GetPointsNumpyCopy", &GetPointsNumpyCopy);
    m.def("GetPointsAsListOfLists", &GetPointsAsListOfLists);
    m.def("SetPointsVtViaBoost", &SetPointsVtViaBoost);
    m.def("Noop", &Noop);
    m.def("GetGeneration", &GetGeneration);
}
