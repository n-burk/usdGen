// Process-exit lifetime regression.  The holder is constructed before the
// scene service singleton and intentionally keeps the index alive through
// return from main, exercising reverse static-destruction ordering.
#include "usdGenImaging/groomSceneIndexPlugin.h"

#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/retainedSceneIndex.h"

#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

struct ExitHolder {
    HdSceneIndexBaseRefPtr input;
    HdSceneIndexBaseRefPtr index;
};

// Must precede first construction of UsdGenSceneService in main.
ExitHolder g_exitHolder;

} // namespace

int main()
{
    auto input = HdRetainedSceneIndex::New();
    const SdfPath root("/__sceneExitGroom");
    input->AddPrims({{root, TfToken("UsdGenGroom"),
                      HdRetainedContainerDataSource::New()}});

    auto index = UsdGenGroomSceneIndex::New(input, 5801);
    auto *owner = dynamic_cast<UsdGenGroomSceneIndex *>(index.operator->());
    if (!owner) {
        std::fprintf(stderr, "scene exit owner cast failed\n");
        return 2;
    }
    owner->Synchronize();
    if (index->GetPrim(root).primType != TfToken("UsdGenGroom")) {
        std::fprintf(stderr, "scene exit groom was not populated\n");
        return 3;
    }

    // Deliberately do not reset either reference.  g_exitHolder's destructor
    // runs after function-local static services and must still retire safely.
    g_exitHolder.input = input;
    g_exitHolder.index = index;
    std::printf("testUsdGenSceneExit: PASS (retained index through exit)\n");
    return 0;
}
