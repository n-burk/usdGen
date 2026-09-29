// usdGenPomade — model registry implementation (plan/18 §2.1).
#include "usdGenPomade/pomadeRegistry.h"

#include <algorithm>

namespace usdGenPomade {

PomadePublishTarget::~PomadePublishTarget() = default;

PomadeRegistry &
PomadeRegistry::Get()
{
    // Leaked on purpose (see the header): scene indices outlive ordinary
    // static destruction order on Windows DLL unload.
    static PomadeRegistry *const registry = new PomadeRegistry();
    return *registry;
}

int
PomadeRegistry::Register(PomadeModel *model)
{
    if (!model) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto const &kv : _models) {
        if (kv.second == model) {
            return kv.first;
        }
    }
    int const id = _nextId++;
    _models[id] = model;
    return id;
}

void
PomadeRegistry::Unregister(int modelId)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_activeId == modelId) {
        _activeId = 0;
    }
    _models.erase(modelId);
}

int
PomadeRegistry::ModelCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return int(_models.size());
}

bool
PomadeRegistry::SetActive(int modelId)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (modelId == 0) {
        _activeId = 0;
        return true;
    }
    if (!_models.count(modelId)) {
        return false;
    }
    _activeId = modelId;
    return true;
}

int
PomadeRegistry::GetActiveId() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _activeId;
}

PomadeModel *
PomadeRegistry::Active() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _models.find(_activeId);
    return it == _models.end() ? nullptr : it->second;
}

void
PomadeRegistry::AttachIndex(PomadePublishTarget *target)
{
    if (!target) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    if (std::find(_targets.begin(), _targets.end(), target) ==
        _targets.end()) {
        _targets.push_back(target);
    }
}

void
PomadeRegistry::DetachIndex(PomadePublishTarget *target)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _targets.erase(std::remove(_targets.begin(), _targets.end(), target),
                   _targets.end());
}

int
PomadeRegistry::IndexCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return int(_targets.size());
}

int
PomadeRegistry::Publish(uint32_t dirtyMask)
{
    // Snapshot under the lock, publish outside it: PublishModel reaches
    // back into the registry (and into Hydra observers) and must not be
    // holding _mutex when it does.
    PomadeModel *model = nullptr;
    std::vector<PomadePublishTarget *> targets;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _models.find(_activeId);
        model = it == _models.end() ? nullptr : it->second;
        targets = _targets;
    }
    int published = 0;
    for (PomadePublishTarget *target : targets) {
        if (target->PublishModel(model, dirtyMask)) {
            ++published;
        }
    }
    return published;
}

bool
PomadeRegistry::QueryPublishedLevel(int level, int *outFaceCount,
                                   int *outPointCount,
                                   int *outTubeCount) const
{
    std::vector<PomadePublishTarget *> targets;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        targets = _targets;
    }
    for (PomadePublishTarget const *target : targets) {
        if (target->QueryPublishedLevel(level, outFaceCount, outPointCount,
                                        outTubeCount)) {
            return true;
        }
    }
    return false;
}

} // namespace usdGenPomade
