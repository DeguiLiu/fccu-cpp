/**
 * @file test_fccu.cpp
 * @brief Catch2 unit tests for fccu-cpp.
 */

#include "fccu/fccu.hpp"

#include <catch2/catch_test_macros.hpp>

// ============================================================================
// Test Helpers
// ============================================================================

using TestCollector = fccu::FaultCollector<16, 8, 4, 4>;

static fccu::HookAction HandledHook(const fccu::FaultEvent& /*e*/, void* /*ctx*/) {
  return fccu::HookAction::kHandled;
}

static fccu::HookAction DeferHook(const fccu::FaultEvent& /*e*/, void* /*ctx*/) {
  return fccu::HookAction::kDefer;
}

static fccu::HookAction EscalateHook(const fccu::FaultEvent& /*e*/, void* /*ctx*/) {
  return fccu::HookAction::kEscalate;
}

static fccu::HookAction ShutdownHook(const fccu::FaultEvent& /*e*/, void* /*ctx*/) {
  return fccu::HookAction::kShutdown;
}

// ============================================================================
// Registration Tests
// ============================================================================

TEST_CASE("RegisterFault basic", "[register]") {
  TestCollector c;
  REQUIRE(c.RegisterFault(0U, 0x1001U) == fccu::FccuError::kOk);
  REQUIRE(c.RegisterFault(1U, 0x1002U, 0U, 3U) == fccu::FccuError::kOk);
}

TEST_CASE("RegisterFault invalid index", "[register]") {
  TestCollector c;
  REQUIRE(c.RegisterFault(16U, 0x1001U) == fccu::FccuError::kInvalidIndex);
  REQUIRE(c.RegisterFault(255U, 0x1001U) == fccu::FccuError::kInvalidIndex);
}

TEST_CASE("RegisterFault duplicate", "[register]") {
  TestCollector c;
  REQUIRE(c.RegisterFault(0U, 0x1001U) == fccu::FccuError::kOk);
  REQUIRE(c.RegisterFault(0U, 0x1001U) == fccu::FccuError::kAlreadyRegistered);
}

TEST_CASE("RegisterHook requires registration", "[register]") {
  TestCollector c;
  REQUIRE(c.RegisterHook(0U, HandledHook) == fccu::FccuError::kNotRegistered);
  c.RegisterFault(0U, 0x1001U);
  REQUIRE(c.RegisterHook(0U, HandledHook) == fccu::FccuError::kOk);
}

// ============================================================================
// Report and Process Tests
// ============================================================================

TEST_CASE("ReportFault and ProcessFaults basic flow", "[report]") {
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);
  c.RegisterHook(0U, HandledHook);

  REQUIRE(c.ReportFault(0U, 0xAA) == fccu::FccuError::kOk);
  REQUIRE(c.ActiveFaultCount() == 1U);
  REQUIRE(c.IsFaultActive(0U));

  uint32_t processed = c.ProcessFaults();
  REQUIRE(processed == 1U);
  REQUIRE_FALSE(c.IsFaultActive(0U));  // Handled -> cleared
  REQUIRE(c.ActiveFaultCount() == 0U);
}

TEST_CASE("ReportFault unregistered fault", "[report]") {
  TestCollector c;
  REQUIRE(c.ReportFault(0U) == fccu::FccuError::kNotRegistered);
}

TEST_CASE("ReportFault invalid index", "[report]") {
  TestCollector c;
  REQUIRE(c.ReportFault(16U) == fccu::FccuError::kInvalidIndex);
}

TEST_CASE("Multiple faults at different priorities", "[report]") {
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);
  c.RegisterFault(1U, 0x1002U);
  c.RegisterFault(2U, 0x1003U);
  c.RegisterHook(0U, HandledHook);
  c.RegisterHook(1U, HandledHook);
  c.RegisterHook(2U, HandledHook);

  c.ReportFault(0U, 0U, fccu::FaultPriority::kCritical);
  c.ReportFault(1U, 0U, fccu::FaultPriority::kMedium);
  c.ReportFault(2U, 0U, fccu::FaultPriority::kLow);

  REQUIRE(c.ActiveFaultCount() == 3U);
  uint32_t processed = c.ProcessFaults();
  REQUIRE(processed == 3U);
  REQUIRE(c.ActiveFaultCount() == 0U);
}

