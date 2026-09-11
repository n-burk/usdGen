#ifndef USDGEN_EXPRESSIONS_FRONTEND_H
#define USDGEN_EXPRESSIONS_FRONTEND_H
#include "usdGen/expressions/context.h"
#include "usdGen/expressions/ir.h"
#include <memory>
#include <string>
#include <vector>
namespace usdGen::expr {
// SeExpr2 performs numeric math in FP; destination integer/bool conversion is
// deferred to the GPU boundary. int64/uint64 results are exact only through 2^53.
struct FrontendOptions { Domain domain = Domain::Groom; ScalarType destination = ScalarType::Float32; uint32_t components = 1; };
class Program {
public:
    Program(); ~Program(); Program(Program&&) noexcept; Program& operator=(Program&&) noexcept;
    Program(Program const&) = delete; Program& operator=(Program const&) = delete;
    bool Valid() const noexcept; std::string const &Source() const noexcept;
    IRProgram const &IR() const noexcept;
private:
    struct Impl; std::unique_ptr<Impl> _impl; explicit Program(std::unique_ptr<Impl>); friend class Frontend;
};
struct CompileResult { bool ok=false; Program program; std::vector<std::string> diagnostics; };
class Frontend { public: static CompileResult Compile(std::string const&, FrontendOptions const& = {}); };
}
#endif
