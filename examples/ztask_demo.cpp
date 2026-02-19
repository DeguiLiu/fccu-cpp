/**
 * @file ztask_demo.cpp
 * @brief FCCU + ztask periodic scheduling demo.
 *
 * Demonstrates ProcessFaults() driven by ztask cooperative scheduler.
 */

#include "fccu/fccu.hpp"
#include "ztask/ztask.hpp"

#include <cstdio>

static fccu::FaultCollector<8, 16>* g_collector = nullptr;
static uint32_t g_tick_count = 0U;

// ztask callback: periodic fault processing
static void FaultProcessTask(void* /*ctx*/) {
  if (g_collector == nullptr) {
    return;
  }
  uint32_t n = g_collector->ProcessFaults();
  if (n > 0U) {
    std::printf("  [ztask tick=%u] Processed %u faults\n", g_tick_count, n);
  }
}

// ztask callback: simulate periodic fault injection
static void FaultInjector(void* /*ctx*/) {
  if (g_collector == nullptr) {
    return;
  }
  static uint16_t inject_idx = 0U;
  auto pri = (inject_idx % 2U == 0U) ? fccu::FaultPriority::kMedium : fccu::FaultPriority::kHigh;
  g_collector->ReportFault(inject_idx % 4U, inject_idx * 0x10U, pri);
  std::printf("  [injector tick=%u] Reported fault idx=%u\n", g_tick_count, inject_idx % 4U);
  ++inject_idx;
}

static fccu::HookAction DemoHook(const fccu::FaultEvent& event, void* /*ctx*/) {
  std::printf("    [hook] fault_index=%u code=0x%04x detail=0x%x -> HANDLED\n", event.fault_index, event.fault_code,
              event.detail);
  return fccu::HookAction::kHandled;
}

int main() {
  std::printf("=== FCCU + ztask Demo ===\n\n");

  // Create collector
  fccu::FaultCollector<8, 16> collector;
  g_collector = &collector;

  // Register faults
  for (uint16_t i = 0U; i < 4U; ++i) {
    collector.RegisterFault(i, 0x3000U + i);
    collector.RegisterHook(i, DemoHook);
  }

  // Create ztask scheduler
  ztask::TaskScheduler<4> scheduler;

  // Bind tasks: process faults every 5 ticks, inject every 10 ticks
  scheduler.Bind(FaultProcessTask, 5U, 5U);
  scheduler.Bind(FaultInjector, 10U, 10U);

  std::printf("--- Running %u ticks ---\n", 50U);
  for (uint32_t t = 0U; t < 50U; ++t) {
    g_tick_count = t;
    scheduler.Tick();
    scheduler.Poll();
  }

  // Final statistics
  auto stats = collector.GetStatistics();
  std::printf("\n--- Final Statistics ---\n");
  std::printf("Reported: %lu  Processed: %lu  Dropped: %lu\n", stats.total_reported, stats.total_processed,
              stats.total_dropped);
  std::printf("Active faults: %u\n", collector.ActiveFaultCount());
  std::printf("Global HSM: %s\n", collector.GetGlobalHsm().CurrentStateName());

  std::printf("\n=== Demo Complete ===\n");
  g_collector = nullptr;
  return 0;
}
