// usdGen engine — generation store and publication diff (03 §6.1/§6.2).
//
// A generation is immutable once published. The publish/read pair is
// std::atomic_store/std::atomic_load over std::shared_ptr — NOT TfRefPtr
// (no atomic overloads in 26.08; verified grep of pxr/base/tf/refPtr.h).
// "Did this tile change" is VtArray::IsIdentical() (data-pointer comparison,
// pxr/base/vt/array.h:945-950), not memcmp.
#ifndef USDGEN_GENERATION_STORE_H
#define USDGEN_GENERATION_STORE_H

#include "usdGen/curveBuffer.h"

#include <cstdint>
#include <memory>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

struct UsdGenGeneration
{
    int64_t id = -1;
    double frame = 0.0;
    std::vector<UsdGenTilePublication> tiles;        // C2 payload, sorted by tile id
    std::vector<UsdGenTilePublication> guides;       // guides/<setName> prims (§4.2)
    std::vector<UsdGenInstancerPublication> instancers; // M6
    UsdGenPrimSetSignature signature;
};
using UsdGenGenerationConstPtr = std::shared_ptr<const UsdGenGeneration>;

class UsdGenGenerationStore
{
public:
    /// Commit thread only: assigns the next generation id, wraps the
    /// (moved) generation in a const shared_ptr and std::atomic_stores it.
    void Publish(UsdGenGeneration gen);

    /// Lock-free read (Hydra readers: GetPrim only atomic_loads — I7).
    UsdGenGenerationConstPtr Get() const noexcept;

    /// Cheap structural diff of two generations (03 §6.1): payload changes via
    /// IsIdentical, structural changes via the signature. Consumed by the
    /// imaging notice emitter (06 §5.1 rules).
    UsdGenDirtyReport Diff(UsdGenGeneration const &prev,
                           UsdGenGeneration const &next) const;

    int64_t NextId() const noexcept;
private:
    UsdGenGenerationConstPtr _current;
    int64_t _nextId = 0;
};

}  // namespace usdGen

#endif  // USDGEN_GENERATION_STORE_H
