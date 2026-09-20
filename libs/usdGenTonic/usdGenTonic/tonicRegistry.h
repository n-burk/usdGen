// usdGenTonic — the process-global model registry (plan/18 §2.1).
//
// Before V0 there were two unrelated models: the one Tonic_Create allocated
// inside the DLL for the tools, and the one UsdGenTonicSceneIndex allocated
// for itself. Nothing joined them, so every edit the tools made was invisible
// in the viewport (plan/18 §0 F1). The registry is that join:
//
//   * Tonic_Create registers its model and gets a model id back;
//     Tonic_Destroy unregisters it.
//   * every scene index attaches on construction and detaches on
//     destruction, as a TonicPublishTarget;
//   * Tonic_Activate names the one model the viewport draws;
//   * Tonic_Publish (main thread) hands the active model to every attached
//     target, which is the ONLY way the viewport learns about a change.
//
// The registry deliberately knows nothing about Hydra: the publish target is
// an interface so this header stays free of pxr includes and the C ABI can
// include it without pulling in the imaging stack.
#ifndef USDGEN_TONIC_REGISTRY_H
#define USDGEN_TONIC_REGISTRY_H

#include "usdGenTonic/api.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace usdGenTonic {

class TonicModel;

// Anything that draws a model. UsdGenTonicSceneIndex is the only
// implementation; the T1 tests use it directly.
class USDGENTONIC_API TonicPublishTarget {
public:
    virtual ~TonicPublishTarget();
    // Publish `model` (null = no active model, publish the empty/test
    // state). `dirtyMask` is a TonicDirty OR that the caller wants
    // republished on top of whatever the model itself reports pending.
    // Returns true when anything was published.
    virtual bool PublishModel(TonicModel *model, uint32_t dirtyMask) = 0;
    // Census of what is on screen for `level`; false when that level is not
    // published. This is how a tool asks what the viewport actually got,
    // rather than assuming its own edit arrived.
    virtual bool QueryPublishedLevel(int level, int *outFaceCount,
                                     int *outPointCount,
                                     int *outTubeCount) const = 0;
};

class USDGENTONIC_API TonicRegistry {
public:
    // Deliberately leaked: Hydra scene indices can be destroyed during
    // static destruction, and DetachIndex must not touch a registry that
    // has already run its own destructor.
    static TonicRegistry &Get();

    // Model lifetime. Register returns a model id > 0; Unregister on the
    // active model deactivates it first. Registering the same model twice
    // returns the existing id.
    int Register(TonicModel *model);
    void Unregister(int modelId);
    int ModelCount() const;

    // The one model the viewport draws. `modelId` 0 clears it. Returns
    // false for an unknown id (the active model is left alone).
    bool SetActive(int modelId);
    int GetActiveId() const;
    TonicModel *Active() const;

    // Scene-index attachment. Attaching twice is a no-op.
    void AttachIndex(TonicPublishTarget *target);
    void DetachIndex(TonicPublishTarget *target);
    int IndexCount() const;

    // Main thread only. Publishes the active model to every attached
    // target and returns how many of them published something.
    int Publish(uint32_t dirtyMask);
    // Ask the attached targets what they published for `level`; the first
    // one that answers wins. False when no target publishes that level.
    bool QueryPublishedLevel(int level, int *outFaceCount, int *outPointCount,
                             int *outTubeCount) const;

private:
    TonicRegistry() = default;
    ~TonicRegistry() = default;
    TonicRegistry(TonicRegistry const &) = delete;
    TonicRegistry &operator=(TonicRegistry const &) = delete;

    mutable std::mutex _mutex;
    std::map<int, TonicModel *> _models;
    std::vector<TonicPublishTarget *> _targets;
    int _nextId = 1;
    int _activeId = 0;
};

} // namespace usdGenTonic

#endif // USDGEN_TONIC_REGISTRY_H
