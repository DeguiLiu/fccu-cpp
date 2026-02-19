/**
 * @file bus_demo.cpp
 * @brief FCCU + mccc AsyncBus integration demo.
 *
 * Demonstrates fault notification via mccc message bus.
 */

#include "fccu/fccu.hpp"
#include "mccc/message_bus.hpp"

#include <cstdint>
#include <cstdio>

#include <variant>

// Message type for fault notifications over mccc bus
struct FaultNotification {
  uint16_t fault_index;
  uint32_t fault_code;
  uint32_t detail;
  uint8_t priority;
  uint64_t timestamp_us;
};

// mccc payload variant
using BusPayload = std::variant<FaultNotification>;
using Bus = mccc::AsyncBus<BusPayload>;

// Bus notifier callback: publish fault events to mccc bus
static Bus* g_bus = nullptr;

static void BusNotifier(const fccu::FaultEvent& event, void* /*ctx*/) {
  if (g_bus == nullptr) {
    return;
  }
  FaultNotification msg{};
  msg.fault_index = event.fault_index;
  msg.fault_code = event.fault_code;
  msg.detail = event.detail;
  msg.priority = static_cast<uint8_t>(event.priority);
  msg.timestamp_us = event.timestamp_us;
  g_bus->Publish(BusPayload{msg}, 0U);
}

static fccu::HookAction SimpleHook(const fccu::FaultEvent& /*event*/, void* /*ctx*/) {
  return fccu::HookAction::kHandled;
}

int main() {
  std::printf("=== FCCU + mccc Bus Demo ===\n\n");

  // Get mccc bus singleton
  Bus& bus = Bus::Instance();
  g_bus = &bus;

  // Subscribe to fault notifications (callback receives envelope)
  bus.Subscribe<FaultNotification>([](const Bus::EnvelopeType& env) {
    if (auto* msg = std::get_if<FaultNotification>(&env.payload)) {
      std::printf("  [Bus] Received: fault_index=%u code=0x%04x detail=0x%x pri=%u\n", msg->fault_index,
                  msg->fault_code, msg->detail, msg->priority);
    }
  });

  // Create FCCU
  fccu::FaultCollector<8, 16> collector;
  collector.RegisterFault(0U, 0xA001U);
  collector.RegisterFault(1U, 0xA002U);
  collector.RegisterHook(0U, SimpleHook);
  collector.RegisterHook(1U, SimpleHook);
  collector.SetBusNotifier(BusNotifier);

  std::printf("--- Reporting faults ---\n");
  collector.ReportFault(0U, 0x11, fccu::FaultPriority::kHigh);
  collector.ReportFault(1U, 0x22, fccu::FaultPriority::kMedium);

  std::printf("\n--- Processing faults (triggers bus notifications) ---\n");
  collector.ProcessFaults();

  std::printf("\n--- Processing bus messages ---\n");
  bus.ProcessBatch();

  std::printf("\n=== Demo Complete ===\n");
  return 0;
}
