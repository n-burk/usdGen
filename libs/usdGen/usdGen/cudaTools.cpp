#ifdef USDGEN_ENABLE_CUDA
#include "usdGen/gpu/tools.h"
#include <limits>

namespace usdGen::gpu {

CudaToolSession::~CudaToolSession() { Close(); }

bool CudaToolSession::Fail(char const* message) {
    diagnostic_ = message;
    return false;
}

bool CudaToolSession::CheckCurrent(UsdGenGenerationConstPtr const& current) {
    if (!base_) return Fail("no active device stroke");
    if (!current || (current != expected_ && current->device != latest_))
        return Fail("device stroke is stale; close it before using a new graph snapshot");
    if (current->id == std::numeric_limits<int64_t>::max())
        return Fail("device publication identity exhausted");
    return true;
}

bool CudaToolSession::Begin(uint64_t graphVersion) {
    if (base_) return Fail("a device stroke is already active");
    auto snapshot = engine_.Generation();
    if (!snapshot || !snapshot->device || engine_.NeedsCommit())
        return Fail("device stroke requires a clean published GPU snapshot");
    auto lease = AcquireGeometry(snapshot->device, stream_);
    if (!lease) return Fail("cannot acquire the device stroke base on this stream");
    if (!engine_.BeginDeviceEdit(snapshot, &editToken_, &diagnostic_)) return false;
    base_ = std::move(snapshot);
    expected_ = base_;
    latest_ = base_->device;
    graphVersion_ = graphVersion;
    diagnostic_.clear();
    return true;
}

bool CudaToolSession::UpdateIndexed(DeviceView<const int32_t> indices,
                                   DeviceView<const float3> absolutePositions) {
    auto current = engine_.Generation();
    if (!CheckCurrent(current)) return false;
    auto lease = AcquireGeometry(base_->device, stream_);
    if (!lease) return Fail("cannot acquire press-time geometry");
    auto status = override_.Apply(lease.Geometry().points, indices, absolutePositions, stream_);
    if (status == PointOverrideStatus::Ok) status = override_.Finish(stream_);
    if (status != PointOverrideStatus::Ok) return Fail("invalid or failed sparse GPU point edit");
    auto points = override_.releaseCompleted();
    if (!points) return Fail("GPU point edit did not produce a completed allocation");
    auto revision = MakePointRevisionGeneration(base_->device, uint64_t(current->id + 1),
        std::move(points), {"pointEdit", graphVersion_, base_->device->Identity().generation}, &diagnostic_);
    if (!revision || !engine_.StageDeviceRevision(current, revision, editToken_, &diagnostic_)) return false;
    latest_ = staged_ = std::move(revision);
    expected_ = current;
    diagnostic_.clear();
    return true;
}

bool CudaToolSession::Cancel() {
    auto current = engine_.Generation();
    if (!CheckCurrent(current)) return false;
    if (current != base_) {
        auto revision = MakePointRevisionGeneration(base_->device, uint64_t(current->id + 1), {},
            {"pointEdit.cancel", graphVersion_, base_->device->Identity().generation}, &diagnostic_);
        if (!revision || !engine_.StageDeviceRevision(current, revision, editToken_, &diagnostic_)) return false;
        // The engine owns the staged restoration. Close/destruction must not
        // remove it before the app's subsequent LiveOverride commit.
    }
    const bool ended = engine_.EndDeviceEdit(editToken_, current, current == base_);
    editToken_ = 0;
    staged_.reset(); latest_.reset(); base_.reset(); expected_.reset();
    if (!ended) return Fail("device stroke was superseded before cancellation");
    diagnostic_.clear();
    return true;
}

std::shared_ptr<const UsdGenDeviceGeneration> CudaToolSession::Close() {
    auto current = engine_.Generation();
    bool stale = base_ && !CheckCurrent(current);
    if (editToken_) {
        if (!engine_.EndDeviceEdit(editToken_, current)) stale = true;
        editToken_ = 0;
    }
    auto result = std::move(latest_);
    staged_.reset(); base_.reset(); expected_.reset();
    return stale ? nullptr : result;
}

bool CudaToolSession::Pick(PickQuery query, PickResult* result) {
    if (!result) return Fail("pick result pointer is null");
    auto current = engine_.Generation();
    if (base_ && !CheckCurrent(current)) return false;
    auto lease = current ? AcquireGeometry(base_ ? base_->device : current->device, stream_) : CudaGeometryLease{};
    if (!lease) return Fail("pick requires a published GPU snapshot on this stream");
    auto status = picking_.ApplyPick(lease.Geometry(), query, stream_);
    if (status == PickingStatus::Ok) status = picking_.Finish(stream_);
    if (status != PickingStatus::Ok) return Fail(picking_.diagnostic());
    *result = picking_.result();
    diagnostic_.clear();
    return true;
}

bool CudaToolSession::Footprint(PickQuery query) {
    auto current = engine_.Generation();
    if (base_ && !CheckCurrent(current)) return false;
    auto lease = current ? AcquireGeometry(base_ ? base_->device : current->device, stream_) : CudaGeometryLease{};
    if (!lease) return Fail("footprint requires a published GPU snapshot on this stream");
    auto status = picking_.ApplyFootprint(lease.Geometry(), query, stream_);
    if (status == PickingStatus::Ok) status = picking_.Finish(stream_);
    if (status != PickingStatus::Ok) return Fail(picking_.diagnostic());
    diagnostic_.clear();
    return true;
}

} // namespace usdGen::gpu
#endif
