#include "pipeline_epoch_supervisor.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

namespace internal = fluxcap::gpu::internal;

static_assert(std::is_trivially_copyable_v<internal::PipelineEpochTicket>);
static_assert(std::is_trivially_copyable_v<internal::PipelineEpochRegistration>);
static_assert(std::is_trivially_copyable_v<internal::PipelineEpochFrameContract>);

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void require(
    internal::PipelineEpochResult result,
    const char* message) {
    require(static_cast<bool>(result), message);
}

void require_status(
    internal::PipelineEpochResult result,
    internal::PipelineEpochStatus expected,
    const char* message) {
    if (result.status != expected) throw std::runtime_error(message);
}

void test_recovery_epoch_and_first_frame_contract() {
    internal::PipelineEpochSupervisor supervisor(7, 0x12345678u);
    const internal::PipelineEpochTicket epoch7 = supervisor.ticket();
    require(epoch7.epoch == 7 && epoch7.nonce == 0x12345678u,
        "initial epoch ticket changed");

    internal::PipelineEpochFrameContract initial_contract;
    require_status(
        supervisor.frame_contract(epoch7, initial_contract),
        internal::PipelineEpochStatus::ok,
        "initial frame contract failed");
    require(!initial_contract.discontinuity && !initial_contract.force_keyframe,
        "initial epoch unexpectedly forced a recovery frame");

    internal::PipelineEpochRegistration stale_registration;
    require(supervisor.register_participant(11, stale_registration),
        "initial participant registration failed");
    require(supervisor.unregister_participant(stale_registration),
        "initial participant unregister failed");

    internal::PipelineEpochRegistration producer;
    internal::PipelineEpochRegistration child;
    require(supervisor.register_participant(11, producer),
        "replacement participant registration failed");
    require(supervisor.register_participant(22, child),
        "child participant registration failed");
    require(producer.registration_nonce != stale_registration.registration_nonce,
        "registration nonce was reused within one epoch");
    require_status(
        supervisor.acknowledge_quiesced(stale_registration),
        internal::PipelineEpochStatus::stale_registration,
        "disconnected participant ACK was accepted");

    const internal::PipelineEpochTicket forged_epoch{
        epoch7.epoch - 1,
        epoch7.nonce};
    require_status(
        supervisor.report_device_lost(forged_epoch, -1),
        internal::PipelineEpochStatus::stale_epoch,
        "stale device-lost report was accepted");
    require(supervisor.state() == internal::PipelineEpochState::running,
        "stale report changed state");

    const auto began = supervisor.report_device_lost(epoch7, -42);
    require(began && began.changed,
        "current device-lost report did not begin recovery");
    require(supervisor.state() == internal::PipelineEpochState::quiescing,
        "supervisor did not enter quiescing");
    const auto duplicate = supervisor.report_device_lost(epoch7, -43);
    require(duplicate && !duplicate.changed,
        "duplicate device-lost report was not idempotent");

    require(supervisor.acknowledge_quiesced(producer),
        "producer quiesce ACK failed");
    require_status(
        supervisor.retire(epoch7),
        internal::PipelineEpochStatus::pending_acknowledgements,
        "retire ignored a live child");
    require(supervisor.unregister_participant(child),
        "disconnect did not remove pending child ACK");
    require(supervisor.retire(epoch7), "retire failed after all participants left");
    require(supervisor.state() == internal::PipelineEpochState::retired,
        "supervisor did not enter retired");
    require_status(
        supervisor.acknowledge_quiesced(producer),
        internal::PipelineEpochStatus::stale_registration,
        "post-retirement ACK was accepted");

    internal::PipelineEpochTicket epoch8;
    require(supervisor.begin_build(epoch7, epoch8), "begin_build failed");
    require(epoch8.epoch == 8 && epoch8.nonce != 0
            && epoch8.nonce != epoch7.nonce,
        "new build did not receive a fresh epoch ticket");
    require(supervisor.state() == internal::PipelineEpochState::building,
        "supervisor did not enter building");
    require_status(
        supervisor.report_device_lost(epoch7, -44),
        internal::PipelineEpochStatus::stale_epoch,
        "old worker failure poisoned the new build");

    internal::PipelineEpochRegistration rebuilt_child;
    require(supervisor.register_participant(22, rebuilt_child),
        "rebuilt child registration failed");
    require(supervisor.mark_ready(epoch8), "mark_ready failed");
    require(supervisor.state() == internal::PipelineEpochState::ready,
        "supervisor did not enter ready");
    require_status(
        supervisor.resume(epoch8),
        internal::PipelineEpochStatus::pending_acknowledgements,
        "resume ignored child readiness");
    require_status(
        supervisor.acknowledge_ready(child),
        internal::PipelineEpochStatus::stale_epoch,
        "old child readiness ACK was accepted");
    const auto ready = supervisor.acknowledge_ready(rebuilt_child);
    require(ready && ready.changed, "rebuilt child readiness ACK failed");
    const auto duplicate_ready = supervisor.acknowledge_ready(rebuilt_child);
    require(duplicate_ready && !duplicate_ready.changed,
        "duplicate readiness ACK was not idempotent");
    require(supervisor.resume(epoch8), "resume failed after readiness ACK");

    internal::PipelineEpochFrameContract recovered_contract;
    require(supervisor.frame_contract(epoch8, recovered_contract),
        "recovered frame contract failed");
    require(recovered_contract.discontinuity
            && recovered_contract.force_keyframe,
        "first recovered frame was not discontinuous and forced-keyframe");
    internal::PipelineEpochFrameContract repeated_contract;
    require(supervisor.frame_contract(epoch8, repeated_contract),
        "repeated recovered frame contract failed");
    require(repeated_contract.discontinuity && repeated_contract.force_keyframe,
        "reading the contract consumed it before publish");
    require_status(
        supervisor.acknowledge_frame_published(epoch7),
        internal::PipelineEpochStatus::stale_epoch,
        "old frame completion consumed the new contract");
    require(supervisor.frame_contract(epoch8, repeated_contract)
            && repeated_contract.discontinuity,
        "stale completion cleared the new frame contract");
    const auto published = supervisor.acknowledge_frame_published(epoch8);
    require(published && published.changed,
        "first recovered frame publish ACK failed");
    require(supervisor.frame_contract(epoch8, repeated_contract)
            && !repeated_contract.discontinuity
            && !repeated_contract.force_keyframe,
        "recovery contract remained armed after publish");

    const auto snapshot = supervisor.snapshot();
    require(snapshot.state == internal::PipelineEpochState::running,
        "final supervisor state is not running");
    require(snapshot.recovery_attempts == 1
            && snapshot.recovery_successes == 1,
        "recovery counters are inconsistent");
    require(snapshot.stale_acknowledgements >= 5,
        "stale protocol messages were not counted");
    require(snapshot.last_device_lost_reason == -42,
        "duplicate device-lost report overwrote root cause");
}