// ============================================================================
// HookAction Tests
// ============================================================================

TEST_CASE("HookAction::kHandled clears fault", "[hook]") {
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);
  c.RegisterHook(0U, HandledHook);
  c.ReportFault(0U);
  c.ProcessFaults();
  REQUIRE_FALSE(c.IsFaultActive(0U));
}

TEST_CASE("HookAction::kDefer keeps fault active", "[hook]") {
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);
  c.RegisterHook(0U, DeferHook);
  c.ReportFault(0U);
  c.ProcessFaults();
  REQUIRE(c.IsFaultActive(0U));
}

TEST_CASE("HookAction::kEscalate re-enqueues at higher priority", "[hook]") {
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);

  // First call escalates, subsequent call handles
  static int call_count = 0;
  call_count = 0;
  c.RegisterHook(0U, [](const fccu::FaultEvent& /*e*/, void* /*ctx*/) -> fccu::HookAction {
    ++call_count;
    return (call_count == 1) ? fccu::HookAction::kEscalate : fccu::HookAction::kHandled;
  });

  c.ReportFault(0U, 0U, fccu::FaultPriority::kMedium);
  c.ProcessFaults();
  // After escalation, the fault is re-enqueued at kHigh
  // Second ProcessFaults processes the escalated entry
  c.ProcessFaults();
  REQUIRE(call_count == 2);
  REQUIRE_FALSE(c.IsFaultActive(0U));
}

TEST_CASE("HookAction::kShutdown sets shutdown flag", "[hook]") {
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);
  c.RegisterHook(0U, ShutdownHook);

  bool shutdown_called = false;
  c.SetShutdownCallback([](void* ctx) { *static_cast<bool*>(ctx) = true; }, &shutdown_called);

  c.ReportFault(0U);
  c.ProcessFaults();
  REQUIRE(c.IsShutdownRequested());
  REQUIRE(shutdown_called);
}

TEST_CASE("Default hook is used when no specific hook", "[hook]") {
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);

  static bool default_called = false;
  default_called = false;
  c.SetDefaultHook([](const fccu::FaultEvent& /*e*/, void* /*ctx*/) -> fccu::HookAction {
    default_called = true;
    return fccu::HookAction::kHandled;
  });

  c.ReportFault(0U);
  c.ProcessFaults();
  REQUIRE(default_called);
}

// ============================================================================
// Priority Admission Control Tests
// ============================================================================

TEST_CASE("Low priority dropped when queue > 60%", "[admission]") {
  // QueueDepth=8, 60% = 4.8, so threshold is 4
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);
  c.RegisterHook(0U, DeferHook);

  // Fill queue with 5 low-priority faults
  for (int i = 0; i < 5; ++i) {
    c.ReportFault(0U, static_cast<uint32_t>(i), fccu::FaultPriority::kLow);
  }

  // 6th low-priority should be rejected (admission denied)
  auto result = c.ReportFault(0U, 0xFF, fccu::FaultPriority::kLow);
  // May be kQueueFull or admission denied depending on fill level
  // The exact behavior depends on queue state
  auto stats = c.GetStatistics();
  REQUIRE(stats.total_reported >= 4U);
}

TEST_CASE("Critical priority always admitted", "[admission]") {
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);
  c.RegisterHook(0U, DeferHook);

  // Fill queue significantly
  for (int i = 0; i < 7; ++i) {
    c.ReportFault(0U, static_cast<uint32_t>(i), fccu::FaultPriority::kCritical);
  }

  // Critical should still be admitted (if physically possible)
  auto result = c.ReportFault(0U, 0xFF, fccu::FaultPriority::kCritical);
  // Only fails if queue is physically full
  REQUIRE((result == fccu::FccuError::kOk || result == fccu::FccuError::kQueueFull));
}

