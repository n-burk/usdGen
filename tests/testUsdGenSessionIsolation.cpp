// Session-key and lifecycle contract checks (stage-free imaging store).
#include "usdGenImaging/usdGenImagingSession.h"

#include <cstdio>

using namespace usdGenImaging;

namespace {
int failures = 0;
void Check(bool value, char const *what)
{
    if (!value) { ++failures; std::printf("FAIL: %s\n", what); }
    else std::printf("ok: %s\n", what);
}
}

int main()
{
    UsdGenSessionStore &store = UsdGenSessionStore::GetInstance();
    const SdfPath pathA("/__sessionIsolationA");
    const SdfPath pathB("/__sessionIsolationB");

    UsdGenSessionKey emptyA{"", pathA, 1001};
    UsdGenSessionKey emptyB{"", pathA, 1002};
    Check(!(emptyA == emptyB), "empty session ids use render instance fallback");
    auto a = store.Attach(emptyA);
    auto b = store.Attach(emptyB);
    Check(a && b && a != b, "independent empty-id sessions do not merge");
    Check(a->AttachedIndices() == 1 && b->AttachedIndices() == 1,
          "independent sessions have independent attach counts");

    UsdGenSessionKey explicitA{"shared-session", pathA, 2001};
    UsdGenSessionKey explicitB{"shared-session", pathA, 2002};
    Check(explicitA == explicitB, "explicit id ignores render instance");
    Check(UsdGenSessionKeyHash()(explicitA) == UsdGenSessionKeyHash()(explicitB),
          "explicit equivalent keys hash equally");
    auto sharedA = store.Attach(explicitA);
    auto sharedB = store.Attach(explicitB);
    Check(sharedA == sharedB, "explicit equivalent sessions merge");
    Check(sharedA->AttachedIndices() == 2, "merged session counts both attachments");

    UsdGenSessionKey differentPath{"shared-session", pathB, 2001};
    UsdGenSessionKey differentId{"other-session", pathA, 2001};
    Check(!(explicitA == differentPath) && !(explicitA == differentId),
          "path and explicit-id changes isolate sessions");
    auto pathSession = store.Attach(differentPath);
    auto idSession = store.Attach(differentId);
    Check(pathSession != sharedA && idSession != sharedA,
          "path and id variants do not share state");

    // A mark arriving after the stager consumed an earlier mark belongs to the
    // next commit and must not be cleared by Commit itself.
    sharedA->MarkNeedsDesc();
    Check(sharedA->ConsumeNeedsDesc(), "first description mark is consumable");
    sharedA->MarkNeedsDesc();
    Check(sharedA->NeedsDesc(), "second description mark is pending");
    sharedA->Commit(usdGen::UsdGenCommitReason::LiveOverride);
    Check(sharedA->NeedsDesc(), "pending description mark survives Commit");
    Check(sharedA->ConsumeNeedsDesc(), "pending mark can be consumed next cycle");
    Check(!sharedA->ConsumeNeedsDesc(), "repeated consume is idempotent");

    // Detach one of the merged handles, then the final attachment. Keep the
    // local strong pointer to verify the object remains safe after store drop.
    store.Detach(explicitA);
    Check(sharedB->AttachedIndices() == 1, "detach decrements merged count");
    store.Detach(explicitB);
    Check(!store.Find(explicitA), "final detach removes store entry");
    Check(sharedB->AttachedIndices() == 0, "final detach reaches zero");

    store.Detach(emptyA);
    store.Detach(emptyB);
    store.Detach(differentPath);
    store.Detach(differentId);
    Check(!store.Find(emptyA) && !store.Find(emptyB), "empty-id sessions detach independently");
    Check(store.LiveSessions().empty(), "all test sessions detached");
    std::printf("testUsdGenSessionIsolation: %s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