void test_capacity_and_transition_guards() {
    internal::PipelineEpochSupervisor supervisor(1, 99);
    const auto epoch = supervisor.ticket();

    require_status(
        supervisor.retire(epoch),
        internal::PipelineEpochStatus::invalid_state,
        "retire skipped quiescing");
    require_status(
        supervisor.mark_ready(epoch),
        internal::PipelineEpochStatus::invalid_state,
        "mark_ready skipped building");

    std::vector<internal::PipelineEpochRegistration> registrations(
        internal::PipelineEpochSupervisor::max_participants);
    for (std::uint32_t index = 0; index != registrations.size(); ++index) {
        require(supervisor.register_participant(index + 1, registrations[index]),
            "participant capacity was smaller than advertised");
    }
    internal::PipelineEpochRegistration overflow;
    require_status(
        supervisor.register_participant(1000, overflow),
        internal::PipelineEpochStatus::participant_capacity_exceeded,
        "participant capacity was not bounded");
    require_status(
        supervisor.register_participant(1, overflow),
        internal::PipelineEpochStatus::duplicate_participant,
        "duplicate participant id was accepted");
}

void test_concurrent_acknowledgements() {
    internal::PipelineEpochSupervisor supervisor(3, 123);
    const auto epoch3 = supervisor.ticket();
    constexpr std::uint32_t participant_count = 16;
    std::vector<internal::PipelineEpochRegistration> registrations(
        participant_count);
    for (std::uint32_t index = 0; index != participant_count; ++index) {
        require(supervisor.register_participant(index + 1, registrations[index]),
            "concurrent test registration failed");
    }
    require(supervisor.report_device_lost(epoch3, -10),
        "concurrent test recovery did not begin");

    std::vector<std::thread> threads;
    for (const auto registration : registrations) {
        threads.emplace_back([&supervisor, registration] {
            for (std::uint32_t repeat = 0; repeat != 100; ++repeat) {
                const auto acknowledged =
                    supervisor.acknowledge_quiesced(registration);
                if (!acknowledged) std::terminate();
            }
        });
    }
    for (auto& thread : threads) thread.join();
    require(supervisor.snapshot().pending_quiesce_acknowledgements == 0,
        "concurrent idempotent ACKs left participants pending");
    require(supervisor.retire(epoch3), "concurrent test retire failed");
}

