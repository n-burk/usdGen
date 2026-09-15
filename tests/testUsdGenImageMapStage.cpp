#include "usdGenImaging/usdGenGraphDescBuilderStage.h"
#include "usdGenImaging/imageMapCache.h"
#include "usdGen/imagePayload.h"

#include "pxr/pxr.h"
#include "pxr/base/tf/token.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/sdf/types.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/relationship.h"
#include "pxr/usd/usd/stage.h"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdGen;

namespace {
int failures = 0;
void Check(bool value, char const *message)
{
    if (!value) { ++failures; std::printf("FAIL: %s\n", message); }
    else std::printf("ok:   %s\n", message);
}

// Ar hands back a resolved path in the host's native separator form, so a
// path that round-trips through the resolver is only expected to match the
// input up to separators ('\' on Windows, '/' everywhere else). Compare the
// normalized forms rather than the raw strings.
std::string NormalizeSeparators(std::string path)
{
    for (char &c : path) {
        if (c == '\\') c = '/';
    }
    return path;
}

void CheckPathsEqual(std::string const &actual, std::string const &expected,
                     char const *message)
{
    bool const equal =
        NormalizeSeparators(actual) == NormalizeSeparators(expected);
    Check(equal, message);
    if (!equal) {
        std::printf("      expected: %s\n      actual:   %s\n",
                    expected.c_str(), actual.c_str());
    }
}
}

int main()
{
    std::string const imagePath =
        std::string(USDGEN_SOURCE_DIR) + "/tests/golden/stormLook_A.png";
    UsdStageRefPtr stage = UsdStage::CreateInMemory("imageMapStage");
    stage->DefinePrim(SdfPath("/Groom"), TfToken("Scope"));
    stage->DefinePrim(SdfPath("/Groom/Description"), TfToken("UsdGenDescription"));
    stage->DefinePrim(SdfPath("/Groom/Description/Ops"), TfToken("Scope"));
    UsdPrim width = stage->DefinePrim(
        SdfPath("/Groom/Description/Ops/Width"), TfToken("UsdGenWidth"));
    UsdPrim map = stage->DefinePrim(
        SdfPath("/Groom/Map"), TfToken("UsdGenImageMap"));
    width.CreateRelationship(TfToken("usdGen:mask:source"))
        .SetTargets({map.GetPath()});
    map.CreateAttribute(TfToken("usdGen:map:file"), SdfValueTypeNames->Asset)
        .Set(SdfAssetPath(imagePath));

    usdGenImaging::UsdGenGraphDescBuildOptions options;
    UsdGenGraphDesc desc = usdGenImaging::BuildGraphDescFromStage(
        stage, SdfPath("/Groom/Description"), options);
    Check(desc.maps.size() == 1, "stage builder pools one ImageMap descriptor");
    if (desc.maps.empty()) return 1;
    if (!desc.maps.empty()) {
        CheckPathsEqual(desc.maps[0].resolvedAssetPath, imagePath,
                        "stage builder preserves resolved ImageMap asset path");
        Check(desc.maps[0].imagePayload && desc.maps[0].imagePayload->IsValid(),
              "stage builder decodes ImageMap into immutable payload");
        if (desc.maps[0].imagePayload) {
            Check(desc.maps[0].imagePayload->Width() == 256 &&
                      desc.maps[0].imagePayload->Height() == 256 &&
                      desc.maps[0].imagePayload->Channels() == 4,
                  "decoded ImageMap payload preserves native dimensions/channels");
        }
    }

    UsdGenGraphDesc shared = usdGenImaging::BuildGraphDescFromStage(
        stage, SdfPath("/Groom/Description"), options);
    Check(shared.maps.size() == 1 && desc.maps.size() == 1 &&
              shared.maps[0].imagePayload == desc.maps[0].imagePayload &&
              shared.maps[0].textureGeneration == desc.maps[0].textureGeneration,
          "same image generation reuses one immutable COW payload");
    auto const oldPayload = desc.maps[0].imagePayload;
    float const oldFirst = oldPayload ? oldPayload->Data()[0] : 0.0f;
    uint64_t const oldGeneration = desc.maps[0].textureGeneration;
    uint64_t const replacementGeneration =
        usdGenImaging::InvalidateUsdGenImageMapCache();
    UsdGenGraphDesc replacement = usdGenImaging::BuildGraphDescFromStage(
        stage, SdfPath("/Groom/Description"), options);
    Check(replacement.maps.size() == 1 && replacement.maps[0].imagePayload &&
              replacement.maps[0].textureGeneration == replacementGeneration &&
              replacement.maps[0].textureGeneration > oldGeneration &&
              replacement.maps[0].imagePayload != oldPayload && oldPayload &&
              oldPayload->Data()[0] == oldFirst,
          "cache invalidation publishes new pixels without mutating retained COW data");

    usdGenImaging::InvalidateUsdGenImageMapCache();
    std::vector<usdGenImaging::UsdGenDecodedImageMap> concurrent(8);
    std::vector<std::thread> workers;
    for (size_t i = 0; i != concurrent.size(); ++i)
        workers.emplace_back([&, i] {
            concurrent[i] = usdGenImaging::ResolveUsdGenImageMap(imagePath);
        });
    for (auto &worker : workers) worker.join();
    bool coalesced = concurrent.front().payload != nullptr;
    for (auto const &result : concurrent)
        coalesced = coalesced && result.payload == concurrent.front().payload &&
            result.textureGeneration == concurrent.front().textureGeneration;
    Check(coalesced, "concurrent image decodes coalesce to one immutable payload");

    // Exercise two distinct cache keys racing to extend the same immutable
    // table. A stale copy-on-write publication must retry instead of
    // overwriting the other key.
    std::string const aliasPath = std::string(USDGEN_SOURCE_DIR) +
        "/tests/golden/../golden/stormLook_A.png";
    bool retainedBothKeys = true;
    for (unsigned iteration = 0; iteration != 16; ++iteration) {
        usdGenImaging::InvalidateUsdGenImageMapCache();
        usdGenImaging::UsdGenDecodedImageMap first, second;
        std::thread a([&] {
            first = usdGenImaging::ResolveUsdGenImageMap(imagePath);
        });
        std::thread b([&] {
            second = usdGenImaging::ResolveUsdGenImageMap(aliasPath);
        });
        a.join();
        b.join();
        auto const firstAgain =
            usdGenImaging::ResolveUsdGenImageMap(imagePath);
        auto const secondAgain =
            usdGenImaging::ResolveUsdGenImageMap(aliasPath);
        retainedBothKeys = retainedBothKeys && first.payload && second.payload &&
            firstAgain.payload == first.payload &&
            secondAgain.payload == second.payload &&
            first.textureGeneration == second.textureGeneration;
    }
    Check(retainedBothKeys,
          "racing COW table extensions retain both immutable cache keys");

    map.GetAttribute(TfToken("usdGen:map:file"))
        .Set(SdfAssetPath(imagePath + ".missing"));
    UsdGenGraphDesc missing = usdGenImaging::BuildGraphDescFromStage(
        stage, SdfPath("/Groom/Description"), options);
    Check(missing.maps.size() == 1 && !missing.maps[0].imagePayload,
          "missing ImageMap asset leaves no fake payload");
    bool diagnosed = false;
    for (std::string const &error : missing.validationErrors)
        diagnosed |= error.find("HioImage could not open") != std::string::npos;
    Check(diagnosed, "missing ImageMap asset reports a validation diagnostic");
    return failures ? 1 : 0;
}
