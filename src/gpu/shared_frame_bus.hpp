#ifndef FLUXCAP_GPU_SHARED_FRAME_BUS_FORWARD_HPP
#define FLUXCAP_GPU_SHARED_FRAME_BUS_FORWARD_HPP
#include <fluxcap/gpu.hpp>

namespace fluxcap::gpu::internal {

[[nodiscard]] GpuError reserve_shared_frame_bus_producer(
    const std::shared_ptr<SharedFrameBusPublisherState>&,
    std::uint64_t&) noexcept;
void release_shared_frame_bus_producer(
    const std::shared_ptr<SharedFrameBusPublisherState>&,
    std::uint64_t) noexcept;
[[nodiscard]] GpuError begin_shared_frame_bus_publish(
    const std::shared_ptr<SharedFrameBusPublisherState>&,
    std::uint64_t, SharedFrameBusWriteLease&) noexcept;
[[nodiscard]] GpuError commit_shared_frame_bus_publish(
    const std::shared_ptr<SharedFrameBusPublisherState>&,
    std::uint64_t, SharedFrameBusWriteLease&&,
    const SharedFrameBusFrameMetadata&) noexcept;
[[nodiscard]] GpuError commit_shared_frame_bus_publish(
    const std::shared_ptr<SharedFrameBusPublisherState>&,
    std::uint64_t, SharedFrameBusWriteLease&&,
    const SharedFrameBusFrameMetadata&,
    const SharedFrameBusFrameSideData&) noexcept;
[[nodiscard]] GpuError publish_shared_frame_bus_cursor_shape(
    const std::shared_ptr<SharedFrameBusPublisherState>&,
    std::uint64_t, const WgcCursorShape&) noexcept;
[[nodiscard]] GpuError publish_shared_frame_bus_move_result(
    const std::shared_ptr<SharedFrameBusPublisherState>&,
    std::uint64_t, const SharedFrameBusMoveResult&) noexcept;
[[nodiscard]] ID3D11Device* shared_frame_bus_device(
    const std::shared_ptr<SharedFrameBusPublisherState>&) noexcept;
[[nodiscard]] SharedFrameBusConfig shared_frame_bus_config(
    const std::shared_ptr<SharedFrameBusPublisherState>&) noexcept;
[[nodiscard]] std::uint64_t shared_frame_bus_sequence(
    const std::shared_ptr<SharedFrameBusPublisherState>&) noexcept;

} // namespace fluxcap::gpu::internal
#endif
