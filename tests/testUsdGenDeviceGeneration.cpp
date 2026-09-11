#include "usdGen/deviceGeneration.h"

#include <cstdio>
#include <memory>
#include <string>

namespace {

class FakeOwner final : public usdGen::UsdGenDeviceOwner
{
public:
    bool ProducerReady() const noexcept override { return ready; }

    usdGen::UsdGenDeviceStatus AcquireConsumer(
        usdGen::UsdGenDeviceStream stream, uint64_t *token) const noexcept override
    {
        if (!token || stream == 0 || reject) {
            ++rejected;
            return usdGen::UsdGenDeviceStatus::ConsumerRejected;
        }
        *token = ++nextToken;
        ++active;
        ++acquired;
        lastStream = stream;
        return usdGen::UsdGenDeviceStatus::Ok;
    }

    usdGen::UsdGenDeviceStatus WaitForProducer(
        usdGen::UsdGenDeviceStream stream, uint64_t token) const noexcept override
    {
        if (stream != lastStream || token == 0 || active == 0)
            return usdGen::UsdGenDeviceStatus::InvalidLease;
        ++waits;
        return ready ? usdGen::UsdGenDeviceStatus::Ok
                     : usdGen::UsdGenDeviceStatus::ProducerNotReady;
    }

    void ReleaseConsumer(
        usdGen::UsdGenDeviceStream stream, uint64_t token) const noexcept override
    {
        if (stream == lastStream && token != 0 && active > 0) {
            --active;
            ++released;
        }
    }

    bool ready = false;
    bool reject = false;
    mutable uint64_t nextToken = 0;
    mutable uint64_t lastStream = 0;
    mutable int active = 0;
    mutable int acquired = 0;
    mutable int rejected = 0;
    mutable int waits = 0;
    mutable int released = 0;
};

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

    generation.reset();
    owner.reset();
    Check(!weakOwner.expired(), "lease retains owner after generation release", &failures);
    secondLease.Complete();
    Check(weakOwner.expired(), "owner releases after asynchronous use completes", &failures);

    std::printf(failures ? "testUsdGenDeviceGeneration: FAILED (%d)\n"
                         : "testUsdGenDeviceGeneration: PASS\n",
                failures);
    return failures ? 1 : 0;
}
