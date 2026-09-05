#include <SeExpr2/Expression.h>
#include <SeExpr2/ExprFunc.h>
#include <SeExpr2/ExprFuncX.h>
#include <SeExpr2/ExprNode.h>
#include <cstdio>
using namespace SeExpr2;
struct SimpleVar : public ExprVarRef {
    SimpleVar(int d) : ExprVarRef(ExprType().FP(d).Varying()), val{0,0,0} {}
    double val[3];
    void eval(double* r) override { const int d = type().dim(); for (int i = 0; i < d; ++i) r[i] = val[i]; }
    void eval(const char**) override {}
};
class E : public Expression {
public:
    mutable SimpleVar u{1}, P{3};
    E(const std::string& e) : Expression(e, ExprType().FP(1)) {}
    ExprVarRef* resolveVar(const std::string& n) const override { if (n=="u") return &u; if (n=="P") return &P; return nullptr; }
};
int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char* e = argc > 1 ? argv[1] : "$u*2";
    printf("A: constructing\n");
    { E ex(e);
      printf("B: isValid=%d\n", (int)ex.isValid());
      if (!ex.isValid()) printf("err: %s\n", ex.parseError().c_str());
      ex.u.val[0]=0.5; ex.P.val[0]=1; ex.P.val[1]=2; ex.P.val[2]=3;
      printf("C: eval=%g\n", ex.evalFP()[0]);
      printf("D: eval2=%g\n", ex.evalFP()[0]);
    }
    printf("E: destroyed ok\n");
    return 0;
}
