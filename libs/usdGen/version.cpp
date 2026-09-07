#include "usdGen/usdGen.h"

#include "pxr/pxr.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/value.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

std::string GetVersionString() {
    // Exercise tf/vt so the link rule (B-1) observes the permitted base libs.
    const TfToken v("usdGen");
    const VtValue marker(v);
    (void)marker;
    return "usdGen 0.1.0 (M0)";
}

}  // namespace usdGen