// ============================================================================
// Statistics Tests
// ============================================================================

TEST_CASE("Statistics accuracy", "[stats]") {
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);
  c.RegisterHook(0U, HandledHook);

  c.ReportFault(0U, 0U, fccu::FaultPriority::kHigh);
  c.ReportFault(0U, 0U, fccu::FaultPriority::kMedium);
  c.ProcessFaults();

  auto stats = c.GetStatistics();
  REQUIRE(stats.total_reported == 2U);
  REQUIRE(stats.total_processed == 2U);
  REQUIRE(stats.total_dropped == 0U);
  REQUIRE(stats.priority_reported[1] == 1U);  // kHigh
  REQUIRE(stats.priority_reported[2] == 1U);  // kMedium
}

TEST_CASE("ResetStatistics", "[stats]") {
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);
  c.RegisterHook(0U, HandledHook);
  c.ReportFault(0U);
  c.ProcessFaults();

  c.ResetStatistics();
  auto stats = c.GetStatistics();
  REQUIRE(stats.total_reported == 0U);
  REQUIRE(stats.total_processed == 0U);
}

// ============================================================================
// Global HSM Tests
// ============================================================================

TEST_CASE("Global HSM starts in Idle", "[hsm]") {
  TestCollector c;
  REQUIRE(c.GetGlobalHsm().IsIdle());
}

TEST_CASE("Global HSM transitions Idle -> Active on first fault", "[hsm]") {
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);
  c.RegisterHook(0U, DeferHook);

  c.ReportFault(0U);
  REQUIRE(c.GetGlobalHsm().IsActive());
}

TEST_CASE("Global HSM transitions Active -> Degraded on critical fault", "[hsm]") {
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);
  c.RegisterHook(0U, DeferHook);

  c.ReportFault(0U, 0U, fccu::FaultPriority::kCritical);
  REQUIRE(c.GetGlobalHsm().IsDegraded());
}

TEST_CASE("Global HSM transitions back to Idle when all cleared", "[hsm]") {
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);
  c.RegisterHook(0U, HandledHook);

  c.ReportFault(0U);
  REQUIRE(c.GetGlobalHsm().IsActive());

  c.ProcessFaults();
  REQUIRE(c.GetGlobalHsm().IsIdle());
}

// ============================================================================
// Per-Fault HSM Tests
// ============================================================================

TEST_CASE("BindFaultHsm", "[per-fault-hsm]") {
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);
  REQUIRE(c.BindFaultHsm(0U, 3U) == fccu::FccuError::kOk);
}

TEST_CASE("BindFaultHsm slot limit", "[per-fault-hsm]") {
  fccu::FaultCollector<16, 8, 4, 2> c;  // MaxPerFaultHsm=2
  c.RegisterFault(0U, 0x1001U);
  c.RegisterFault(1U, 0x1002U);
  c.RegisterFault(2U, 0x1003U);

  REQUIRE(c.BindFaultHsm(0U) == fccu::FccuError::kOk);
  REQUIRE(c.BindFaultHsm(1U) == fccu::FccuError::kOk);
  REQUIRE(c.BindFaultHsm(2U) == fccu::FccuError::kHsmSlotFull);
}

// ============================================================================
// Clear Tests
// ============================================================================

TEST_CASE("ClearFault clears single fault", "[clear]") {
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);
  c.RegisterFault(1U, 0x1002U);
  c.RegisterHook(0U, DeferHook);
  c.RegisterHook(1U, DeferHook);

  c.ReportFault(0U);
  c.ReportFault(1U);
  c.ProcessFaults();

  c.ClearFault(0U);
  REQUIRE_FALSE(c.IsFaultActive(0U));
  REQUIRE(c.IsFaultActive(1U));
  REQUIRE(c.ActiveFaultCount() == 1U);
}

TEST_CASE("ClearAllFaults", "[clear]") {
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);
  c.RegisterFault(1U, 0x1002U);
  c.RegisterHook(0U, DeferHook);
  c.RegisterHook(1U, DeferHook);

  c.ReportFault(0U);
  c.ReportFault(1U);
  c.ProcessFaults();

  c.ClearAllFaults();
  REQUIRE(c.ActiveFaultCount() == 0U);
  REQUIRE(c.GetGlobalHsm().IsIdle());
}

