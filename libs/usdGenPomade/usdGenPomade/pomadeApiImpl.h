// usdGenPomade — the private context layouts behind the C ABI handles.
//
// pomadeApi.cpp defines these in its own anonymous namespace (it predates the
// split into several ABI translation units). This header carries the SAME
// layouts so a second ABI TU — pomadeApiStage.cpp — can reinterpret the same
// opaque handles without editing that file. The two definitions must stay in
// lockstep: a field added there has to be added here, in the same order.
#ifndef USDGEN_POMADE_API_IMPL_H
#define USDGEN_POMADE_API_IMPL_H

#include "usdGenPomade/pomadeBake.h"
#include "usdGenPomade/pomadeCommit.h"
#include "usdGenPomade/pomadeModel.h"
#include "usdGenPomade/pomadeTransport.h"

#include <memory>
#include <string>

namespace usdGenPomade {

struct PomadeModelContextImpl {
    PomadeModel model;
    PomadePinnedStaging positions;
    PomadePinnedStaging normals;
};

struct PomadeCommitterContextImpl {
    std::unique_ptr<PomadeCommitter> committer;
};

struct PomadeBakeContextImpl {
    PomadeModel *model = nullptr;  // non-owning (outlives us)
    std::unique_ptr<PomadeBakeWorker> worker;
    std::string outDir;
    std::string baseName = "regionMap";
    int resOverride = -1;
    int levelCount = 1;
};

}  // namespace usdGenPomade

#endif  // USDGEN_POMADE_API_IMPL_H
