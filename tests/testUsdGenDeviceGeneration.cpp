#include "usdGen/deviceGeneration.h"

#include <cstdio>
#include <memory>
#include <string>

namespace {

class FakeOwner;

class FakeConsumer final : public usdGen::UsdGenDeviceConsumer
{
public:
    FakeConsumer(FakeOwner const *owner, usdGen::UsdGenDeviceStream stream)
        : owner_(owner), stream_(stream) {}
    ~FakeConsumer() override { Complete(); }
    usdGen::UsdGenDeviceStream Stream() const noexcept override { return stream_; }
    usdGen::UsdGenDeviceStatus WaitUntilReady() const noexcept override;
    void Complete() noexcept override;
private:
    FakeOwner const *owner_;
    usdGen::UsdGenDeviceStream stream_;
    bool completed_ = false;
};

class FakeOwner final : public usdGen::UsdGenDeviceOwner
{
public:
    bool ProducerReady() const noexcept override { return ready; }
    std::unique_ptr<usdGen::UsdGenDeviceConsumer> AcquireConsumer(
        usdGen::UsdGenDeviceStream stream) const noexcept override;

    bool ready = false;
    bool reject = false;
    bool wrongStream = false;
    mutable int active = 0;
    mutable int acquired = 0;
    mutable int rejected = 0;
    mutable int waits = 0;
    mutable int released = 0;
};

std::unique_ptr<usdGen::UsdGenDeviceConsumer>
FakeOwner::AcquireConsumer(usdGen::UsdGenDeviceStream stream) const noexcept
{
    if (reject) {
        ++rejected;
        return {};
    }
    try {
        auto consumer = std::make_unique<FakeConsumer>(
            this, wrongStream ? stream + 1 : stream);
        ++active;
        ++acquired;
        return consumer;
    } catch (...) {
        ++rejected;
        return {};
    }
}

usdGen::UsdGenDeviceStatus FakeConsumer::WaitUntilReady() const noexcept
{
    if (!owner_ || completed_) return usdGen::UsdGenDeviceStatus::InvalidLease;
    ++owner_->waits;
    return owner_->ready ? usdGen::UsdGenDeviceStatus::Ok
                         : usdGen::UsdGenDeviceStatus::ProducerNotReady;
}

void FakeConsumer::Complete() noexcept
{
    if (completed_) return;
    completed_ = true;
    if (owner_ && owner_->active > 0) {
        --owner_->active;
        ++owner_->released;
    }
}

int Check(bool condition, char const *message, int *failures)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++*failures;
    }
    return condition ? 0 : 1;
}

} // namespace

