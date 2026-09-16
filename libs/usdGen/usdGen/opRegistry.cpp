// usdGen engine — operator registry implementation (M1 scaffold).
//
// M1 registers exactly nine kernels (11-roadmap.md §2.2): UsdGenScatter,
// UsdGenGrow, UsdGenNoise, UsdGenLength, UsdGenWidth, UsdGenWidthBlend,
// UsdGenCurveSource, UsdGenDeform and UsdGenReferenceSource. Later milestones
// register more via the same entry point; the registry is internal in
// v1/v2 (03 §8.2).
#include "usdGen/opRegistry.h"

#include "usdGen/ops/scatter.h"
#include "usdGen/ops/grow.h"
#include "usdGen/ops/noise.h"
#include "usdGen/ops/length.h"
#include "usdGen/ops/width.h"
#include "usdGen/ops/widthBlend.h"
#include "usdGen/ops/curveSource.h"
#include "usdGen/ops/deform.h"
#include "usdGen/ops/referenceSource.h"

#include "pxr/pxr.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

UsdGenOpRegistry &UsdGenOpRegistry::Get()
{
    static UsdGenOpRegistry instance;
    return instance;
}

bool UsdGenOpRegistry::Register(TfToken const &type, Factory factory)
{
    for (Entry const &e : _entries) {
        if (e.type == type) return false;
    }
    if (!factory) return false;
    std::unique_ptr<UsdGenOp> probe = factory();
    if (!probe) return false;
    _entries.push_back(Entry{type, std::move(factory),
                             probe->GeometryInputArity(),
                             probe->ReferenceInputs().size(), probe->Role()});
    return true;
}

std::unique_ptr<UsdGenOp> UsdGenOpRegistry::Create(TfToken const &type) const
{
    for (Entry const &e : _entries) {
        if (e.type == type && e.factory) return e.factory();
    }
    return nullptr;
}

std::vector<TfToken> UsdGenOpRegistry::KnownTypes() const
{
    std::vector<TfToken> types;
    for (Entry const &e : _entries) types.push_back(e.type);
    return types;
}

bool UsdGenOpRegistry::HasKernel(TfToken const &type) const
{
    for (auto const& entry : _entries)
        if (entry.type == type && entry.factory) return true;
    return false;
}

bool UsdGenOpRegistry::GetGeometryInputArity(
    TfToken const &type, size_t *outArity) const
{
    return GetOperatorContract(type, outArity, nullptr, nullptr);
}

bool UsdGenOpRegistry::GetOperatorContract(
    TfToken const &type, size_t *outGeometryInputArity,
    size_t *outReferenceInputArity, UsdGenRole *outRole) const
{
    Entry const *best = nullptr;
    for (Entry const &entry : _entries) {
        if (entry.type != type) continue;
        best = &entry;
        break;
    }
    if (!best) return false;
    if (outGeometryInputArity)
        *outGeometryInputArity = best->geometryInputArity;
    if (outReferenceInputArity)
        *outReferenceInputArity = best->referenceInputArity;
    if (outRole) *outRole = best->role;
    return true;
}

std::unique_ptr<UsdGenOp> CreateScatterOp() { return std::make_unique<UsdGenScatterOp>(); }
std::unique_ptr<UsdGenOp> CreateGrowOp()    { return std::make_unique<UsdGenGrowOp>(); }
std::unique_ptr<UsdGenOp> CreateNoiseOp()   { return std::make_unique<UsdGenNoiseOp>(); }
std::unique_ptr<UsdGenOp> CreateLengthOp()  { return std::make_unique<UsdGenLengthOp>(); }
std::unique_ptr<UsdGenOp> CreateWidthOp()   { return std::make_unique<UsdGenWidthOp>(); }
std::unique_ptr<UsdGenOp> CreateWidthBlendOp() { return std::make_unique<UsdGenWidthBlendOp>(); }
std::unique_ptr<UsdGenOp> CreateCurveSourceOp() { return std::make_unique<UsdGenCurveSourceOp>(); }
std::unique_ptr<UsdGenOp> CreateDeformOp()    { return std::make_unique<UsdGenDeformOp>(); }
std::unique_ptr<UsdGenOp> CreateReferenceSourceOp() { return std::make_unique<UsdGenReferenceSourceOp>(); }

UsdGenOpRegistry::UsdGenOpRegistry()
{
    _entries.reserve(9);
    Register(TfToken("UsdGenScatter"), &CreateScatterOp);
    Register(TfToken("UsdGenGrow"), &CreateGrowOp);
    Register(TfToken("UsdGenNoise"), &CreateNoiseOp);
    Register(TfToken("UsdGenLength"), &CreateLengthOp);
    Register(TfToken("UsdGenWidth"), &CreateWidthOp);
    Register(TfToken("UsdGenWidthBlend"), &CreateWidthBlendOp);
    Register(TfToken("UsdGenCurveSource"), &CreateCurveSourceOp);
    Register(TfToken("UsdGenDeform"), &CreateDeformOp);
    Register(TfToken("UsdGenReferenceSource"), &CreateReferenceSourceOp);
}

void usdGenRegisterM1Operators() { (void)UsdGenOpRegistry::Get(); }

}  // namespace usdGen
