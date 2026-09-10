// usdGen engine — operator registry implementation (M1 scaffold).
//
// M1 registers exactly five kernels (11-roadmap.md §2.2): UsdGenScatter
// (mode=random), UsdGenGrow, UsdGenNoise, UsdGenLength, UsdGenWidth.
// Later milestones register more via the same entry point; the registry is
// internal in v1/v2 (03 §8.2).
#include "usdGen/opRegistry.h"

#include "usdGen/ops/scatter.h"
#include "usdGen/ops/grow.h"
#include "usdGen/ops/noise.h"
#include "usdGen/ops/length.h"
#include "usdGen/ops/width.h"

#include "pxr/pxr.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

UsdGenOpRegistry &UsdGenOpRegistry::Get()
{
    static UsdGenOpRegistry instance;
    return instance;
}

bool UsdGenOpRegistry::Register(
    TfToken const &type, int algorithmVersion, Factory factory)
{
    for (Entry const &e : _entries) {
        if (e.type == type && e.version == algorithmVersion) return false;
    }
    _entries.push_back(Entry{type, algorithmVersion, std::move(factory)});
    return true;
}

std::unique_ptr<UsdGenOp> UsdGenOpRegistry::Create(
    TfToken const &type, int algorithmVersion, int *outVersion) const
{
    int bestVersion = -1;
    Factory bestFactory;
    for (Entry const &e : _entries) {
        if (e.type != type) continue;
        if (algorithmVersion != 0 && e.version != algorithmVersion) continue;
        if (e.version > bestVersion) { bestVersion = e.version; bestFactory = e.factory; }
    }
    if (!bestFactory) return nullptr;
    if (outVersion) *outVersion = (algorithmVersion != 0) ? algorithmVersion : bestVersion;
    return bestFactory();
}

int UsdGenOpRegistry::NewestVersion(TfToken const &type) const
{
    int newest = -1;
    for (Entry const &e : _entries) {
        if (e.type == type && e.version > newest) newest = e.version;
    }
    return newest;
}

std::vector<TfToken> UsdGenOpRegistry::KnownTypes() const
{
    std::vector<TfToken> types;
    for (Entry const &e : _entries) types.push_back(e.type);
    return types;
}

std::unique_ptr<UsdGenOp> CreateScatterOp() { return std::make_unique<UsdGenScatterOp>(); }
std::unique_ptr<UsdGenOp> CreateGrowOp()    { return std::make_unique<UsdGenGrowOp>(); }
std::unique_ptr<UsdGenOp> CreateNoiseOp()   { return std::make_unique<UsdGenNoiseOp>(); }
std::unique_ptr<UsdGenOp> CreateLengthOp()  { return std::make_unique<UsdGenLengthOp>(); }
std::unique_ptr<UsdGenOp> CreateWidthOp()   { return std::make_unique<UsdGenWidthOp>(); }

void usdGenRegisterM1Operators()
{
    UsdGenOpRegistry::Get().Register(TfToken("UsdGenScatter"), 0, &CreateScatterOp);
    UsdGenOpRegistry::Get().Register(TfToken("UsdGenGrow"), 0, &CreateGrowOp);
    UsdGenOpRegistry::Get().Register(TfToken("UsdGenNoise"), 0, &CreateNoiseOp);
    UsdGenOpRegistry::Get().Register(TfToken("UsdGenLength"), 0, &CreateLengthOp);
    UsdGenOpRegistry::Get().Register(TfToken("UsdGenWidth"), 0, &CreateWidthOp);
}

}  // namespace usdGen
