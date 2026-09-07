#include "usdGenTestUtilsHd/testUtilsHd.h"

#include <cstdio>

namespace usdGenTest {

bool EglContextAvailable() {
    // M0 placeholder. Full EGL pbuffer harness arrives in M1 (PW-12).
    // For now, just check if /dev/dri exists as a heuristic.
    FILE *f = fopen("/dev/dri", "r");
    if (f) { fclose(f); return true; }
    return false;
}

}  // namespace usdGenTest
