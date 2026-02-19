/**
 * @file basic_demo.cpp
 * @brief Basic FCCU demo - standalone fault reporting and processing.
 *
 * Demonstrates: register faults, set hooks, report faults at different
 * priorities, manually call ProcessFaults(), query and clear active faults.
 */

#include "fccu/fccu.hpp"

#include <cstdio>

// User-defined hook: handle faults based on priority
static fccu::HookAction MyFaultHook(const fccu::FaultEvent& event, void* /*ctx*/) {
  std::printf("  [Hook] fault_index=%u code=0x%04x detail=0x%x pri=%u count=%u%s\n", event.fault_index,
              event.fault_code, event.detail, static_cast<unsigned>(event.priority), event.occurrence_count,
              event.is_first ? " (FIRST)" : "");

  if (event.priority == fccu::FaultPriority::kCritical) {
    std::printf("  [Hook] Critical fault -> DEFER for manual review\n");
    return fccu::HookAction::kDefer;
  }
  return fccu::HookAction::kHandled;
}

// Default hook for unregistered fault hooks
static fccu::HookAction DefaultHook(const fccu::FaultEvent& event, void* /*ctx*/) {
  std::printf("  [Default] fault_index=%u code=0x%04x -> HANDLED\n", event.fault_index, event.fault_code);
  return fccu::HookAction::kHandled;
}

// Overflow callback
static void OnOverflow(uint16_t fault_index, fccu::FaultPriority priority, void* /*ctx*/) {
  std::printf("  [Overflow] fault_index=%u pri=%u DROPPED!\n", fault_index, static_cast<unsigned>(priority));
}

int main() {
  std::printf("=== FCCU Basic Demo ===\n\n");

  // Create collector: 16 max faults, 8-deep queues, 4 priority levels
  fccu::FaultCollector<16, 8, 4, 4> collector;

  // Register fault points
  collector.RegisterFault(0U, 0x1001U, 0U, 1U);  // Temperature sensor
  collector.RegisterFault(1U, 0x1002U, 0U, 1U);  // Voltage monitor
  collector.RegisterFault(2U, 0x2001U, 0U, 3U);  // Communication timeout (threshold=3)

  // Register hooks
  collector.RegisterHook(0U, MyFaultHook);
  collector.RegisterHook(1U, MyFaultHook);
  collector.RegisterHook(2U, MyFaultHook);
  collector.SetDefaultHook(DefaultHook);
  collector.SetOverflowCallback(OnOverflow);

  // Bind per-fault HSM for critical fault
  collector.BindFaultHsm(0U, 1U);

  std::printf("--- Reporting faults ---\n");

  // Report faults at different priorities
  collector.ReportFault(0U, 0xDEAD, fccu::FaultPriority::kCritical);
  collector.ReportFault(1U, 0xBEEF, fccu::FaultPriority::kHigh);
  collector.ReportFault(2U, 0x0001, fccu::FaultPriority::kLow);

  std::printf("\nActive faults before processing: %u\n", collector.ActiveFaultCount());
  std::printf("Global HSM state: %s\n", collector.GetGlobalHsm().CurrentStateName());
  std::printf("Backpressure: %u\n\n", static_cast<unsigned>(collector.GetBackpressureLevel()));

  // Process all queued faults
  std::printf("--- Processing faults ---\n");
  uint32_t processed = collector.ProcessFaults();
  std::printf("\nProcessed %u faults\n", processed);

  // Query state after processing
  std::printf("\nActive faults after processing: %u\n", collector.ActiveFaultCount());
  std::printf("Fault 0 active: %s\n", collector.IsFaultActive(0U) ? "YES (deferred)" : "NO");
  std::printf("Fault 1 active: %s\n", collector.IsFaultActive(1U) ? "YES" : "NO (handled)");
  std::printf("Global HSM state: %s\n\n", collector.GetGlobalHsm().CurrentStateName());

  // Statistics
  auto stats = collector.GetStatistics();
  std::printf("--- Statistics ---\n");
  std::printf("Reported: %lu  Processed: %lu  Dropped: %lu\n", stats.total_reported, stats.total_processed,
              stats.total_dropped);

  // Recent faults
  std::printf("\n--- Recent Faults (newest first) ---\n");
  collector.ForEachRecent([](const fccu::RecentFaultInfo& info) {
    std::printf("  idx=%u detail=0x%x pri=%u ts=%lu us\n", info.fault_index, info.detail,
                static_cast<unsigned>(info.priority), info.timestamp_us);
  });

  // Clear deferred fault
  std::printf("\n--- Clearing fault 0 ---\n");
  collector.ClearFault(0U);
  std::printf("Active faults: %u\n", collector.ActiveFaultCount());
  std::printf("Global HSM state: %s\n", collector.GetGlobalHsm().CurrentStateName());

  // FaultReporter injection point demo
  std::printf("\n--- FaultReporter injection ---\n");
  auto reporter = collector.GetReporter();
  reporter.Report(1U, 0xCAFE, fccu::FaultPriority::kMedium);
  collector.ProcessFaults();
  std::printf("Active faults: %u\n", collector.ActiveFaultCount());

  std::printf("\n=== Demo Complete ===\n");
  return 0;
}
