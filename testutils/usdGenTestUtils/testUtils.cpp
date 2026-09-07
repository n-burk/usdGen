#include "usdGenTestUtils/testUtils.h"

#include <cstdio>

namespace usdGenTest {

int Check(bool ok, const char *what) {
    if (ok) {
        std::printf("ok:   %s\n", what);
    } else {
        std::printf("FAIL: %s\n", what);
    }
    return ok ? 1 : 0;
}

int ReportChecks(const char *suite, int pass, int fail) {
    std::printf("[%s] pass=%d fail=%d\n", suite, pass, fail);
    return fail == 0 ? 0 : 1;
}

}  // namespace usdGenTest
