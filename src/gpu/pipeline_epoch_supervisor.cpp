#include "pipeline_epoch_supervisor.hpp"

#include <Windows.h>

#include <atomic>
#include <limits>

namespace fluxcap::gpu::internal {
namespace {

std::atomic<std::uint64_t> nonce_sequence{1};

std::uint64_t mix_nonce(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

PipelineEpochResult result(
    PipelineEpochStatus status,
    bool changed = false) noexcept {
    return {status, changed};
}

} // namespace

PipelineEpochSupervisor::PipelineEpochSupervisor(
    std::uint64_t initial_epoch,
    std::uint64_t initial_nonce) noexcept {
    ticket_.epoch = initial_epoch == 0 ? 1 : initial_epoch;
    ticket_.nonce = initial_nonce == 0 ? make_nonce() : initial_nonce;
    if (ticket_.nonce == 0) ticket_.nonce = 1;
}

std::uint64_t PipelineEpochSupervisor::make_nonce() noexcept {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const std::uint64_t sequence = nonce_sequence.fetch_add(
        1, std::memory_order_relaxed);
    std::uint64_t value = static_cast<std::uint64_t>(counter.QuadPart);
    value ^= GetTickCount64() + (sequence * 0xd6e8feb86659fd93ull);
    value ^= static_cast<std::uint64_t>(GetCurrentProcessId()) << 32u;
    value ^= static_cast<std::uint64_t>(GetCurrentThreadId());
    value = mix_nonce(value);
    return value == 0 ? mix_nonce(sequence) : value;
}

std::uint64_t PipelineEpochSupervisor::make_unique_nonce_locked() const noexcept {
    for (std::uint32_t attempt = 0; attempt != 8; ++attempt) {
        const std::uint64_t candidate = make_nonce();
        if (candidate == 0 || candidate == ticket_.nonce) continue;
        bool collision = false;
        for (const Participant& participant : participants_) {
            if (participant.active
                && participant.registration_nonce == candidate) {
                collision = true;
                break;
            }
        }
        if (!collision) return candidate;
    }

    std::uint64_t candidate = ticket_.nonce;
    do {
        candidate = mix_nonce(candidate + 1);
    } while (candidate == 0 || candidate == ticket_.nonce);
    return candidate;
}

std::uint64_t
PipelineEpochSupervisor::make_registration_nonce_locked() noexcept {
    while (next_registration_nonce_ != 0) {
        const std::uint64_t candidate = next_registration_nonce_++;
        if (candidate != ticket_.nonce) return candidate;
    }
    return 0;
}

bool PipelineEpochSupervisor::ticket_matches_locked(
    const PipelineEpochTicket& ticket) const noexcept {
    return ticket.epoch != 0 && ticket.nonce != 0 && ticket == ticket_;
}

PipelineEpochSupervisor::Participant*
PipelineEpochSupervisor::find_participant_locked(
    const PipelineEpochRegistration& registration) noexcept {
    for (Participant& participant : participants_) {
        if (participant.active
            && participant.participant_id == registration.participant_id
            && participant.registration_nonce
                == registration.registration_nonce) {
            return &participant;
        }
    }
    return nullptr;
}

const PipelineEpochSupervisor::Participant*
PipelineEpochSupervisor::find_participant_locked(
    const PipelineEpochRegistration& registration) const noexcept {
    for (const Participant& participant : participants_) {
        if (participant.active
            && participant.participant_id == registration.participant_id
            && participant.registration_nonce
                == registration.registration_nonce) {
            return &participant;
        }
    }
    return nullptr;
}

std::uint32_t PipelineEpochSupervisor::pending_quiesce_locked() const noexcept {
    std::uint32_t pending = 0;
    for (const Participant& participant : participants_) {
        if (participant.active && !participant.quiesced) ++pending;
    }
    return pending;
}

std::uint32_t PipelineEpochSupervisor::pending_ready_locked() const noexcept {
    std::uint32_t pending = 0;
    for (const Participant& participant : participants_) {
        if (participant.active && !participant.ready) ++pending;
    }
    return pending;
}

std::uint32_t PipelineEpochSupervisor::participant_count_locked() const noexcept {
    std::uint32_t count = 0;
    for (const Participant& participant : participants_) {
        if (participant.active) ++count;
    }
    return count;
}

PipelineEpochResult PipelineEpochSupervisor::stale_epoch_locked() noexcept {
    if (stale_acknowledgements_ != std::numeric_limits<std::uint64_t>::max()) {
        ++stale_acknowledgements_;
    }
    return result(PipelineEpochStatus::stale_epoch);
}

PipelineEpochResult PipelineEpochSupervisor::stale_registration_locked() noexcept {
    if (stale_acknowledgements_ != std::numeric_limits<std::uint64_t>::max()) {
        ++stale_acknowledgements_;
    }
    return result(PipelineEpochStatus::stale_registration);
}

PipelineEpochResult PipelineEpochSupervisor::register_participant(
    std::uint64_t participant_id,
    PipelineEpochRegistration& output) noexcept {
    if (participant_id == 0) {
        return result(PipelineEpochStatus::invalid_argument);
    }
    std::lock_guard lock(mutex_);
    if (state_ != PipelineEpochState::running
        && state_ != PipelineEpochState::building
        && state_ != PipelineEpochState::ready) {
        return result(PipelineEpochStatus::invalid_state);
    }
    Participant* free_slot = nullptr;
    for (Participant& participant : participants_) {
        if (participant.active
            && participant.participant_id == participant_id) {
            return result(PipelineEpochStatus::duplicate_participant);
        }
        if (!participant.active && free_slot == nullptr) {
            free_slot = &participant;
        }
    }
    if (free_slot == nullptr) {
        return result(PipelineEpochStatus::participant_capacity_exceeded);
    }

    const std::uint64_t registration_nonce = make_registration_nonce_locked();
    if (registration_nonce == 0) {
        return result(PipelineEpochStatus::registration_space_exhausted);
    }
    *free_slot = {
        participant_id,
        registration_nonce,
        true,
        state_ == PipelineEpochState::running,
        state_ == PipelineEpochState::running};
    output = {ticket_, participant_id, registration_nonce};
    return result(PipelineEpochStatus::ok, true);
}

PipelineEpochResult PipelineEpochSupervisor::unregister_participant(
    const PipelineEpochRegistration& registration) noexcept {
    std::lock_guard lock(mutex_);
    if (!ticket_matches_locked(registration.ticket)) {
        return stale_epoch_locked();
    }
    Participant* participant = find_participant_locked(registration);
    if (participant == nullptr) return stale_registration_locked();
    *participant = {};
    return result(PipelineEpochStatus::ok, true);
}

PipelineEpochResult PipelineEpochSupervisor::report_device_lost(
    const PipelineEpochTicket& observed_epoch,
    std::int32_t reason) noexcept {
    std::lock_guard lock(mutex_);
    if (!ticket_matches_locked(observed_epoch)) return stale_epoch_locked();
    if (state_ == PipelineEpochState::quiescing) {
        return result(PipelineEpochStatus::ok);
    }
    if (state_ != PipelineEpochState::running) {
        return result(PipelineEpochStatus::invalid_state);
    }
    for (Participant& participant : participants_) {
        if (participant.active) participant.quiesced = false;
    }
    state_ = PipelineEpochState::quiescing;
    first_frame_pending_ = false;
    last_device_lost_reason_ = reason;
    if (recovery_attempts_ != std::numeric_limits<std::uint64_t>::max()) {
        ++recovery_attempts_;
    }
    return result(PipelineEpochStatus::ok, true);
}

PipelineEpochResult PipelineEpochSupervisor::acknowledge_quiesced(
    const PipelineEpochRegistration& registration) noexcept {
    std::lock_guard lock(mutex_);
    if (!ticket_matches_locked(registration.ticket)) {
        return stale_epoch_locked();
    }
    Participant* participant = find_participant_locked(registration);
    if (participant == nullptr) return stale_registration_locked();
    if (state_ != PipelineEpochState::quiescing) {
        return result(PipelineEpochStatus::invalid_state);
    }
    const bool changed = !participant->quiesced;
    participant->quiesced = true;
    return result(PipelineEpochStatus::ok, changed);
}

PipelineEpochResult PipelineEpochSupervisor::retire(
    const PipelineEpochTicket& ticket) noexcept {
    std::lock_guard lock(mutex_);
    if (!ticket_matches_locked(ticket)) return stale_epoch_locked();
    if (state_ != PipelineEpochState::quiescing) {
        return result(PipelineEpochStatus::invalid_state);
    }
    if (pending_quiesce_locked() != 0) {
        return result(PipelineEpochStatus::pending_acknowledgements);
    }
    state_ = PipelineEpochState::retired;
    for (Participant& participant : participants_) participant = {};
    return result(PipelineEpochStatus::ok, true);
}

PipelineEpochResult PipelineEpochSupervisor::begin_build(
    const PipelineEpochTicket& retired_epoch,
    PipelineEpochTicket& output) noexcept {
    std::lock_guard lock(mutex_);
    if (!ticket_matches_locked(retired_epoch)) return stale_epoch_locked();
    if (state_ != PipelineEpochState::retired) {
        return result(PipelineEpochStatus::invalid_state);
    }
    if (ticket_.epoch == std::numeric_limits<std::uint64_t>::max()) {
        return result(PipelineEpochStatus::epoch_exhausted);
    }
    ticket_ = {ticket_.epoch + 1, make_unique_nonce_locked()};
    next_registration_nonce_ = 1;
    state_ = PipelineEpochState::building;
    first_frame_pending_ = false;
    output = ticket_;
    return result(PipelineEpochStatus::ok, true);
}

PipelineEpochResult PipelineEpochSupervisor::abandon_build(
    const PipelineEpochTicket& ticket) noexcept {
    std::lock_guard lock(mutex_);
    if (!ticket_matches_locked(ticket)) return stale_epoch_locked();
    if (state_ != PipelineEpochState::building
        && state_ != PipelineEpochState::ready) {
        return result(PipelineEpochStatus::invalid_state);
    }
    for (Participant& participant : participants_) participant = {};
    state_ = PipelineEpochState::retired;
    first_frame_pending_ = false;
    return result(PipelineEpochStatus::ok, true);
}

PipelineEpochResult PipelineEpochSupervisor::acknowledge_ready(
    const PipelineEpochRegistration& registration) noexcept {
    std::lock_guard lock(mutex_);
    if (!ticket_matches_locked(registration.ticket)) {
        return stale_epoch_locked();
    }
    Participant* participant = find_participant_locked(registration);
    if (participant == nullptr) return stale_registration_locked();
    if (state_ != PipelineEpochState::building
        && state_ != PipelineEpochState::ready) {
        return result(PipelineEpochStatus::invalid_state);
    }
    const bool changed = !participant->ready;
    participant->ready = true;
    return result(PipelineEpochStatus::ok, changed);
}

PipelineEpochResult PipelineEpochSupervisor::mark_ready(
    const PipelineEpochTicket& ticket) noexcept {
    std::lock_guard lock(mutex_);
    if (!ticket_matches_locked(ticket)) return stale_epoch_locked();
    if (state_ != PipelineEpochState::building) {
        return result(PipelineEpochStatus::invalid_state);
    }
    state_ = PipelineEpochState::ready;
    return result(PipelineEpochStatus::ok, true);
}

PipelineEpochResult PipelineEpochSupervisor::resume(
    const PipelineEpochTicket& ticket) noexcept {
    std::lock_guard lock(mutex_);
    if (!ticket_matches_locked(ticket)) return stale_epoch_locked();
    if (state_ != PipelineEpochState::ready) {
        return result(PipelineEpochStatus::invalid_state);
    }
    if (pending_ready_locked() != 0) {
        return result(PipelineEpochStatus::pending_acknowledgements);
    }
    state_ = PipelineEpochState::running;
    first_frame_pending_ = true;
    if (recovery_successes_ != std::numeric_limits<std::uint64_t>::max()) {
        ++recovery_successes_;
    }
    return result(PipelineEpochStatus::ok, true);
}

PipelineEpochResult PipelineEpochSupervisor::frame_contract(
    const PipelineEpochTicket& ticket,
    PipelineEpochFrameContract& output) const noexcept {
    std::lock_guard lock(mutex_);
    if (!ticket_matches_locked(ticket)) {
        return result(PipelineEpochStatus::stale_epoch);
    }
    if (state_ != PipelineEpochState::running) {
        return result(PipelineEpochStatus::invalid_state);
    }
    output = {ticket_, first_frame_pending_, first_frame_pending_};
    return result(PipelineEpochStatus::ok);
}

PipelineEpochResult PipelineEpochSupervisor::acknowledge_frame_published(
    const PipelineEpochTicket& ticket) noexcept {
    std::lock_guard lock(mutex_);
    if (!ticket_matches_locked(ticket)) return stale_epoch_locked();
    if (state_ != PipelineEpochState::running) {
        return result(PipelineEpochStatus::invalid_state);
    }
    const bool changed = first_frame_pending_;
    first_frame_pending_ = false;
    return result(PipelineEpochStatus::ok, changed);
}

PipelineEpochSnapshot PipelineEpochSupervisor::snapshot() const noexcept {
    std::lock_guard lock(mutex_);
    return {
        state_,
        ticket_,
        participant_count_locked(),
        state_ == PipelineEpochState::quiescing
            ? pending_quiesce_locked()
            : 0,
        state_ == PipelineEpochState::building
                || state_ == PipelineEpochState::ready
            ? pending_ready_locked()
            : 0,
        recovery_attempts_,
        recovery_successes_,
        stale_acknowledgements_,
        last_device_lost_reason_,
        first_frame_pending_};
}

PipelineEpochTicket PipelineEpochSupervisor::ticket() const noexcept {
    std::lock_guard lock(mutex_);
    return ticket_;
}

PipelineEpochState PipelineEpochSupervisor::state() const noexcept {
    std::lock_guard lock(mutex_);
    return state_;
}

const char* pipeline_epoch_state_string(PipelineEpochState state) noexcept {
    switch (state) {
    case PipelineEpochState::running: return "running";
    case PipelineEpochState::quiescing: return "quiescing";
    case PipelineEpochState::retired: return "retired";
    case PipelineEpochState::building: return "building";
    case PipelineEpochState::ready: return "ready";
    default: return "unknown";
    }
}

const char* pipeline_epoch_status_string(PipelineEpochStatus status) noexcept {
    switch (status) {
    case PipelineEpochStatus::ok: return "ok";
    case PipelineEpochStatus::invalid_argument: return "invalid argument";
    case PipelineEpochStatus::invalid_state: return "invalid state";
    case PipelineEpochStatus::stale_epoch: return "stale epoch";
    case PipelineEpochStatus::stale_registration: return "stale registration";
    case PipelineEpochStatus::duplicate_participant:
        return "duplicate participant";
    case PipelineEpochStatus::participant_capacity_exceeded:
        return "participant capacity exceeded";
    case PipelineEpochStatus::registration_space_exhausted:
        return "registration space exhausted";
    case PipelineEpochStatus::pending_acknowledgements:
        return "pending acknowledgements";
    case PipelineEpochStatus::epoch_exhausted: return "epoch exhausted";
    default: return "unknown";
    }
}

} // namespace fluxcap::gpu::internal
