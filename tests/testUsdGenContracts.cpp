// testUsdGenContracts — gate SI-* registry contract (docs/freezes/C1.md).
//
// C1 is FROZEN at M1: the usdGen prim type names, property names, value
// types, and fallback defaults pinned in docs/freezes/C1.md (authoritative;
// supersedes the abstract/API summary in docs/m1/interfaces.md §7) must match
// the live UsdSchemaRegistry / UsdPrimDefinition as loaded from
// plugin/usdGenSchema/resources/generatedSchema.usda.
//
// Uniform encoding note: the usda `uniform` qualifier surfaces as the
// attribute's SdfVariability (SdfVariabilityUniform). In USD 26.08 neither
// the value-type token ("int", "token", …) nor a "uniform" metadata key
// carries it on the loaded definition — verified live. (02 §2.2/§2.3, C1.md
// Uniform column.)
//
// Any FAIL line here is a C1 break: rename/retyping/default change requires
// a new contract version, not an M1 edit (02 §8.1/§8.5). No skip path: this
// test always asserts.

#include "pxr/pxr.h"
#include "pxr/base/tf/token.h"
#include "pxr/usd/sdf/types.h"
#include "pxr/usd/usd/primDefinition.h"
#include "pxr/usd/usd/schemaRegistry.h"

#include <cstdio>
#include <sstream>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

int g_failures = 0;

void Check(bool ok, std::string const &what)
{
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    } else {
        std::printf("ok:   %s\n", what.c_str());
    }
}

// Render an attribute's fallback default as a stable string for comparison:
// floats via %g (1.0 -> "1", 0.01 -> "0.01"), ints via ostream, bool ->
// "1"/"0", string/token -> contents. |ok| reports whether any fallback was
// readable at all (so "missing default" fails, distinct from "wrong value").
std::string FallbackToString(UsdPrimDefinition::Attribute const &attr,
                             bool *ok)
{
    float f;
    double d;
    std::int64_t i;
    int n;
    bool b;
    std::string s;
    TfToken t;
    char buf[64];
    if (attr.GetFallbackValue(&f)) {
        std::snprintf(buf, sizeof buf, "%g", static_cast<double>(f));
        *ok = true; return buf;
    }
    if (attr.GetFallbackValue(&d)) {
        std::snprintf(buf, sizeof buf, "%g", d);
        *ok = true; return buf;
    }
    if (attr.GetFallbackValue(&i)) {
        std::ostringstream os; os << i;
        *ok = true; return os.str();
    }
    if (attr.GetFallbackValue(&n)) {
        std::ostringstream os; os << n;
        *ok = true; return os.str();
    }
    if (attr.GetFallbackValue(&b)) {
        *ok = true; return b ? "1" : "0";
    }
    if (attr.GetFallbackValue(&s)) { *ok = true; return s; }
    if (attr.GetFallbackValue(&t)) { *ok = true; return t.GetString(); }
    *ok = false;
    return std::string();
}

// Assert |name| is an attribute of |pd| with value-type token |typeName| and
// fallback default stringifying to |defaultText|.
void CheckAttr(UsdPrimDefinition const *pd,
               std::string const &name,
               std::string const &typeName,
               std::string const &defaultText,
               bool uniformExpected = false)
{
    const TfToken prop(name);
    UsdPrimDefinition::Attribute attr = pd->GetAttributeDefinition(prop);
    if (!attr) {
        Check(false, name + ": not an attribute of prim definition");
        return;
    }
    Check(attr.GetTypeNameToken().GetString() == typeName,
          name + ": value-type token is \"" +
              attr.GetTypeNameToken().GetString() + "\", contract pins \"" +
              typeName + "\"");
    // Uniformity == SdfVariability (verified live: the metadata-key probe
    // returns nothing; GetVariability() is the 26.08 accessor).
    const SdfVariability var = attr.GetVariability();
    Check(var == (uniformExpected ? SdfVariabilityUniform
                                  : SdfVariabilityVarying),
          name + std::string(uniformExpected ? ": expected uniform, but "
                                            : ": expected non-uniform, but ") +
              "variability=" +
              (var == SdfVariabilityUniform ? "uniform" : "varying"));
    bool haveFallback = false;
    std::string got = FallbackToString(attr, &haveFallback);
    Check(haveFallback && got == defaultText,
          name + ": fallback default is " +
              (haveFallback ? "\"" + got + "\"" : "<none>") +
              ", contract pins \"" + defaultText + "\"");
}

// Assert |name| is a relationship of |pd| (and not also an attribute).
void CheckRel(UsdPrimDefinition const *pd, std::string const &name)
{
    const TfToken prop(name);
    UsdPrimDefinition::Relationship rel = pd->GetRelationshipDefinition(prop);
    Check(static_cast<bool>(rel), name + ": not a relationship");
    if (static_cast<bool>(pd->GetAttributeDefinition(prop))) {
        Check(false, name + ": defined as attribute as well as relationship");
    }
}

