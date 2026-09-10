// usdGen engine — types.cpp: density decimation predicate (ADR §9 R12/R13).
#include "usdGen/types.h"
#include "usdGenMath/usdGenMath/hash.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

// R13: keep iff Draw01(kSaltDensity, curveId) < keepFraction. kSaltDensity != 0
// so the surviving set is never {hairId < keepFraction}. keepFraction >= 1
// keeps everything (density scrub at max); <= 0 keeps nothing.
bool KeepCurve(uint64_t curveId, float keepFraction)
{
    if (keepFraction >= 1.0f) return true;
    if (keepFraction <= 0.0f) return false;
    return UsdGenDraw01(0, curveId, kSaltDensity) < keepFraction;
}

}  // namespace usdGen
