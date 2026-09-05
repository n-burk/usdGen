#include <SeExpr2/Expression.h>
#include <SeExpr2/ExprFunc.h>
#include <SeExpr2/ExprFuncX.h>
#include <SeExpr2/ExprNode.h>
#include <SeExpr2/VarBlock.h>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#include <cmath>
using namespace SeExpr2;

// A stand-in for XGen's map(): takes a string + returns a varying float
// computed from the current $u,$v (here: read through a pointer into the
// per-thread evaluation context).
struct MapCtx { double u, v; };
static thread_local MapCtx* gCtx = nullptr;
class MapFuncX : public ExprFuncSimple {
public:
    MapFuncX() : ExprFuncSimple(true) {}
    ExprType prep(ExprFuncNode* node, bool, ExprVarEnvBuilder& env) const override {
        bool ok = node->checkArg(0, ExprType().String().Constant(), env);
        return ok ? ExprType().FP(1).Varying() : ExprType().Error();
    }
    ExprFuncNode::Data* evalConstant(const ExprFuncNode*, ArgHandle) const override { return new ExprFuncNode::Data(); }
    void eval(ArgHandle args) override {
        // pretend bilinear lookup: 0.5 + 0.5*sin(u*10)*cos(v*10)
        double u = gCtx ? gCtx->u : 0, v = gCtx ? gCtx->v : 0;
        args.outFp = 0.5 + 0.5 * std::sin(u * 10) * std::cos(v * 10);
    }
} mapFunc;
static ExprFunc mapExprFunc(mapFunc, 1, 1);

struct SimpleVar : public ExprVarRef {
    SimpleVar(int d) : ExprVarRef(ExprType().FP(d).Varying()), val{0,0,0} {}
    double val[3];
    void eval(double* r) override { const int d = type().dim(); for (int i = 0; i < d; ++i) r[i] = val[i]; }
    void eval(const char**) override {}
};
class GroomExpr : public Expression {
public:
    mutable SimpleVar u{1}, v{1}, id{1}, P{3};
    GroomExpr(const std::string& e) : Expression(e, ExprType().FP(1)) {}
    ExprVarRef* resolveVar(const std::string& n) const override {
        if (n=="u") return &u; if (n=="v") return &v; if (n=="id") return &id; if (n=="P") return &P; return nullptr;
    }
    ExprFunc* resolveFunc(const std::string& n) const override {
        if (n=="map") return &mapExprFunc; return nullptr;
    }
};

int main() {
    const char* exprs[] = {
        "$u*$v+1",
        "fit(noise($P*4)+0.5*fbm($P*2,4),0,1.5,0.2,1.0)",
        "map(\"length\")*(0.5+hash($id))",
        "$c = cellnoise($P*5); voronoi($P*3,1,0.5)*$c + smoothstep($u,0.2,0.8)",
    };
    for (const char* e : exprs) {
        auto t0 = std::chrono::steady_clock::now();
        GroomExpr ex(e);
        bool valid = ex.isValid();
        auto t1 = std::chrono::steady_clock::now();
        if (!valid) { printf("INVALID %s : %s\n", e, ex.parseError().c_str()); continue; }
        const int N = 1000000; double acc = 0; MapCtx ctx; gCtx = &ctx;
        auto t2 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; ++i) {
            ex.u.val[0] = (i % 1000) * 0.001; ex.v.val[0] = (i / 1000) * 0.001; ex.id.val[0] = i;
            ex.P.val[0] = ex.u.val[0]*7; ex.P.val[1] = ex.v.val[0]*7; ex.P.val[2] = 0.3;
            ctx.u = ex.u.val[0]; ctx.v = ex.v.val[0];
            acc += ex.evalFP()[0];
        }
        auto t3 = std::chrono::steady_clock::now();
        double prepUs = std::chrono::duration<double, std::micro>(t1 - t0).count();
        double nsPer = std::chrono::duration<double, std::nano>(t3 - t2).count() / N;
        printf("expr=%-70s prep=%.0fus  eval=%.0f ns/eval  threadSafe=%d isVec=%d (acc=%g)\n", e, prepUs, nsPer, (int)ex.isThreadSafe(), (int)ex.isVec(), acc);
    }

    // Multithreaded evaluation of ONE expression via VarBlockCreator with threadSafe blocks.
    {
        VarBlockCreator creator;
        int uOff = creator.registerVariable("u", ExprType().FP(1).Varying());
        int vOff = creator.registerVariable("v", ExprType().FP(1).Varying());
        int POff = creator.registerVariable("P", ExprType().FP(3).Varying());
        Expression ex("fit(noise($P*4)+0.5*fbm($P*2,4),0,1.5,0.2,1.0)*smoothstep($u,0.2,0.8)", ExprType().FP(1));
        ex.setVarBlockCreator(&creator);
        if (!ex.isValid()) { printf("VB INVALID: %s\n", ex.parseError().c_str()); return 1; }
        const int T = 8, N = 1000000;
        std::vector<double> sums(T, 0.0);
        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> th;
        for (int t = 0; t < T; ++t) th.emplace_back([&, t] {
            VarBlock block = creator.create(/*makeThreadSafe=*/true);
            double u, v, P[3];
            block.Pointer(uOff) = &u; block.Pointer(vOff) = &v; block.Pointer(POff) = P;
            double s = 0;
            for (int i = 0; i < N; ++i) { u = (i%1000)*0.001; v = (i/1000)*0.001; P[0]=u*7; P[1]=v*7; P[2]=t; s += ex.evalFP(&block)[0]; }
            sums[t] = s;
        });
        for (auto& x : th) x.join();
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double total = 0; for (double s : sums) total += s;
        printf("VarBlock threadSafe: %d threads x %d evals in %.0f ms => %.0f ns/eval/thread, %.1f M evals/s aggregate (sum=%g)\n", T, N, ms, ms*1e6/N, T*N/ms/1e3, total);
        // Same, single thread, for reference
        VarBlock block = creator.create(true); double u,v,P[3]; block.Pointer(uOff)=&u; block.Pointer(vOff)=&v; block.Pointer(POff)=P;
        auto t2 = std::chrono::steady_clock::now(); double s=0;
        for (int i = 0; i < N; ++i) { u=(i%1000)*0.001; v=(i/1000)*0.001; P[0]=u*7; P[1]=v*7; P[2]=0; s+=ex.evalFP(&block)[0]; }
        auto t3 = std::chrono::steady_clock::now();
        printf("VarBlock single thread: %.0f ns/eval (s=%g)\n", std::chrono::duration<double,std::nano>(t3-t2).count()/N, s);
    }
    return 0;
}