int main()
{
    int failures = 0;
    auto owner = std::make_shared<FakeOwner>();
    std::weak_ptr<FakeOwner> weakOwner = owner;

    usdGen::UsdGenDeviceGeneration::CreateInfo info;
    info.identity = {usdGen::UsdGenDeviceBackend::Cuda, 2, 17};
    info.geometry.topologyVersion = 4;
    info.geometry.valueVersion = 9;
    info.geometry.curveCount = 3;
    info.geometry.pointCount = 12;
    info.geometry.tiles.push_back({7, 0, 3, 0, 12});
    info.tool = {"brushA", 21, 17};
    info.channels.push_back({"points", usdGen::UsdGenDeviceValueType::Float32x3,
                             usdGen::UsdGenDeviceDomain::Point, 12, 3, 12, true,
                             usdGen::UsdGenDeviceChannelSemantic::Points});
    info.channels.push_back({"width", usdGen::UsdGenDeviceValueType::Float32,
                             usdGen::UsdGenDeviceDomain::Point, 12, 1, 4, true,
                             usdGen::UsdGenDeviceChannelSemantic::Widths});
    info.channels.push_back({"curveOffsets", usdGen::UsdGenDeviceValueType::UInt32,
                             usdGen::UsdGenDeviceDomain::Topology, 4, 1, 4, true,
                             usdGen::UsdGenDeviceChannelSemantic::CurveOffsets});
    info.owner = owner;

    std::string reason;
    std::shared_ptr<const usdGen::UsdGenDeviceGeneration> generation =
        usdGen::UsdGenDeviceGeneration::Create(std::move(info), &reason);
    Check(bool(generation), "valid device generation creates", &failures);
    Check(generation && generation->Identity().deviceIndex == 2,
          "identity is retained", &failures);
    Check(generation && generation->Geometry().tiles.size() == 1,
          "tile metadata is retained", &failures);
    Check(generation && generation->Tool().toolId == "brushA",
          "tool snapshot is retained", &failures);

    // Legacy range-only snapshots retain the all-Unknown topology contract;
    // a partially supplied topology is never ambiguous for a device renderer.
    usdGen::UsdGenDeviceGeneration::CreateInfo legacyInfo;
    legacyInfo.identity = generation->Identity();
    legacyInfo.geometry = generation->Geometry();
    legacyInfo.tool = generation->Tool();
    legacyInfo.channels = generation->Channels();
    legacyInfo.owner = generation->Owner();
    Check(bool(usdGen::UsdGenDeviceGeneration::Create(std::move(legacyInfo), &reason)),
          "all-Unknown curve topology preserves legacy generation admission", &failures);
    usdGen::UsdGenDeviceGeneration::CreateInfo mixedTopology;
    mixedTopology.identity = generation->Identity();
    mixedTopology.geometry = generation->Geometry();
    mixedTopology.geometry.curveTopology.type = usdGen::UsdGenDeviceCurveType::Cubic;
    mixedTopology.tool = generation->Tool();
    mixedTopology.channels = generation->Channels();
    mixedTopology.owner = generation->Owner();
    Check(!usdGen::UsdGenDeviceGeneration::Create(std::move(mixedTopology), &reason),
          "mixed Unknown and concrete curve topology is rejected", &failures);

    auto invalid = usdGen::UsdGenDeviceGeneration::Create(
        usdGen::UsdGenDeviceGeneration::CreateInfo{}, &reason);
    Check(!invalid && !reason.empty(), "invalid generation is rejected", &failures);

    usdGen::UsdGenDeviceGeneration::CreateInfo badShape;
    badShape.identity = {usdGen::UsdGenDeviceBackend::Cuda, 2, 18};
    badShape.geometry.curveCount = 3;
    badShape.geometry.pointCount = 12;
    badShape.owner = owner;
    badShape.channels.push_back(
        {"width", usdGen::UsdGenDeviceValueType::Float32,
         usdGen::UsdGenDeviceDomain::Point, 11, 1, 4, true,
         usdGen::UsdGenDeviceChannelSemantic::Widths});
    Check(!usdGen::UsdGenDeviceGeneration::Create(std::move(badShape), &reason),
          "semantic channel count is validated", &failures);

    usdGen::UsdGenDeviceLease lease = generation->AcquireLease(0x1234);
    Check(lease.IsValid() && owner->active == 1,
          "consumer lease acquires owner use", &failures);
    usdGen::UsdGenDeviceLease secondLease = lease;
    lease.Complete();
    Check(!lease.IsValid() && secondLease.IsValid() && owner->active == 1,
          "completing one copy does not release shared backend use", &failures);
    Check(secondLease.WaitUntilReady() == usdGen::UsdGenDeviceStatus::ProducerNotReady,
          "consumer cannot use an unfinished producer", &failures);
    owner->ready = true;
    Check(owner->ProducerReady(), "owner exposes producer readiness", &failures);
    Check(secondLease.WaitUntilReady() == usdGen::UsdGenDeviceStatus::Ok,
          "lease inserts producer ordering after readiness", &failures);

    owner->reject = true;
    usdGen::UsdGenDeviceLease rejectedLease = generation->AcquireLease(0x4321);
    Check(!rejectedLease.IsValid() && owner->rejected == 1,
          "owner rejection does not create a lease", &failures);
    owner->reject = false;

    owner->wrongStream = true;
    auto wrongStreamLease = generation->AcquireLease(0x4321);
    Check(!wrongStreamLease.IsValid() && owner->active == 1 &&
              owner->released == 1,
          "wrong-stream consumer is completed and rejected", &failures);
    owner->wrongStream = false;

    generation.reset();
    owner.reset();
    Check(!weakOwner.expired(), "lease retains owner after generation release", &failures);
    secondLease.Complete();
    secondLease.Complete();
    Check(weakOwner.expired() &&
              secondLease.WaitUntilReady() == usdGen::UsdGenDeviceStatus::InvalidLease,
          "owner releases after asynchronous use completes exactly once", &failures);

    std::printf(failures ? "testUsdGenDeviceGeneration: FAILED (%d)\n"
                         : "testUsdGenDeviceGeneration: PASS\n",
                failures);
    return failures ? 1 : 0;
}
