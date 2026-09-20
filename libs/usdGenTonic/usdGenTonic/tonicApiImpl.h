// usdGenTonic — the private context layouts behind the C ABI handles.
//
// tonicApi.cpp defines these in its own anonymous namespace (it predates the
// split into several ABI translation units). This header carries the SAME
// layouts so a second ABI TU — tonicApiStage.cpp — can reinterpret the same
// opaque handles without editing that file. The two definitions must stay in
// lockstep: a field added there has to be added here, in the same order.
#ifndef USDGEN_TONIC_API_IMPL_H
#define USDGEN_TONIC_API_IMPL_H

#include "usdGenTonic/tonicBake.h"
#include "usdGenTonic/tonicCommit.h"
#include "usdGenTonic/tonicModel.h"
#include "usdGenTonic/tonicTransport.h"

#include <memory>
#include <string>

namespace usdGenTonic {

struct TonicModelContextImpl {
    TonicModel model;
    TonicPinnedStaging positions;
    TonicPinnedStaging normals;
};

struct TonicCommitterContextImpl {
    std::unique_ptr<TonicCommitter> committer;
};

struct TonicBakeContextImpl {
    TonicModel *model = nullptr;  // non-owning (outlives us)
    std::unique_ptr<TonicBakeWorker> worker;
    std::string outDir;
    std::string baseName = "regionMap";
    int resOverride = -1;
    int levelCount = 1;
};

}  // namespace usdGenTonic

#endif  // USDGEN_TONIC_API_IMPL_H
