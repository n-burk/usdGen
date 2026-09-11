#ifndef USDGEN_EXPRESSIONS_CONTEXT_H
#define USDGEN_EXPRESSIONS_CONTEXT_H

#include <cstdint>
#include <string>

namespace usdGen::expr {

enum class Domain : uint8_t { None = 0, Groom = 1, Primitive = 2, Point = 4,
                              All = 7 };
constexpr Domain operator|(Domain a, Domain b) {
    return Domain(uint8_t(a) | uint8_t(b));
}
constexpr bool HasDomain(Domain mask, Domain d) {
    return (uint8_t(mask) & uint8_t(d)) != 0;
}

enum class ScalarType : uint8_t { Invalid, Bool, Int32, UInt32, Int64, UInt64,
                                  Float16, Float32, Float64 };

// Non-owning, trivially-copyable payload view. Shape is explicit: arrays are
// not collapsed into point samples and matrices retain row-major dimensions.
struct ValueView {
    const void *data = nullptr;
    uint32_t elementCount = 0; // array length; 1 for scalar/vector/matrix
    uint32_t components = 1;
    uint32_t rows = 1;
    uint32_t columns = 1;
    uint32_t strideBytes = 0;
    ScalarType scalar = ScalarType::Invalid;
    bool isArray = false;
};

struct ValueShape {
    ScalarType scalar = ScalarType::Invalid;
    uint32_t elementCount = 1;
    uint32_t components = 1;
    uint32_t rows = 1;
    uint32_t columns = 1;
    bool isArray = false;
};

enum class Variable : uint16_t {
    Invalid, Value, Frame, Time, Index, Count, Seed, DescId,
    PrimIndex, PrimCount, IdLo, IdHi, Id, U, V, FaceId, PatchId,
    P, PRef, RootP, RootPRef, N, NRef, DPdu, DPdv, DPduRef, DPdvRef,
    T, PointIndex, PointCount, CLength, CWidth, Cs, As, CountVariables
};

struct VariableInfo {
    const char *name;
    Variable id;
    ScalarType scalar;
    uint32_t components;
    Domain domains;
};

class Registry {
public:
    static const Registry &Get();
    const VariableInfo *Find(const char *name) const noexcept;
    const VariableInfo *Find(Variable id) const noexcept;
    bool Validate(const char *name, Domain domain, std::string *diagnostic = nullptr) const;
};

struct Context {
    double frame = 0.0;
    double time = 0.0;
    uint32_t index = 0;
    uint32_t count = 1;
    int32_t seed = 0;
    uint32_t descId = 0;
    Domain domain = Domain::Groom;
};

uint32_t DescriptionId(const char *descriptionPath) noexcept;
bool ValidateValueShape(ValueView const &value, ValueShape const &expected,
                        std::string *diagnostic = nullptr);

} // namespace usdGen::expr
#endif