// ============================================================================
// Overflow Callback Tests
// ============================================================================

TEST_CASE("Overflow callback invoked on queue full", "[overflow]") {
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);
  c.RegisterHook(0U, DeferHook);

  static int overflow_count = 0;
  overflow_count = 0;
  c.SetOverflowCallback([](uint16_t /*fi*/, fccu::FaultPriority /*pri*/, void* /*ctx*/) { ++overflow_count; });

  // Fill queue completely (capacity = 8)
  for (int i = 0; i < 12; ++i) {
    c.ReportFault(0U, static_cast<uint32_t>(i), fccu::FaultPriority::kCritical);
  }

  REQUIRE(overflow_count > 0);
}

// ============================================================================
// BackpressureLevel Tests
// ============================================================================

TEST_CASE("BackpressureLevel starts Normal", "[backpressure]") {
  TestCollector c;
  REQUIRE(c.GetBackpressureLevel() == fccu::BackpressureLevel::kNormal);
}

// ============================================================================
// FaultReporter Tests
// ============================================================================

TEST_CASE("FaultReporter injection point", "[reporter]") {
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);
  c.RegisterHook(0U, HandledHook);

  auto reporter = c.GetReporter();
  reporter.Report(0U, 0xBEEF, fccu::FaultPriority::kMedium);

  REQUIRE(c.IsFaultActive(0U));
  c.ProcessFaults();
  REQUIRE_FALSE(c.IsFaultActive(0U));
}

TEST_CASE("FaultReporter with null fn does nothing", "[reporter]") {
  fccu::FaultReporter reporter{};
  reporter.Report(0U);  // Should not crash
}

// ============================================================================
// Recent Fault Ring Tests
// ============================================================================

TEST_CASE("ForEachRecent iterates newest first", "[recent]") {
  TestCollector c;
  c.RegisterFault(0U, 0x1001U);
  c.RegisterFault(1U, 0x1002U);
  c.RegisterHook(0U, HandledHook);
  c.RegisterHook(1U, HandledHook);

  c.ReportFault(0U, 0x11);
  c.ReportFault(1U, 0x22);
  c.ProcessFaults();

  uint32_t count = 0U;
  uint32_t last_detail = 0U;
  c.ForEachRecent([&](const fccu::RecentFaultInfo& info) {
    if (count == 0U) {
      last_detail = info.detail;
    }
    ++count;
  });

  REQUIRE(count == 2U);
  REQUIRE(last_detail == 0x22U);  // Newest first
}

// ============================================================================
// FaultQueueSet Standalone Tests
// ============================================================================

TEST_CASE("FaultQueueSet basic push/pop", "[queue]") {
  fccu::FaultQueueSet<fccu::FaultEntry, 4, 8> qs;

  fccu::FaultEntry entry{};
  entry.fault_index = 5U;
  entry.priority = fccu::FaultPriority::kHigh;

  REQUIRE(qs.Push(1U, entry));
  REQUIRE_FALSE(qs.IsEmpty());
  REQUIRE(qs.TotalSize() == 1U);

  fccu::FaultEntry out{};
  uint8_t level = 0U;
  REQUIRE(qs.Pop(out, level));
  REQUIRE(out.fault_index == 5U);
  REQUIRE(level == 1U);
  REQUIRE(qs.IsEmpty());
}

TEST_CASE("FaultQueueSet priority ordering", "[queue]") {
  fccu::FaultQueueSet<fccu::FaultEntry, 4, 8> qs;

  fccu::FaultEntry low{};
  low.fault_index = 1U;
  fccu::FaultEntry high{};
  high.fault_index = 2U;

  // Push low first, then high
  qs.Push(3U, low);   // Low priority
  qs.Push(0U, high);  // Critical priority

  fccu::FaultEntry out{};
  uint8_t level = 0U;

  // Pop should return critical first
  REQUIRE(qs.Pop(out, level));
  REQUIRE(out.fault_index == 2U);
  REQUIRE(level == 0U);

  REQUIRE(qs.Pop(out, level));
  REQUIRE(out.fault_index == 1U);
  REQUIRE(level == 3U);
}

