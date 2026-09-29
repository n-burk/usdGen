// PW-5 / SI-8 pre-work probe.
//
// Question: does OpenUSD's auto-apply-API-schemas mechanism propagate
// UsdGenMaskAPI (declared `apiSchemaAutoApplyTo: [UsdGenOperator]` + the
// Info-level `AutoApplyAPISchemas` block in plugInfo.json) to CODELESS
// derived types such as UsdGenNoise / UsdGenScatter, i.e. does
// UsdSchemaRegistry::FindConcretePrimDefinition(type)->GetPropertyNames()
// already contain the API's properties WITHOUT any explicit
// `apiSchemas`/apply on the prim?
//
// Method: point PXR_PLUGINPATH_NAME at the usdGen build-tree schema
// resources (+ the stock OpenUSD plugin dir), trigger plugin discovery,
// force-load the usdGen schema types, then query the schema registry.
//
//   - UsdGenNoise   (concrete, derives UsdGenOperator)  -> expect mask prop IF propagation works
//   - UsdGenScatter (concrete, derives UsdGenOperator)  -> control: expect same
//   - UsdGenGroom   (concrete, derives UsdGeomImageable)-> control: must NOT get mask prop
//   - UsdGenMaskAPI (singleApplyAPI)                    -> print its declared props
//
// Compile (see docs/prework/PW-5-si8-codeless-autoapply.md):
//   PXR=$USD
//   g++ -std=c++17 -O1 -w probe.cpp -o probe \
//     -I $PXR/include -I /usr/include/python3.12 \
//     -L $PXR/lib \
//     -lusd_usd -lusd_usdGeom -lusd_tf -lusd_sdf -lusd_gf -lusd_arch -lusd_kind \
//     -lusd_vt -lusd_trace -lusd_work -lusd_js -lusd_plug -lusd_ar -lusd_hio \
//     -Wl,-rpath,$PXR/lib
//
// Run:
//   LD_LIBRARY_PATH=$PXR/lib \
//   PXR_PLUGINPATH_NAME="<usdgen-src>/build/usd/usdGenSchema/resources:$PXR/plugin/usd" \
//   ./probe

#include "pxr/pxr.h"
#include "pxr/base/plug/registry.h"
#include "pxr/base/tf/getenv.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/tf/type.h"
#include "pxr/usd/usd/common.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/primDefinition.h"
#include "pxr/usd/usd/schemaRegistry.h"

#include <iostream>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

static const char *const MASK_PROP = "usdGen:mask:opacity";

static const char *KindString(UsdSchemaKind k) {
    switch (k) {
        case UsdSchemaKind::ConcreteTyped: return "concreteTyped";
        case UsdSchemaKind::AbstractTyped: return "abstractTyped";
        case UsdSchemaKind::SingleApplyAPI: return "singleApplyAPI";
        case UsdSchemaKind::MultipleApplyAPI: return "multipleApplyAPI";
        default: return "?";
    }
}

static void PrintProps(const char *label, const UsdPrimDefinition *def) {
    if (!def) {
        std::cout << label << " : <NULL prim definition>\n";
        return;
    }
    std::cout << label << ";";
    std::cout << " appliedAPISchemas=[";
    const TfTokenVector &applied = def->GetAppliedAPISchemas();
    for (size_t i = 0; i < applied.size(); ++i)
        std::cout << (i ? "," : "") << applied[i].GetString();
    std::cout << "]";
    const TfTokenVector &props = def->GetPropertyNames();
    std::cout << "  (" << props.size() << " props):";
    for (size_t i = 0; i < props.size(); ++i) {
        std::cout << (i ? "," : " ") << props[i].GetString();
    }
    std::cout << "\n";
}

static bool ContainsProp(const UsdPrimDefinition *def, const char *prop) {
    if (!def) return false;
    for (const TfToken &p : def->GetPropertyNames()) {
        if (p == TfToken(prop)) return true;
    }
    return false;
}

