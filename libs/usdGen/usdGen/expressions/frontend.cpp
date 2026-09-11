#include "usdGen/expressions/frontend.h"

// This translation unit is compiled with SeExpr2=UsdGenSeExprFrontend so the
// complete vendored parser is private and cannot collide with the legacy noise
// subset's symbols.
#include <Expression.h>
#include <ExprNode.h>
#include <set>

namespace usdGen::expr {
namespace {
class Var final : public UsdGenSeExprFrontend::ExprVarRef {
public:
    explicit Var(int dim) : ExprVarRef(UsdGenSeExprFrontend::ExprType().FP(dim).Varying()) {}
    void eval(double *out) override { for (int i=0;i<type().dim();++i) out[i]=0.0; }
    void eval(const char **) override {}
};
class CheckedExpression final : public UsdGenSeExprFrontend::Expression {
public:
    CheckedExpression(std::string const &s, int dim, Domain domain) : Expression(s, UsdGenSeExprFrontend::ExprType().FP(dim), Expression::UseInterpreter), _domain(domain), _valueDim(dim) {}
    UsdGenSeExprFrontend::ExprVarRef *resolveVar(const std::string &name) const override {
        std::string canonical = name;
        if (canonical.empty() || canonical[0] != '$') canonical.insert(canonical.begin(), '$');
        auto info = Registry::Get().Find(canonical.c_str());
        if (!info) return nullptr;
        if (!Registry::Get().Validate(canonical.c_str(), _domain)) return nullptr;
        const int dim = info->components ? int(info->components) : (info->id == Variable::Value ? _valueDim : 1);
        _vars.emplace_back(new Var(dim)); return _vars.back().get();
    }
private:
    mutable std::vector<std::unique_ptr<Var>> _vars;
    Domain _domain;
    int _valueDim;
};
constexpr const char *kPure[] = {"abs","acos","asin","atan","atan2","ceil","clamp","cos","cosh","exp","floor","fmod","log","log10","max","min","pow","sin","sinh","sqrt","tan","tanh","fit","smoothstep","noise","snoise","fbm","hash","mix","dot","length","norm","cross","rotate"};
bool Pure(char const *n) { for (auto p:kPure) if (std::string(n)==p) return true; return false; }
bool Walk(UsdGenSeExprFrontend::ExprNode const *node, std::vector<std::string> &errors)
{
    if (!node) return true;
    if (auto fn = dynamic_cast<UsdGenSeExprFrontend::ExprFuncNode const *>(node)) {
        if (!Pure(fn->name())) { errors.push_back(std::string("unsupported expression function '") + fn->name() + "' at " + std::to_string(fn->startPos())); return false; }
    }
    if (dynamic_cast<UsdGenSeExprFrontend::ExprStrNode const *>(node)) { errors.push_back("string literals are not supported in numeric expressions at " + std::to_string(node->startPos())); return false; }
    if (dynamic_cast<UsdGenSeExprFrontend::ExprAssignNode const *>(node) || dynamic_cast<UsdGenSeExprFrontend::ExprLocalFunctionNode const *>(node)) { errors.push_back("local assignments/functions are not supported in CUDA expressions at " + std::to_string(node->startPos())); return false; }
    bool ok=true; for (int i=0;i<node->numChildren();++i) ok = Walk(node->child(i),errors) && ok; return ok;
}
}
struct Program::Impl { std::string source; bool valid=false; IRProgram ir; };
Program::Program() : _impl(new Impl) {}
Program::~Program() = default;
Program::Program(Program&&) noexcept = default;
Program& Program::operator=(Program&&) noexcept = default;
Program::Program(std::unique_ptr<Impl> p) : _impl(std::move(p)) {}
bool Program::Valid() const noexcept { return _impl && _impl->valid; }
std::string const &Program::Source() const noexcept { static std::string empty; return _impl ? _impl->source : empty; }
IRProgram const &Program::IR() const noexcept { static IRProgram empty; return _impl ? _impl->ir : empty; }

uint16_t Lower(UsdGenSeExprFrontend::ExprNode const *n, IRProgram &ir, std::vector<std::string> &errors, uint8_t component=0) {
    if (!n || ir.instructions.size() >= 4096 || ir.registerCount >= 256) { errors.push_back("expression IR limit exceeded"); return 0; }
    auto emit=[&](IRInstruction x){ x.dst=ir.registerCount++; ir.instructions.push_back(x); return uint16_t(x.dst); };
    if (auto x=dynamic_cast<UsdGenSeExprFrontend::ExprNumNode const*>(n)) { IRInstruction q; q.op=IROp::Const;q.immediate=x->value(); return emit(q); }
    if (auto x=dynamic_cast<UsdGenSeExprFrontend::ExprVarNode const*>(n)) { std::string name=x->name(); if(name.empty()||name[0]!='$')name.insert(name.begin(),'$'); auto v=Registry::Get().Find(name.c_str()); if(!v){errors.push_back("unknown variable in IR");return 0;} IRInstruction q; q.op=IROp::LoadVariable;q.variable=v->id;q.component=v->components == 1 ? 0 : component; return emit(q); }
    if (dynamic_cast<UsdGenSeExprFrontend::ExprModuleNode const*>(n) || dynamic_cast<UsdGenSeExprFrontend::ExprBlockNode const*>(n)) { if(n->numChildren()!=1){errors.push_back("multi-statement expression IR is unsupported");return 0;} return Lower(n->child(0),ir,errors,component); }
    if (auto x=dynamic_cast<UsdGenSeExprFrontend::ExprVecNode const*>(n)) { if(component>=x->numChildren()){errors.push_back("vector component out of range");return 0;} return Lower(x->child(component),ir,errors,0); }
    if (auto x=dynamic_cast<UsdGenSeExprFrontend::ExprSubscriptNode const*>(n)) { auto idx=dynamic_cast<UsdGenSeExprFrontend::ExprNumNode const*>(x->child(1)); if(!idx || idx->value()<0 || idx->value()>3 || idx->value()!=int(idx->value())) { errors.push_back("only constant vector indexing is supported"); return 0; } return Lower(x->child(0),ir,errors,static_cast<uint8_t>(idx->value())); }
    if (auto x=dynamic_cast<UsdGenSeExprFrontend::ExprFuncNode const*>(n)) {
        const std::string name=x->name(); int want=(name=="clamp"?3:(name=="abs"||name=="sin"||name=="cos"?1:2));
        if (name!="abs" && name!="sin" && name!="cos" && name!="pow" && name!="min" && name!="max" && name!="clamp") { errors.push_back("unsupported function in IR: "+name); return 0; }
        if (x->numChildren()!=want) { errors.push_back("invalid arity for function: "+name); return 0; }
        if (want==1) { auto a=Lower(x->child(0),ir,errors,component); IRInstruction q; q.a=a; q.op=name=="abs"?IROp::Abs:name=="sin"?IROp::Sin:IROp::Cos; return emit(q); }
        auto a=Lower(x->child(0),ir,errors,component), b=Lower(x->child(1),ir,errors,component); IRInstruction q; q.a=a;q.b=b; q.op=name=="pow"?IROp::Pow:name=="min"?IROp::Min:name=="max"?IROp::Max:IROp::Clamp;
        if (want==3) q.c=Lower(x->child(2),ir,errors,component); return emit(q);
    }
    if (auto x=dynamic_cast<UsdGenSeExprFrontend::ExprUnaryOpNode const*>(n)) { auto a=Lower(x->child(0),ir,errors,component); if(x->_op=='+') return a; if(x->_op=='-'){IRInstruction q; q.op=IROp::Neg;q.a=a; return emit(q);} if(x->_op=='!'){IRInstruction z;z.op=IROp::Const;z.immediate=0;auto zr=emit(z);IRInstruction q;q.op=IROp::Compare;q.a=a;q.b=zr;q.compare='=';return emit(q);} errors.push_back("unsupported unary operator in IR"); return 0; }
    if (auto x=dynamic_cast<UsdGenSeExprFrontend::ExprBinaryOpNode const*>(n)) { auto a=Lower(x->child(0),ir,errors,component),b=Lower(x->child(1),ir,errors,component); IRInstruction q; q.a=a;q.b=b; q.op=x->_op=='+'?IROp::Add:x->_op=='-'?IROp::Sub:x->_op=='*'?IROp::Mul:x->_op=='/'?IROp::Div:IROp::Const; if(q.op==IROp::Const)errors.push_back("unsupported binary operator in IR"); return emit(q); }
    if (auto x=dynamic_cast<UsdGenSeExprFrontend::ExprCompareNode const*>(n)) { auto a=Lower(x->child(0),ir,errors,component),b=Lower(x->child(1),ir,errors,component); IRInstruction q; q.op=IROp::Compare;q.a=a;q.b=b;q.compare=x->_op; return emit(q); }
    if (auto x=dynamic_cast<UsdGenSeExprFrontend::ExprCompareEqNode const*>(n)) { auto a=Lower(x->child(0),ir,errors,component),b=Lower(x->child(1),ir,errors,component); IRInstruction q; q.op=IROp::Compare;q.a=a;q.b=b;q.compare=x->_op; return emit(q); }
    if (dynamic_cast<UsdGenSeExprFrontend::ExprCondNode const*>(n)) { auto c=Lower(n->child(0),ir,errors,component),a=Lower(n->child(1),ir,errors,component),b=Lower(n->child(2),ir,errors,component); IRInstruction q; q.op=IROp::Select;q.a=c;q.b=a;q.c=b;ir.hasLazyBranches=true;return emit(q); }
    errors.push_back("unsupported AST node in CUDA expression IR"); return 0;
}

CompileResult Frontend::Compile(std::string const &source, FrontendOptions const &opt)
{
    CompileResult out; out.program = Program(std::unique_ptr<Program::Impl>(new Program::Impl));
    out.program._impl->source = source;
    if (opt.domain != Domain::Groom && opt.domain != Domain::Primitive && opt.domain != Domain::Point) { out.diagnostics.push_back("invalid evaluation domain"); return out; }
    switch (opt.destination) {
    case ScalarType::Bool: case ScalarType::Int32: case ScalarType::UInt32:
    case ScalarType::Int64: case ScalarType::UInt64: case ScalarType::Float16:
    case ScalarType::Float32: case ScalarType::Float64: break;
    default: out.diagnostics.push_back("invalid expression destination type"); return out;
    }
    if (opt.components == 0 || opt.components > 4) { out.diagnostics.push_back("unsupported vector dimension"); return out; }
    CheckedExpression expr(source, int(opt.components), opt.domain);
    const auto *tree = expr.parseTree();
    if (!tree) { out.diagnostics.push_back(expr.parseError().empty() ? "expression parse failed" : expr.parseError()); return out; }
    // Parse first, then walk the actual AST before prep/binding. This catches
    // disallowed builtins even in branches and ignores comment/string text.
    if (!Walk(tree, out.diagnostics)) return out;
    if (!expr.isValid()) { out.diagnostics.push_back(expr.parseError()); for (auto const &e:expr.getErrors()) out.diagnostics.push_back(e.error); return out; }
    for (auto const &name : {"rand","file","map","system","exec"}) if (expr.usesFunc(name)) { out.diagnostics.push_back(std::string("unsupported non-pure expression function: ")+name); return out; }
    out.program._impl->ir.result = 0;
    out.program._impl->ir.valueComponents = static_cast<uint8_t>(opt.components);
    out.program._impl->ir.outputCount = opt.components > 1 ? static_cast<uint8_t>(opt.components) : 0;
    for (uint8_t c=0; c<opt.components; ++c) out.program._impl->ir.output[c] = Lower(tree, out.program._impl->ir, out.diagnostics, c);
    out.program._impl->ir.result = out.program._impl->ir.output[0];
    out.program._impl->ir.registerCount = static_cast<uint16_t>(out.program._impl->ir.instructions.size());
    if (!out.diagnostics.empty()) return out;
    out.program._impl->valid = true; out.ok = true; return out;
}
}
