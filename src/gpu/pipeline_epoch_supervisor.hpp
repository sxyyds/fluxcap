#pragma once

#include <array>
#include <cstdint>
#include <mutex>

namespace fluxcap::gpu::internal {

enum class PipelineEpochState : std::uint8_t {
    running,
    quiescing,
    retired,
    building,
    ready
};

enum class PipelineEpochStatus : std::uint8_t {
    ok,
    invalid_argument,
    invalid_state,
    stale_epoch,
    stale_registration,
    duplicate_participant,
    participant_capacity_exceeded,
    registration_space_exhausted,
    pending_acknowledgements,
    epoch_exhausted
};

struct PipelineEpochResult final {
    PipelineEpochStatus status = PipelineEpochStatus::ok;
    bool changed = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == PipelineEpochStatus::ok;
    }
};

struct PipelineEpochTicket final {
    std::uint64_t epoch = 0;
    std::uint64_t nonce = 0;

    [[nodiscard]] friend constexpr bool operator==(
        const PipelineEpochTicket&,
        const PipelineEpochTicket&) noexcept = default;
};

// Registrations are epoch-scoped. registration_nonce prevents a delayed ACK
// from a disconnected process being accepted after its participant id is
// reused in the same epoch.
struct PipelineEpochRegistration final {
    PipelineEpochTicket ticket{};
    std::uint64_t participant_id = 0;
    std::uint64_t registration_nonce = 0;
};

struct PipelineEpochFrameContract final {
    PipelineEpochTicket ticket{};
    bool discontinuity = false;
    bool force_keyframe = false;
};

struct PipelineEpochSnapshot final {
    PipelineEpochState state = PipelineEpochState::running;
    PipelineEpochTicket ticket{};
    std::uint32_t registered_participants = 0;
    std::uint32_t pending_quiesce_acknowledgements = 0;
    std::uint32_t pending_ready_acknowledgements = 0;
    std::uint64_t recovery_attempts = 0;
    std::uint64_t recovery_successes = 0;
    std::uint64_t stale_acknowledgements = 0;
    std::int32_t last_device_lost_reason = 0;
    bool first_frame_pending = false;
};

// Coordinates ownership epochs; it does not own GPU objects. The resource
// supervisor drives these transitions around capture/bus/transform/encoder
// teardown and construction:
//
// RUNNING -> QUIESCING -> RETIRED -> BUILDING -> READY -> RUNNING
//
// Every asynchronous message must carry its ticket and, for participants, its
// registration. Nonces are freshness tokens, not authentication secrets.
class PipelineEpochSupervisor final {
public:
    static constexpr std::uint32_t max_participants = 64;

    explicit PipelineEpochSupervisor(
        std::uint64_t initial_epoch = 1,
        std::uint64_t initial_nonce = 0) noexcept;

    PipelineEpochSupervisor(const PipelineEpochSupervisor&) = delete;
    PipelineEpochSupervisor& operator=(const PipelineEpochSupervisor&) = delete;

    [[nodiscard]] PipelineEpochResult register_participant(
        std::uint64_t participant_id,
        PipelineEpochRegistration& output) noexcept;
    [[nodiscard]] PipelineEpochResult unregister_participant(
        const PipelineEpochRegistration&) noexcept;

    // Duplicate reports for the active quiescing epoch are idempotent. Reports
    // from a retired epoch are rejected after begin_build changes the ticket.
    [[nodiscard]] PipelineEpochResult report_device_lost(
        const PipelineEpochTicket& observed_epoch,
        std::int32_t reason) noexcept;
    [[nodiscard]] PipelineEpochResult acknowledge_quiesced(
        const PipelineEpochRegistration&) noexcept;
    [[nodiscard]] PipelineEpochResult retire(
        const PipelineEpochTicket&) noexcept;
    [[nodiscard]] PipelineEpochResult begin_build(
        const PipelineEpochTicket& retired_epoch,
        PipelineEpochTicket& output) noexcept;
    // Discards a failed in-progress build so the same ticket can be used to
    // begin another build epoch. Any registered build participants are
    // removed before returning to RETIRED.
    [[nodiscard]] PipelineEpochResult abandon_build(
        const PipelineEpochTicket&) noexcept;
    [[nodiscard]] PipelineEpochResult acknowledge_ready(
        const PipelineEpochRegistration&) noexcept;
    [[nodiscard]] PipelineEpochResult mark_ready(
        const PipelineEpochTicket&) noexcept;
    [[nodiscard]] PipelineEpochResult resume(
        const PipelineEpochTicket&) noexcept;

    // Reading the contract does not consume it. Call acknowledge_frame_published
    // only after the same frame has been committed to capture output and queued
    // to the encoder. Failed submissions therefore cannot lose the contract.
    [[nodiscard]] PipelineEpochResult frame_contract(
        const PipelineEpochTicket&,
        PipelineEpochFrameContract& output) const noexcept;
    [[nodiscard]] PipelineEpochResult acknowledge_frame_published(
        const PipelineEpochTicket&) noexcept;

    [[nodiscard]] PipelineEpochSnapshot snapshot() const noexcept;
    [[nodiscard]] PipelineEpochTicket ticket() const noexcept;
    [[nodiscard]] PipelineEpochState state() const noexcept;

private:
    struct Participant final {
        std::uint64_t participant_id = 0;
        std::uint64_t registration_nonce = 0;
        bool active = false;
        bool quiesced = false;
        bool ready = false;
    };

    [[nodiscard]] static std::uint64_t make_nonce() noexcept;
    [[nodiscard]] std::uint64_t make_unique_nonce_locked() const noexcept;
    [[nodiscard]] std::uint64_t make_registration_nonce_locked() noexcept;
    [[nodiscard]] bool ticket_matches_locked(
        const PipelineEpochTicket&) const noexcept;
    [[nodiscard]] Participant* find_participant_locked(
        const PipelineEpochRegistration&) noexcept;
    [[nodiscard]] const Participant* find_participant_locked(
        const PipelineEpochRegistration&) const noexcept;
    [[nodiscard]] std::uint32_t pending_quiesce_locked() const noexcept;
    [[nodiscard]] std::uint32_t pending_ready_locked() const noexcept;
    [[nodiscard]] std::uint32_t participant_count_locked() const noexcept;
    PipelineEpochResult stale_epoch_locked() noexcept;
    PipelineEpochResult stale_registration_locked() noexcept;

    mutable std::mutex mutex_;
    PipelineEpochState state_ = PipelineEpochState::running;
    PipelineEpochTicket ticket_{};
    std::array<Participant, max_participants> participants_{};
    std::uint64_t recovery_attempts_ = 0;
    std::uint64_t recovery_successes_ = 0;
    std::uint64_t stale_acknowledgements_ = 0;
    std::uint64_t next_registration_nonce_ = 1;
    std::int32_t last_device_lost_reason_ = 0;
    bool first_frame_pending_ = false;
};

[[nodiscard]] const char* pipeline_epoch_state_string(
    PipelineEpochState) noexcept;
[[nodiscard]] const char* pipeline_epoch_status_string(
    PipelineEpochStatus) noexcept;

} // namespace fluxcap::gpu::internal