int main() {
    const char *const kTypes[] = {
        "UsdGenNoise", "UsdGenScatter", "UsdGenLength",
        "UsdGenGroom", "UsdGenOperator", "UsdGenMaskAPI"};

    std::cout << "== plugin path ==" << std::endl;
    std::cout << "PXR_PLUGINPATH_NAME="
              << TfGetenv("PXR_PLUGINPATH_NAME") << std::endl;

    // Belt: explicitly register the build-tree schema resources dir (the
    // environment already puts it on PXR_PLUGINPATH_NAME).
    std::string root = TfGetenv("GEN");
    if (root.empty()) {
        root = TfGetenv("USDGEN_SRC");
    }
    if (root.empty()) {
        root = ".";
    }
    PlugRegistry::GetInstance().RegisterPlugins(
        std::vector<std::string>{
            root + "/build/usd/usdGenSchema/resources"});

    std::cout << "\n== TfType resolution (triggers plugin discovery + load) =="
              << std::endl;
    for (const char *t : kTypes) {
        const TfType ty = TfType::FindByName(t);
        std::cout << "  " << t << " : "
                  << (ty.IsUnknown() ? "UNKNOWN" : ty.GetTypeName().c_str())
                  << "\n";
    }
    // Derivation checks.
    const TfType op = TfType::FindByName("UsdGenOperator");
    for (const char *t : {"UsdGenNoise", "UsdGenScatter", "UsdGenLength", "UsdGenGroom"}) {
        const TfType ty = TfType::FindByName(t);
        std::cout << "  " << t << " IsA(UsdGenOperator) = "
                  << (ty.IsA(op) ? "yes" : "no") << "\n";
    }

    // Only now construct the UsdSchemaRegistry view (TfSingleton) so all
    // codeless TfTypes and SdfSchema classes are registered.
    const UsdSchemaRegistry &reg = UsdSchemaRegistry::GetInstance();

    std::cout << "\n== UsdSchemaRegistry::GetAutoApplyAPISchemas() ==" << std::endl;
    const auto &autoApply = UsdSchemaRegistry::GetAutoApplyAPISchemas();
    if (autoApply.empty()) {
        std::cout << "  <empty - no AutoApplyAPISchemas registered>\n";
    }
    for (const auto &kv : autoApply) {
        std::cout << "  " << kv.first.GetString() << " -> [";
        for (size_t i = 0; i < kv.second.size(); ++i) {
            std::cout << (i ? "," : "") << kv.second[i].GetString();
        }
        std::cout << "]\n";
    }

    std::cout << "\n== FindSchemaInfo (registration evidence) ==" << std::endl;
    for (const char *t : kTypes) {
        const UsdSchemaRegistry::SchemaInfo *info =
            reg.FindSchemaInfo(TfToken(t));
        if (!info) {
            std::cout << "  " << t << " : <no schema info>\n";
            continue;
        }
        std::cout << "  " << t
                 << " : identifier=" << info->identifier.GetString()
                 << " kind=" << KindString(info->kind)
                 << " TfType=" << info->type.GetTypeName()
                 << "\n";
    }

    std::cout << "\n== prim definition property sets ==" << std::endl;
    const UsdPrimDefinition *noiseDef =
        reg.FindConcretePrimDefinition(TfToken("UsdGenNoise"));
    const UsdPrimDefinition *scatterDef =
        reg.FindConcretePrimDefinition(TfToken("UsdGenScatter"));
    const UsdPrimDefinition *lengthDef =
        reg.FindConcretePrimDefinition(TfToken("UsdGenLength"));
    const UsdPrimDefinition *groomDef =
        reg.FindConcretePrimDefinition(TfToken("UsdGenGroom"));
    PrintProps("UsdGenNoise   ", noiseDef);
    PrintProps("UsdGenScatter ", scatterDef);
    PrintProps("UsdGenLength  ", lengthDef);
    PrintProps("UsdGenGroom   ", groomDef);
    PrintProps("UsdGenOperator (abstract)",
               reg.FindAbstractPrimDefinition(TfToken("UsdGenOperator")));
    PrintProps("UsdGenMaskAPI  (applied API def)",
               reg.FindAppliedAPIPrimDefinition(TfToken("UsdGenMaskAPI")));

    std::cout << "\n== stage-level check: applied schemas on a live prim ==" << std::endl;
    {
        UsdStageRefPtr stage = UsdStage::CreateInMemory();
        UsdPrim noise = stage->DefinePrim(SdfPath("/Noise"), TfToken("UsdGenNoise"));
        UsdPrim scatter = stage->DefinePrim(SdfPath("/Scatter"), TfToken("UsdGenScatter"));
        UsdPrim length = stage->DefinePrim(SdfPath("/Length"), TfToken("UsdGenLength"));
        UsdPrim groom = stage->DefinePrim(SdfPath("/Groom"), TfToken("UsdGenGroom"));
        auto printApplied = [](const char *label, const UsdPrim &p) {
            std::cout << "  " << label << " applied schemas: [";
            TfTokenVector s = p.GetAppliedSchemas();
            for (size_t i = 0; i < s.size(); ++i)
                std::cout << (i ? "," : "") << s[i].GetString();
            std::cout << "]\n";
        };
        printApplied("UsdGenNoise  ", noise);
        printApplied("UsdGenScatter", scatter);
        printApplied("UsdGenLength ", length);
        printApplied("UsdGenGroom  ", groom);
    }

    std::cout << "\n== verdict: does the prim definition already carry '"
                 << MASK_PROP << "'? ==" << std::endl;
    const bool noise = ContainsProp(noiseDef, MASK_PROP);
    const bool scatter = ContainsProp(scatterDef, MASK_PROP);
    const bool length = ContainsProp(lengthDef, MASK_PROP);
    const bool groom = ContainsProp(groomDef, MASK_PROP);
    std::cout << "  UsdGenNoise   contains mask prop : "
              << (noise ? "YES" : "no") << "\n";
    std::cout << "  UsdGenScatter contains mask prop : "
              << (scatter ? "YES" : "no") << "\n";
    std::cout << "  UsdGenLength  contains mask prop : "
              << (length ? "YES" : "no") << "\n";
    std::cout << "  UsdGenGroom   contains mask prop : "
              << (groom ? "YES (unexpected)" : "no (expected)") << "\n";

    std::cout << "\n== summary ==" << std::endl;
    if (noise && scatter && length && !groom) {
        std::cout << "PASS: auto-apply propagated to every codeless UsdGenOperator-"
                     "derived type; control (UsdGenGroom) correctly excluded.\n";
    } else if (noise || scatter || length) {
        std::cout << "PARTIAL: some UsdGenOperator-derived types saw the mask "
                     "prop; see detail above.\n";
    } else {
        std::cout << "FAIL: codeless derived types do NOT inherit auto-applied "
                     "UsdGenMaskAPI - the tool must keep applying it explicitly.\n";
    }

    return 0;
}
