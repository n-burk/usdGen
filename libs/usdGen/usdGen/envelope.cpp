// usdGen engine — envelope implementation (03 §8.5).
//
// Exact-endpoint rule (usdRig's RigExecBlendEnvelope contract,
// research/A1-usdrig-graph.md §5): w <= 0 aliases the input (out := in),
// w >= 1 runs no blend pass (out already holds the operator's result).
#include "usdGen/op.h"

#include <algorithm>
#include <cmath>
#include <cstring>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

void UsdGenBlendEnvelope(float const *in, float *out, float w, size_t n)
{
    if (in == out || w >= 1.0f) return;          // in-place result or full-strength
    if (w <= 0.0f) {                             // mute: alias the input
        if (in != out) std::copy(in, in + n, out);
        return;
    }
    const float iw = 1.0f - w;
    for (size_t i = 0; i < n; ++i) out[i] = in[i] * iw + out[i] * w;
}

void UsdGenBlendEnvelopeVec3(
    float const *inPx, float const *inPy, float const *inPz,
    float *outPx, float *outPy, float *outPz, float w, size_t n)
{
    UsdGenBlendEnvelope(inPx, outPx, w, n);
    UsdGenBlendEnvelope(inPy, outPy, w, n);
    UsdGenBlendEnvelope(inPz, outPz, w, n);
}

}  // namespace usdGen
