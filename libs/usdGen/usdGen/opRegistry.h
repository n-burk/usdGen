// usdGen engine — internal operator registry (03 §8.2; ADR §3: internal in v1/v2).
//
// Maps (type name, algorithmVersion) to a factory. M1 registers five types:
// UsdGenScatter, UsdGenGrow, UsdGenNoise, UsdGenLength, UsdGenWidth.
// algorithmVersion 0 means "track the newest kernel" (R17); the registry
// resolves 0 to the newest known version at compile time.
#ifndef USDGEN_OP_REGISTRY_H
#define USDGEN_OP_REGISTRY_H

#include "usdGen/op.h"

#include <functional>
#include <memory>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

class UsdGenOpRegistry
{
public:
    using Factory = std::function<std::unique_ptr<UsdGenOp>()>;

    static UsdGenOpRegistry &Get();

    /// Register a kernel for (type, version). The registry owns the factory,
    /// not the instances. Returns false on duplicate (type, version).
    /// Extension registration is startup-only: finish before launching any
    /// compiler/readers. Runtime graph compilation never mutates the registry.
    bool Register(TfToken const &type, int algorithmVersion, Factory factory);

    /// Create an operator instance. *outVersion receives the resolved version.
    /// nullptr when the type is unknown or has no kernel (published types with
    /// unimplemented kernels: one TF_WARN naming the prim, 02 §8.2).
    std::unique_ptr<UsdGenOp> Create(TfToken const &type, int algorithmVersion,
                                     int *outVersion = nullptr) const;

    /// Highest registered version for a type; -1 when the type is unknown.
    int NewestVersion(TfToken const &type) const;
    bool HasKernel(TfToken const &type, int algorithmVersion) const;
    /// Read the complete version-resolved static operator contract without
    /// allocating an operator during graph compilation.  The metadata is
    /// captured from the registration-time probe, so it is valid only for
    /// the selected (type, version) entry; version 0 resolves to newest.
    bool GetOperatorContract(TfToken const &type, int algorithmVersion,
                             size_t *outGeometryInputArity,
                             size_t *outReferenceInputArity,
                             UsdGenRole *outRole) const;
    /// Read the version-resolved geometry-input contract without allocating
    /// an operator during graph compilation. Returns false for an unknown
    /// type/version.
    bool GetGeometryInputArity(TfToken const &type, int algorithmVersion,
                               size_t *outArity) const;

    /// All registered type names (sorted); used by diagnostics.
    std::vector<TfToken> KnownTypes() const;

private:
    UsdGenOpRegistry();
    struct Entry {
        TfToken type;
        int version;
        Factory factory;
        size_t geometryInputArity;
        size_t referenceInputArity;
        UsdGenRole role;
    };
    std::vector<Entry> _entries;
};

// --- M1 kernel factories (registered by usdGenOpRegistryInit) ----------------
std::unique_ptr<UsdGenOp> CreateScatterOp();
std::unique_ptr<UsdGenOp> CreateGrowOp();
std::unique_ptr<UsdGenOp> CreateNoiseOp();
std::unique_ptr<UsdGenOp> CreateLengthOp();
std::unique_ptr<UsdGenOp> CreateWidthOp();
std::unique_ptr<UsdGenOp> CreateWidthBlendOp();
std::unique_ptr<UsdGenOp> CreateReferenceSourceOp();

/// Compatibility initializer. Built-ins are installed by registry construction;
/// concurrent compiler calls only obtain that completed immutable initial state.
void usdGenRegisterM1Operators();

}  // namespace usdGen

#endif  // USDGEN_OP_REGISTRY_H