void test_epoch_exhaustion_is_terminally_safe() {
    internal::PipelineEpochSupervisor supervisor(
        std::numeric_limits<std::uint64_t>::max(),
        77);
    const auto epoch = supervisor.ticket();
    require(supervisor.report_device_lost(epoch, -9),
        "epoch exhaustion recovery did not enter quiescing");
    require(supervisor.retire(epoch),
        "epoch exhaustion recovery did not retire");
    internal::PipelineEpochTicket output{44, 55};
    require_status(
        supervisor.begin_build(epoch, output),
        internal::PipelineEpochStatus::epoch_exhausted,
        "epoch wraparound was permitted");
    require(supervisor.state() == internal::PipelineEpochState::retired,
        "epoch exhaustion changed retired state");
    require(output == internal::PipelineEpochTicket{44, 55},
        "failed begin_build modified its output");
}

void test_abandoned_build_can_retry_with_stale_ack_isolation() {
    internal::PipelineEpochSupervisor supervisor(20, 0x2020u);
    const auto running = supervisor.ticket();
    require_status(
        supervisor.abandon_build(running),
        internal::PipelineEpochStatus::invalid_state,
        "running epoch was abandoned");
    require(supervisor.report_device_lost(running, -20),
        "retry test recovery did not begin");
    require(supervisor.retire(running),
        "retry test recovery did not retire");
    require_status(
        supervisor.abandon_build(running),
        internal::PipelineEpochStatus::invalid_state,
        "retired epoch was abandoned");

    internal::PipelineEpochTicket abandoned_ticket;
    require(supervisor.begin_build(running, abandoned_ticket),
        "first retry build did not begin");
    internal::PipelineEpochRegistration abandoned_participant;
    require(supervisor.register_participant(41, abandoned_participant),
        "abandoned build participant registration failed");

    const auto abandoned = supervisor.abandon_build(abandoned_ticket);
    require(abandoned && abandoned.changed,
        "building epoch was not abandoned");
    const auto retired_snapshot = supervisor.snapshot();
    require(retired_snapshot.state == internal::PipelineEpochState::retired
            && retired_snapshot.registered_participants == 0,
        "abandon_build retained build participants or wrong state");

    internal::PipelineEpochTicket retry_ticket;
    require(supervisor.begin_build(abandoned_ticket, retry_ticket),
        "build retry did not begin");
    require(retry_ticket.epoch == abandoned_ticket.epoch + 1
            && retry_ticket.nonce != 0
            && retry_ticket.nonce != abandoned_ticket.nonce,
        "build retry did not receive a fresh ticket");
    require_status(
        supervisor.acknowledge_ready(abandoned_participant),
        internal::PipelineEpochStatus::stale_epoch,
        "late readiness ACK from abandoned build was accepted");

    internal::PipelineEpochRegistration retry_participant;
    require(supervisor.register_participant(41, retry_participant),
        "retry participant could not reuse abandoned participant id");
    require(supervisor.mark_ready(retry_ticket),
        "retry build did not enter ready");
    require_status(
        supervisor.acknowledge_ready(abandoned_participant),
        internal::PipelineEpochStatus::stale_epoch,
        "late abandoned ACK affected ready retry build");
    require(supervisor.acknowledge_ready(retry_participant),
        "retry participant readiness ACK failed");

    const auto ready_abandoned = supervisor.abandon_build(retry_ticket);
    require(ready_abandoned && ready_abandoned.changed,
        "ready epoch was not abandoned");
    require(supervisor.snapshot().registered_participants == 0,
        "ready abandon retained build participants");

    internal::PipelineEpochTicket second_retry_ticket;
    require(supervisor.begin_build(retry_ticket, second_retry_ticket),
        "second build retry did not begin");
    require_status(
        supervisor.acknowledge_ready(retry_participant),
        internal::PipelineEpochStatus::stale_epoch,
        "late ACK from abandoned ready build was accepted");
    require(supervisor.snapshot().stale_acknowledgements == 3,
        "abandoned build stale ACKs were not counted");
}

} // namespace

int main() {
    try {
        test_recovery_epoch_and_first_frame_contract();
        test_capacity_and_transition_guards();
        test_concurrent_acknowledgements();
        test_epoch_exhaustion_is_terminally_safe();
        test_abandoned_build_can_retry_with_stale_ack_isolation();
        std::cout << "pipeline epoch supervisor tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pipeline epoch supervisor test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
