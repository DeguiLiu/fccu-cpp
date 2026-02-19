/**
 * @file fccu_hsm.hpp
 * @brief Two-layer HSM definitions for FCCU state management.
 *
 * Layer 1: Global FCCU HSM (Idle/Active/Degraded/Shutdown)
 *   Controls system-level fault state and admission policies.
 *
 * Layer 2: Per-Fault HSM (Dormant/Detected/Active/Recovering/Cleared)
 *   Manages individual critical fault lifecycles (optional, max 8).
 *
 * Uses hsm-cpp library for hierarchical state machine with LCA transitions.
 */

#ifndef FCCU_FCCU_HSM_HPP_
#define FCCU_FCCU_HSM_HPP_

#include "hsm/state_machine.hpp"

#include <cstdint>

namespace fccu {

// ============================================================================
// Event IDs
// ============================================================================

namespace evt {

// Global FCCU HSM events
static constexpr uint32_t kFaultReported = 1U;     ///< First fault reported
static constexpr uint32_t kAllCleared = 2U;        ///< All faults cleared
static constexpr uint32_t kCriticalDetected = 3U;  ///< Critical-level fault detected
static constexpr uint32_t kShutdownReq = 4U;       ///< Shutdown requested by hook
static constexpr uint32_t kDegradeRecovered = 5U;  ///< No more critical faults

// Per-Fault HSM events
static constexpr uint32_t kDetected = 10U;       ///< Fault occurrence detected
static constexpr uint32_t kConfirmed = 11U;      ///< Threshold reached, fault confirmed
static constexpr uint32_t kRecoveryStart = 12U;  ///< Recovery attempt initiated
static constexpr uint32_t kRecoveryDone = 13U;   ///< Recovery completed successfully
static constexpr uint32_t kClearFault = 14U;     ///< Fault cleared by user/system

}  // namespace evt

// ============================================================================
// Global FCCU HSM - System-level fault state machine
// ============================================================================

/**
 * @brief Context for the global FCCU state machine.
 *
 * Tracks aggregate fault metrics that drive state transitions.
 */
struct GlobalHsmContext {
  uint32_t active_count = 0U;       ///< Number of currently active faults
  uint32_t critical_count = 0U;     ///< Number of active critical faults
  bool shutdown_requested = false;  ///< Shutdown flag
};

/**
 * @brief Global FCCU hierarchical state machine.
 *
 * States:
 *   Idle     -> no active faults
 *   Active   -> faults present, normal processing
 *   Degraded -> critical faults detected, restricted admission
 *   Shutdown -> system shutdown requested
 *
 * Transition diagram:
 *   Idle ──FaultReported──> Active
 *   Active ──AllCleared──> Idle
 *   Active ──CriticalDetected──> Degraded
 *   Active ──ShutdownReq──> Shutdown
 *   Degraded ──DegradeRecovered──> Active
 *   Degraded ──ShutdownReq──> Shutdown
 */
class GlobalHsm {
 public:
  GlobalHsm() : sm_(idle_, ctx_) { Setup(); }

  /** @brief Dispatch an event to the state machine. */
  bool Dispatch(uint32_t event_id) noexcept { return sm_.Dispatch(hsm::Event(event_id)); }

  // --- State queries ---
  bool IsIdle() const noexcept { return sm_.IsInState(idle_); }
  bool IsActive() const noexcept { return sm_.IsInState(active_); }
  bool IsDegraded() const noexcept { return sm_.IsInState(degraded_); }
  bool IsShutdown() const noexcept { return sm_.IsInState(shutdown_); }

  const char* CurrentStateName() const noexcept { return sm_.current_state_name(); }

  GlobalHsmContext& context() noexcept { return ctx_; }
  const GlobalHsmContext& context() const noexcept { return ctx_; }

  /** @brief Reset to initial state (Idle). */
  void Reset() noexcept {
    ctx_ = GlobalHsmContext{};
    sm_.Reset();
  }

 private:
  void Setup() {
    // Idle -> Active: when first fault is reported
    idle_.AddTransition(evt::kFaultReported, active_, [](GlobalHsmContext& /*ctx*/, const hsm::Event& /*e*/) {});

    // Active -> Idle: when all faults cleared
    active_.AddTransition(evt::kAllCleared, idle_, [](GlobalHsmContext& ctx, const hsm::Event& /*e*/) {
      ctx.active_count = 0U;
      ctx.critical_count = 0U;
    });

    // Active -> Degraded: when critical fault detected
    active_.AddTransition(evt::kCriticalDetected, degraded_);

    // Active -> Shutdown: on shutdown request
    active_.AddTransition(evt::kShutdownReq, shutdown_,
                          [](GlobalHsmContext& ctx, const hsm::Event& /*e*/) { ctx.shutdown_requested = true; });

    // Degraded -> Active: when all critical faults resolved
    degraded_.AddTransition(evt::kDegradeRecovered, active_);

    // Degraded -> Shutdown: on shutdown request
    degraded_.AddTransition(evt::kShutdownReq, shutdown_,
                            [](GlobalHsmContext& ctx, const hsm::Event& /*e*/) { ctx.shutdown_requested = true; });
  }