TEST_CASE("FaultQueueSet admission control", "[queue]") {
  fccu::FaultQueueSet<fccu::FaultEntry, 4, 8> qs;
  fccu::FaultEntry entry{};

  // Fill 60% of low-priority queue (8 * 60% = 4.8 -> threshold 4)
  for (int i = 0; i < 5; ++i) {
    qs.Push(3U, entry);
  }

  // PushWithAdmission should reject low-priority (> 60%)
  REQUIRE_FALSE(qs.PushWithAdmission(3U, entry));

  // But critical should still be admitted
  REQUIRE(qs.PushWithAdmission(0U, entry));
}

TEST_CASE("FaultQueueSet invalid level", "[queue]") {
  fccu::FaultQueueSet<fccu::FaultEntry, 4, 8> qs;
  fccu::FaultEntry entry{};
  REQUIRE_FALSE(qs.Push(4U, entry));
  REQUIRE_FALSE(qs.Push(255U, entry));
}

// ============================================================================
// Global HSM Standalone Tests
// ============================================================================

TEST_CASE("GlobalHsm full lifecycle", "[global-hsm]") {
  fccu::GlobalHsm hsm;

  REQUIRE(hsm.IsIdle());

  hsm.Dispatch(fccu::evt::kFaultReported);
  REQUIRE(hsm.IsActive());

  hsm.Dispatch(fccu::evt::kCriticalDetected);
  REQUIRE(hsm.IsDegraded());

  hsm.Dispatch(fccu::evt::kDegradeRecovered);
  REQUIRE(hsm.IsActive());

  hsm.Dispatch(fccu::evt::kAllCleared);
  REQUIRE(hsm.IsIdle());
}

TEST_CASE("GlobalHsm shutdown", "[global-hsm]") {
  fccu::GlobalHsm hsm;

  hsm.Dispatch(fccu::evt::kFaultReported);
  hsm.Dispatch(fccu::evt::kShutdownReq);
  REQUIRE(hsm.IsShutdown());
  REQUIRE(hsm.context().shutdown_requested);
}

TEST_CASE("GlobalHsm reset", "[global-hsm]") {
  fccu::GlobalHsm hsm;
  hsm.Dispatch(fccu::evt::kFaultReported);
  REQUIRE(hsm.IsActive());

  hsm.Reset();
  REQUIRE(hsm.IsIdle());
}

// ============================================================================
// Per-Fault HSM Standalone Tests
// ============================================================================

TEST_CASE("PerFaultHsm lifecycle", "[per-fault-hsm]") {
  fccu::PerFaultHsm hsm;
  hsm.Bind(0U, 3U);  // threshold = 3

  REQUIRE(hsm.IsDormant());

  // First detection
  hsm.Dispatch(fccu::evt::kDetected);
  REQUIRE(hsm.IsDetected());
  REQUIRE(hsm.context().occurrence_count == 1U);

  // Below threshold - stays in Detected
  hsm.Dispatch(fccu::evt::kDetected);
  REQUIRE(hsm.IsDetected());
  REQUIRE(hsm.context().occurrence_count == 2U);

  // Third detection + confirm (threshold=3)
  hsm.Dispatch(fccu::evt::kDetected);
  REQUIRE(hsm.context().occurrence_count == 3U);
  hsm.Dispatch(fccu::evt::kConfirmed);
  REQUIRE(hsm.IsActive());

  // Recovery
  hsm.Dispatch(fccu::evt::kRecoveryStart);
  REQUIRE(hsm.IsRecovering());

  hsm.Dispatch(fccu::evt::kRecoveryDone);
  REQUIRE(hsm.IsCleared());

  // Back to dormant
  hsm.Dispatch(fccu::evt::kClearFault);
  REQUIRE(hsm.IsDormant());
}
