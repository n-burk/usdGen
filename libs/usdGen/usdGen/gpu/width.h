#ifndef USDGEN_GPU_WIDTH_H
#define USDGEN_GPU_WIDTH_H

#include "styleOps.h"

#include <cstdint>

namespace usdGen::gpu {

// Runtime expression booleans are represented as bytes, never as float
// fields. A null pointer with count zero is a Groom-domain literal.
struct BoolField {
    const uint8_t *data = nullptr;
    size_t count = 0;
    expr::Domain domain = expr::Domain::Groom;
    bool literal = true;

    static BoolField Literal(bool value) {
        BoolField result;
        result.literal = value;
        return result;
    }
    static BoolField Device(DeviceView<const uint8_t> values,
                            expr::Domain d) {
        return {values.data, values.size, d, true};
    }
};

// Controls for the Width primitive. `base * width` is the target's scalar
// width, allowing a caller to supply a groom-level base multiplier while
// retaining the schema's `width` value. Width and mask profiles are sampled
// by interpolating the 257-entry device LUT at hairT (or canonical CV t).
// maskProfile may be empty, meaning an all-one envelope ramp.
struct WidthParameters {
    ScalarField base = ScalarField::Literal(1.0f);
    ScalarField width = ScalarField::Literal(0.01f);
    ScalarField rootScale = ScalarField::Literal(1.0f);
    ScalarField tipScale = ScalarField::Literal(1.0f);
    ScalarField taper = ScalarField::Literal(0.0f);
    ScalarField taperStart = ScalarField::Literal(0.5f);
    ScalarField blend = ScalarField::Literal(1.0f);
    ScalarField maskAmount = ScalarField::Literal(1.0f);
    BoolField enabled = BoolField::Literal(true);
    BoolField replace = BoolField::Literal(true);
    DeviceView<const float> widthProfile{}; // exactly 257 entries
    DeviceView<const float> maskProfile{};  // empty or exactly 257 entries
};

// Device-only, topology-preserving Width primitive. All inputs and output
// storage are borrowed until Finish returns. Apply queues one operation;
// output is written only after Finish validates the one scalar device error.
class CudaWidth {
public:
    CudaWidth() = default;
    ~CudaWidth();
    CudaWidth(CudaWidth const &) = delete;
    CudaWidth &operator=(CudaWidth const &) = delete;

    StyleStatus Apply(DeviceCurveGeometryView geometry,
                      DeviceView<const float> hairT,
                      WidthParameters parameters,
                      DeviceView<float> output,
                      cudaStream_t stream);
    StyleStatus Finish(cudaStream_t stream);
    bool pending() const { return pending_; }

private:
    StyleStatus validateField(ScalarField field,
                              DeviceCurveGeometryView geometry) const;
    StyleStatus validateBoolField(BoolField field,
                                  DeviceCurveGeometryView geometry,
                                  bool groomOnly) const;
    StyleStatus begin(DeviceCurveGeometryView geometry,
                      DeviceView<float> output, cudaStream_t stream);
    StyleStatus finishPublication(cudaStream_t stream);

    DeviceBuffer<int> error_;
    DeviceBuffer<float> staging_;
    cudaEvent_t ready_ = nullptr;
    DeviceView<float> output_{};
    size_t pointCount_ = 0;
    int deviceIndex_ = -1;
    bool pending_ = false;
};

} // namespace usdGen::gpu

#endif // USDGEN_GPU_WIDTH_H