void CheckConcrete(std::string const &typeName)
{
    Check(UsdSchemaRegistry::IsConcrete(TfToken(typeName)),
          typeName + ": not registered concrete");
}

void CheckAbstract(std::string const &typeName)
{
    Check(UsdSchemaRegistry::IsAbstract(TfToken(typeName)),
          typeName + ": not registered abstract");
}

void CheckSingleApplyAPI(std::string const &typeName)
{
    const TfToken tok(typeName);
    Check(UsdSchemaRegistry::IsAppliedAPISchema(tok) &&
              !UsdSchemaRegistry::IsMultipleApplyAPISchema(tok),
          typeName + ": not a single-apply API schema");
}

} // namespace

int main()
{
    // C1 §1 — frozen prim type names (35 concrete + 5 abstract + 4 API).
    const char *kConcrete[] = {
        "UsdGenGroom", "UsdGenDescription", "UsdGenGuideSet", "UsdGenScatter",
        "UsdGenGrow", "UsdGenGuideInterpolate", "UsdGenCurveSource",
        "UsdGenClump", "UsdGenNoise", "UsdGenLength", "UsdGenWidth",
        "UsdGenDirection", "UsdGenSmooth", "UsdGenResample", "UsdGenScale",
        "UsdGenCurl", "UsdGenBend", "UsdGenStraighten", "UsdGenDisplace",
        "UsdGenWave", "UsdGenPart", "UsdGenExprOp", "UsdGenSculptLayer",
        "UsdGenDeform", "UsdGenCollide", "UsdGenWind", "UsdGenFreeze",
        "UsdGenInstance", "UsdGenImageMap", "UsdGenPtexMap", "UsdGenExprMap",
        "UsdGenPaintMap", "UsdGenNoiseMap", "UsdGenCombineMap",
        "UsdGenGuideProximityMap",
    };
    for (const char *name : kConcrete)
        CheckConcrete(name);

    for (const char *name :
         { "UsdGenOperator", "UsdGenGenerator", "UsdGenStyler",
           "UsdGenDeformer", "UsdGenMap" })
        CheckAbstract(name);

    for (const char *name : { "UsdGenMaskAPI", "UsdGenLookAPI",
                              "UsdGenRestAPI", "UsdGenCurveAPI" })
        CheckSingleApplyAPI(name);

    // C1 §2 — UsdGenGroom property rows.
    if (UsdPrimDefinition const *groom = UsdSchemaRegistry::GetInstance()
            .FindConcretePrimDefinition(TfToken("UsdGenGroom"))) {
        CheckRel(groom, "usdGen:surface");
        CheckAttr(groom, "usdGen:densityScale", "float", "1");
        CheckAttr(groom, "usdGen:renderDensityScale", "float", "1");
        CheckAttr(groom, "usdGen:schemaVersion", "int", "1", /*uniform*/ true);
        CheckAttr(groom, "usdGen:sessionId", "string", "", /*uniform*/ true);
        CheckAttr(groom, "usdGen:label", "string", "");
    } else {
        Check(false, "UsdGenGroom: no concrete prim definition (schema not loaded?)");
    }

    // C1 §2 — UsdGenDescription property rows.
    if (UsdPrimDefinition const *desc = UsdSchemaRegistry::GetInstance()
            .FindConcretePrimDefinition(TfToken("UsdGenDescription"))) {
        CheckRel(desc, "usdGen:surface");
        CheckRel(desc, "usdGen:terminal");
        CheckRel(desc, "usdGen:guides");
        CheckAttr(desc, "usdGen:tileTarget", "int", "64", /*uniform*/ true);
        CheckAttr(desc, "usdGen:densityScale", "float", "1");
        CheckAttr(desc, "usdGen:renderDensityScale", "float", "1");
        CheckAttr(desc, "usdGen:curve:basis", "token", "bspline", /*uniform*/ true);
        CheckAttr(desc, "usdGen:width:default", "float", "0.01");
        CheckAttr(desc, "usdGen:motion:mode", "token", "single", /*uniform*/ true);
        CheckAttr(desc, "usdGen:motion:sampleCount", "int", "3", /*uniform*/ true);
        CheckAttr(desc, "usdGen:motion:forwardSurfaceSamples", "bool", "0", /*uniform*/ true);
        CheckAttr(desc, "usdGen:pickTarget", "token", "description", /*uniform*/ true);
        CheckAttr(desc, "usdGen:label", "string", "");
    } else {
        Check(false, "UsdGenDescription: no concrete prim definition (schema not loaded?)");
    }

    if (g_failures != 0) {
        std::printf("%d contract FAILURES — C1 break (docs/freezes/C1.md)\n",
                    g_failures);
        return 1;
    }
    std::printf("C1 registry contract holds\n");
    return 0;
}