  GlobalHsmContext ctx_;
  hsm::State<GlobalHsmContext> idle_{"Idle"};
  hsm::State<GlobalHsmContext> active_{"Active"};
  hsm::State<GlobalHsmContext> degraded_{"Degraded"};
  hsm::State<GlobalHsmContext> shutdown_{"Shutdown"};
  hsm::StateMachine<GlobalHsmContext> sm_;
};

// ============================================================================
// Per-Fault HSM - Individual fault lifecycle state machine
// ============================================================================

/**
 * @brief Context for a per-fault state machine.
 */
struct PerFaultContext {
  uint16_t fault_index = 0U;       ///< Associated fault index
  uint32_t occurrence_count = 0U;  ///< Cumulative occurrence count
  uint32_t err_threshold = 1U;     ///< Threshold for Detected->Active transition
};

/**
 * @brief Per-fault hierarchical state machine.
 *
 * Manages the lifecycle of an individual critical fault.
 *
 * States:
 *   Dormant    -> fault not active
 *   Detected   -> fault reported but below threshold
 *   Active     -> fault confirmed (threshold reached)
 *   Recovering -> recovery in progress
 *   Cleared    -> fault resolved
 *
 * Transition diagram:
 *   Dormant ──Detected──> Detected
 *   Detected ──Confirmed──> Active
 *   Detected ──ClearFault──> Cleared
 *   Active ──RecoveryStart──> Recovering
 *   Active ──ClearFault──> Cleared
 *   Recovering ──RecoveryDone──> Cleared
 *   Cleared ──ClearFault──> Dormant
 */
class PerFaultHsm {
 public:
  PerFaultHsm() : sm_(dormant_, ctx_) { Setup(); }

  /** @brief Dispatch an event to the state machine. */
  bool Dispatch(uint32_t event_id) noexcept { return sm_.Dispatch(hsm::Event(event_id)); }

  // --- State queries ---
  bool IsDormant() const noexcept { return sm_.IsInState(dormant_); }
  bool IsDetected() const noexcept { return sm_.IsInState(detected_); }
  bool IsActive() const noexcept { return sm_.IsInState(active_); }
  bool IsRecovering() const noexcept { return sm_.IsInState(recovering_); }
  bool IsCleared() const noexcept { return sm_.IsInState(cleared_); }

  const char* CurrentStateName() const noexcept { return sm_.current_state_name(); }

  PerFaultContext& context() noexcept { return ctx_; }
  const PerFaultContext& context() const noexcept { return ctx_; }

  /** @brief Bind this HSM to a specific fault index and threshold. */
  void Bind(uint16_t fault_index, uint32_t threshold = 1U) noexcept {
    ctx_.fault_index = fault_index;
    ctx_.err_threshold = threshold;
    ctx_.occurrence_count = 0U;
    sm_.Reset();
  }

  /** @brief Reset to Dormant state. */
  void Reset() noexcept {
    ctx_.occurrence_count = 0U;
    sm_.Reset();
  }

 private:
  void Setup() {
    // Dormant -> Detected: fault occurrence detected
    dormant_.AddTransition(evt::kDetected, detected_,
                           [](PerFaultContext& ctx, const hsm::Event& /*e*/) { ctx.occurrence_count = 1U; });

    // Detected -> Active: threshold reached (auto-confirm via guard)
    detected_.AddTransition(
        evt::kConfirmed, active_,
        // Guard: only transition if threshold reached
        [](const PerFaultContext& ctx, const hsm::Event& /*e*/) -> bool {
          return ctx.occurrence_count >= ctx.err_threshold;
        },
        // Action: (none needed)
        [](PerFaultContext& /*ctx*/, const hsm::Event& /*e*/) {});

    // Detected: internal transition for additional detections
    detected_.AddInternalTransition(evt::kDetected,
                                    [](PerFaultContext& ctx, const hsm::Event& /*e*/) { ++ctx.occurrence_count; });

    // Detected -> Cleared: fault cleared before confirmation
    detected_.AddTransition(evt::kClearFault, cleared_);

    // Active -> Recovering: recovery initiated
    active_.AddTransition(evt::kRecoveryStart, recovering_);

    // Active -> Cleared: direct clear
    active_.AddTransition(evt::kClearFault, cleared_);

    // Recovering -> Cleared: recovery succeeded
    recovering_.AddTransition(evt::kRecoveryDone, cleared_);

    // Cleared -> Dormant: reset to dormant state
    cleared_.AddTransition(evt::kClearFault, dormant_,
                           [](PerFaultContext& ctx, const hsm::Event& /*e*/) { ctx.occurrence_count = 0U; });
  }

  PerFaultContext ctx_;
  hsm::State<PerFaultContext> dormant_{"Dormant"};
  hsm::State<PerFaultContext> detected_{"Detected"};
  hsm::State<PerFaultContext> active_{"Active"};
  hsm::State<PerFaultContext> recovering_{"Recovering"};
  hsm::State<PerFaultContext> cleared_{"Cleared"};
  hsm::StateMachine<PerFaultContext> sm_;
};

}  // namespace fccu

#endif  // FCCU_FCCU_HSM_HPP_
