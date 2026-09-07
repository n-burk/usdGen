// usdGenTestUtilsHd — Hydra test helpers.
// M0: minimal stub. Full scene fixture and observer utilities arrive in M1.
#ifndef USDGEN_TESTUTILS_HD_H
#define USDGEN_TESTUTILS_HD_H

#include <string>

namespace usdGenTest {

// Returns true if an EGL display can be created (used to gate T2 tests).
bool EglContextAvailable();

}  // namespace usdGenTest

#endif  // USDGEN_TESTUTILS_HD_H
