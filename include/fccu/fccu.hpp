/**
 * @file fccu.hpp
 * @brief Core FaultCollector template class - software FCCU component.
 *
 * Header-only, zero-heap-allocation, bare-metal friendly fault collection
 * and control unit. Reuses proven patterns from newosp fault_collector.hpp
 * with external component integration (ringbuffer, hsm-cpp).
 *
 * Thread safety: SPSC model - one producer thread, one consumer thread.
 * For multi-producer scenarios, use mccc AsyncBus as the reporting front-end.
 */

#ifndef FCCU_FCCU_HPP_
#define FCCU_FCCU_HPP_

#include "fccu/fault_queue_set.hpp"
#include "fccu/fccu_hsm.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <array>
#include <atomic>
#include <chrono>

namespace fccu {

// ============================================================================
// Platform Utilities
// ============================================================================

namespace detail {

inline uint64_t SteadyNowUs() noexcept {
  auto dur = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(dur).count());
}

inline uint32_t PopCount64(uint64_t x) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<uint32_t>(__builtin_popcountll(x));
#else
  x = x - ((x >> 1U) & 0x5555555555555555ULL);
  x = (x & 0x3333333333333333ULL) + ((x >> 2U) & 0x3333333333333333ULL);
  x = (x + (x >> 4U)) & 0x0F0F0F0F0F0F0F0FULL;
  return static_cast<uint32_t>((x * 0x0101010101010101ULL) >> 56U);
#endif
}

}  // namespace detail

// ============================================================================
// Enumerations
// ============================================================================

enum class FaultPriority : uint8_t { kCritical = 0U, kHigh = 1U, kMedium = 2U, kLow = 3U };

enum class HookAction : uint8_t { kHandled = 0U, kEscalate = 1U, kDefer = 2U, kShutdown = 3U };

enum class FccuError : uint8_t {
  kOk = 0U,
  kQueueFull,
  kInvalidIndex,
  kAlreadyRegistered,
  kNotRegistered,
  kAdmissionDenied,
  kHsmSlotFull
};

enum class BackpressureLevel : uint8_t { kNormal = 0U, kWarning = 1U, kCritical = 2U, kFull = 3U };

// ============================================================================
// Data Structures
// ============================================================================

struct FaultEntry {
  uint16_t fault_index = 0U;
  FaultPriority priority = FaultPriority::kMedium;
  uint8_t reserved = 0U;
  uint32_t detail = 0U;
  uint64_t timestamp_us = 0U;
};

struct FaultEvent {
  uint16_t fault_index = 0U;
  FaultPriority priority = FaultPriority::kMedium;
  uint32_t fault_code = 0U;
  uint32_t detail = 0U;
  uint64_t timestamp_us = 0U;
  uint32_t occurrence_count = 0U;
  bool is_first = false;
};

struct FaultStatistics {
  uint64_t total_reported = 0U;
  uint64_t total_processed = 0U;
  uint64_t total_dropped = 0U;
  uint64_t priority_reported[4] = {};
  uint64_t priority_dropped[4] = {};
};

struct RecentFaultInfo {
  uint16_t fault_index = 0U;
  uint32_t detail = 0U;
  FaultPriority priority = FaultPriority::kMedium;
  uint64_t timestamp_us = 0U;
};

// ============================================================================
// Callback Types (function pointer + context, zero overhead)
// ============================================================================

using FaultHookFn = HookAction (*)(const FaultEvent& event, void* ctx);
using OverflowFn = void (*)(uint16_t fault_index, FaultPriority priority, void* ctx);
using ShutdownFn = void (*)(void* ctx);
using BusNotifyFn = void (*)(const FaultEvent& event, void* ctx);
using FaultReportFn = void (*)(uint16_t fault_index, uint32_t detail, FaultPriority priority, void* ctx);

/** @brief Lightweight fault reporter injection point (POD, 16 bytes). */
struct FaultReporter {
  FaultReportFn fn = nullptr;
  void* ctx = nullptr;

  void Report(uint16_t fault_index, uint32_t detail = 0U,
              FaultPriority priority = FaultPriority::kMedium) const noexcept {
    if (fn != nullptr) {
      fn(fault_index, detail, priority, ctx);
    }
  }
};

// ============================================================================
// FaultCollector - Core Template Class
// ============================================================================

/**
 * @brief Software Fault Collection and Control Unit.
 *
 * @tparam MaxFaults      Maximum fault points (1..256, default: 64)
 * @tparam QueueDepth     SPSC queue capacity per priority level (power of 2, default: 32)
 * @tparam QueueLevels    Number of priority levels (1..8, default: 4)
 * @tparam MaxPerFaultHsm Maximum per-fault HSM instances (default: 8)
 */
template <uint32_t MaxFaults = 64U, uint32_t QueueDepth = 32U, uint32_t QueueLevels = 4U, uint32_t MaxPerFaultHsm = 8U>
class FaultCollector {
  static_assert(MaxFaults >= 1U && MaxFaults <= 256U, "MaxFaults must be 1..256");
  static_assert(QueueLevels >= 1U && QueueLevels <= 8U, "QueueLevels must be 1..8");
  static_assert(MaxPerFaultHsm <= 16U, "MaxPerFaultHsm must be <= 16");

 public:
  static constexpr uint32_t kMaxFaults = MaxFaults;
  static constexpr uint32_t kQueueDepth = QueueDepth;
  static constexpr uint32_t kQueueLevels = QueueLevels;
  static constexpr uint32_t kRecentRingSize = 16U;

  // --- Configuration (call before processing) ---

  FccuError RegisterFault(uint16_t fault_index, uint32_t fault_code, uint32_t attr = 0U,
                          uint32_t err_threshold = 1U) noexcept {
    if (fault_index >= MaxFaults) {
      return FccuError::kInvalidIndex;
    }
    auto& entry = table_[fault_index];
    if (entry.registered) {
      return FccuError::kAlreadyRegistered;
    }
    entry.fault_code = fault_code;
    entry.attr = attr;
    entry.err_threshold = err_threshold;
    entry.registered = true;
    return FccuError::kOk;
  }

  FccuError RegisterHook(uint16_t fault_index, FaultHookFn fn, void* ctx = nullptr) noexcept {
    if (fault_index >= MaxFaults) {
      return FccuError::kInvalidIndex;
    }
    if (!table_[fault_index].registered) {
      return FccuError::kNotRegistered;
    }
    table_[fault_index].hook_fn = fn;
    table_[fault_index].hook_ctx = ctx;
    return FccuError::kOk;
  }

  void SetDefaultHook(FaultHookFn fn, void* ctx = nullptr) noexcept {
    default_hook_fn_ = fn;
    default_hook_ctx_ = ctx;
  }

  void SetOverflowCallback(OverflowFn fn, void* ctx = nullptr) noexcept {
    overflow_fn_ = fn;
    overflow_ctx_ = ctx;
  }

  void SetShutdownCallback(ShutdownFn fn, void* ctx = nullptr) noexcept {
    shutdown_fn_ = fn;
    shutdown_ctx_ = ctx;
  }

  void SetBusNotifier(BusNotifyFn fn, void* ctx = nullptr) noexcept {
    bus_notify_fn_ = fn;
    bus_notify_ctx_ = ctx;
  }

  FccuError BindFaultHsm(uint16_t fault_index, uint32_t threshold = 1U) noexcept {
    if (fault_index >= MaxFaults) {
      return FccuError::kInvalidIndex;
    }
    if (per_fault_hsm_count_ >= MaxPerFaultHsm) {
      return FccuError::kHsmSlotFull;
    }
    uint32_t slot = per_fault_hsm_count_++;
    per_fault_hsm_map_[slot] = fault_index;
    per_fault_hsms_[slot].Bind(fault_index, threshold);
    return FccuError::kOk;
  }

  // --- Reporting (producer side, hot path) ---

  FccuError ReportFault(uint16_t fault_index, uint32_t detail = 0U,
                        FaultPriority priority = FaultPriority::kMedium) noexcept {
    if (fault_index >= MaxFaults) {
      return FccuError::kInvalidIndex;
    }
    if (!table_[fault_index].registered) {
      return FccuError::kNotRegistered;
    }

    uint8_t level = static_cast<uint8_t>(priority);
    if (level >= QueueLevels) {
      level = static_cast<uint8_t>(QueueLevels - 1U);
    }

    FaultEntry entry{};
    entry.fault_index = fault_index;
    entry.priority = priority;
    entry.detail = detail;
    entry.timestamp_us = detail::SteadyNowUs();

    bool pushed = queue_set_.PushWithAdmission(level, entry);
    if (!pushed) {
      stats_total_dropped_.fetch_add(1U, std::memory_order_relaxed);
      if (level < 4U) {
        stats_dropped_[level].fetch_add(1U, std::memory_order_relaxed);
      }
      if (overflow_fn_ != nullptr) {
        overflow_fn_(fault_index, priority, overflow_ctx_);
      }
      return FccuError::kQueueFull;
    }

    // Set active bit
    SetFaultActive(fault_index);

    stats_total_reported_.fetch_add(1U, std::memory_order_relaxed);
    if (level < 4U) {
      stats_reported_[level].fetch_add(1U, std::memory_order_relaxed);
    }

    // Notify per-fault HSM
    DispatchPerFaultEvent(fault_index, evt::kDetected);

    // Update global HSM
    if (global_hsm_.IsIdle()) {
      global_hsm_.Dispatch(evt::kFaultReported);
    }
    if (priority == FaultPriority::kCritical && !global_hsm_.IsDegraded()) {
      global_hsm_.Dispatch(evt::kCriticalDetected);
      global_hsm_.context().critical_count++;
    }

    return FccuError::kOk;
  }

  // --- Processing (consumer side) ---

  uint32_t ProcessFaults() noexcept {
    if (shutdown_requested_) {
      return 0U;
    }

    uint32_t total = 0U;
    FaultEntry entry{};
    uint8_t level = 0U;

    while (queue_set_.Pop(entry, level)) {
      ProcessEntry(entry);
      ++total;
    }
    return total;
  }

  // --- Query Operations ---

  bool IsFaultActive(uint16_t fault_index) const noexcept {
    if (fault_index >= MaxFaults) {
      return false;
    }
    uint32_t word_idx = fault_index / 64U;
    uint32_t bit_idx = fault_index % 64U;
    return (active_bitmap_[word_idx].load(std::memory_order_relaxed) & (1ULL << bit_idx)) != 0U;
  }

  uint32_t ActiveFaultCount() const noexcept {
    uint32_t count = 0U;
    for (uint32_t i = 0U; i < kBitmapWords; ++i) {
      count += detail::PopCount64(active_bitmap_[i].load(std::memory_order_relaxed));
    }
    return count;
  }

  void ClearFault(uint16_t fault_index) noexcept {
    if (fault_index >= MaxFaults) {
      return;
    }
    ClearFaultActive(fault_index);
    table_[fault_index].occurrence_count.store(0U, std::memory_order_relaxed);

    DispatchPerFaultEvent(fault_index, evt::kClearFault);

    if (ActiveFaultCount() == 0U) {
      global_hsm_.Dispatch(evt::kAllCleared);
    }
  }

  void ClearAllFaults() noexcept {
    for (uint32_t i = 0U; i < kBitmapWords; ++i) {
      active_bitmap_[i].store(0U, std::memory_order_relaxed);
    }
    for (uint32_t i = 0U; i < MaxFaults; ++i) {
      table_[i].occurrence_count.store(0U, std::memory_order_relaxed);
    }
    for (uint32_t i = 0U; i < per_fault_hsm_count_; ++i) {
      per_fault_hsms_[i].Reset();
    }
    global_hsm_.Dispatch(evt::kAllCleared);
  }

  FaultStatistics GetStatistics() const noexcept {
    FaultStatistics stats{};
    stats.total_reported = stats_total_reported_.load(std::memory_order_relaxed);
    stats.total_processed = stats_total_processed_.load(std::memory_order_relaxed);
    stats.total_dropped = stats_total_dropped_.load(std::memory_order_relaxed);
    for (uint32_t i = 0U; i < 4U; ++i) {
      stats.priority_reported[i] = stats_reported_[i].load(std::memory_order_relaxed);
      stats.priority_dropped[i] = stats_dropped_[i].load(std::memory_order_relaxed);
    }
    return stats;
  }

  void ResetStatistics() noexcept {
    stats_total_reported_.store(0U, std::memory_order_relaxed);
    stats_total_processed_.store(0U, std::memory_order_relaxed);
    stats_total_dropped_.store(0U, std::memory_order_relaxed);
    for (uint32_t i = 0U; i < 4U; ++i) {
      stats_reported_[i].store(0U, std::memory_order_relaxed);
      stats_dropped_[i].store(0U, std::memory_order_relaxed);
    }
  }

  BackpressureLevel GetBackpressureLevel() const noexcept {
    auto total = queue_set_.TotalSize();
    auto cap = static_cast<decltype(total)>(QueueDepth * QueueLevels);
    if (cap == 0U) {
      return BackpressureLevel::kFull;
    }
    uint32_t pct = static_cast<uint32_t>((total * 100U) / cap);
    if (pct >= 95U) {
      return BackpressureLevel::kFull;
    }
    if (pct >= 80U) {
      return BackpressureLevel::kCritical;
    }
    if (pct >= 60U) {
      return BackpressureLevel::kWarning;
    }
    return BackpressureLevel::kNormal;
  }

  /** @brief Iterate recent faults (newest first). */
  template <typename Fn>
  void ForEachRecent(Fn&& fn, uint32_t max_count = kRecentRingSize) const {
    uint32_t count = (recent_count_ < max_count) ? recent_count_ : max_count;
    for (uint32_t i = 0U; i < count; ++i) {
      uint32_t idx = (recent_head_ + kRecentRingSize - 1U - i) % kRecentRingSize;
      fn(recent_ring_[idx]);
    }
  }

  /** @brief Get a FaultReporter that forwards to this collector's ReportFault. */
  FaultReporter GetReporter() noexcept {
    FaultReporter reporter{};
    reporter.fn = [](uint16_t fi, uint32_t det, FaultPriority pri, void* ctx) {
      static_cast<FaultCollector*>(ctx)->ReportFault(fi, det, pri);
    };
    reporter.ctx = this;
    return reporter;
  }

  // --- HSM access ---
  const GlobalHsm& GetGlobalHsm() const noexcept { return global_hsm_; }
  bool IsShutdownRequested() const noexcept { return shutdown_requested_; }

 private:
  // --- Bitmap operations (newosp pattern) ---

  void SetFaultActive(uint16_t fault_index) noexcept {
    uint32_t word_idx = fault_index / 64U;
    uint32_t bit_idx = fault_index % 64U;
    active_bitmap_[word_idx].fetch_or(1ULL << bit_idx, std::memory_order_relaxed);
  }

  void ClearFaultActive(uint16_t fault_index) noexcept {
    uint32_t word_idx = fault_index / 64U;
    uint32_t bit_idx = fault_index % 64U;
    active_bitmap_[word_idx].fetch_and(~(1ULL << bit_idx), std::memory_order_relaxed);
  }

  // --- Entry processing ---

  void ProcessEntry(const FaultEntry& entry) noexcept {
    uint16_t idx = entry.fault_index;
    if (idx >= MaxFaults) {
      return;
    }

    auto& tbl = table_[idx];
    uint32_t prev_count = tbl.occurrence_count.fetch_add(1U, std::memory_order_relaxed);

    FaultEvent evt_data{};
    evt_data.fault_index = idx;
    evt_data.priority = entry.priority;
    evt_data.fault_code = tbl.fault_code;
    evt_data.detail = entry.detail;
    evt_data.timestamp_us = entry.timestamp_us;
    evt_data.occurrence_count = prev_count + 1U;
    evt_data.is_first = (prev_count == 0U);

    // Record in recent ring
    AddToRecentRing(evt_data);

    // Bus notification
    if (bus_notify_fn_ != nullptr) {
      bus_notify_fn_(evt_data, bus_notify_ctx_);
    }

    // Per-fault HSM: check threshold for confirmation
    if (evt_data.occurrence_count >= tbl.err_threshold) {
      DispatchPerFaultEvent(idx, evt::kConfirmed);
    }

    // Invoke hook
    HookAction action = HookAction::kHandled;
    if (tbl.hook_fn != nullptr) {
      action = tbl.hook_fn(evt_data, tbl.hook_ctx);
    } else if (default_hook_fn_ != nullptr) {
      action = default_hook_fn_(evt_data, default_hook_ctx_);
    }

    // Handle action
    switch (action) {
      case HookAction::kHandled:
        ClearFaultActive(idx);
        DispatchPerFaultEvent(idx, evt::kClearFault);
        if (ActiveFaultCount() == 0U) {
          global_hsm_.Dispatch(evt::kAllCleared);
        }
        break;
      case HookAction::kEscalate:
        HandleEscalation(entry);
        break;
      case HookAction::kDefer:
        break;
      case HookAction::kShutdown:
        shutdown_requested_ = true;
        global_hsm_.Dispatch(evt::kShutdownReq);
        if (shutdown_fn_ != nullptr) {
          shutdown_fn_(shutdown_ctx_);
        }
        break;
    }

    stats_total_processed_.fetch_add(1U, std::memory_order_relaxed);
  }

  void HandleEscalation(const FaultEntry& original) noexcept {
    uint8_t pri = static_cast<uint8_t>(original.priority);
    if (pri == 0U) {
      return;  // Already critical
    }

    FaultEntry escalated = original;
    escalated.priority = static_cast<FaultPriority>(pri - 1U);
    escalated.timestamp_us = detail::SteadyNowUs();

    if (!queue_set_.Push(static_cast<uint8_t>(pri - 1U), escalated)) {
      stats_total_dropped_.fetch_add(1U, std::memory_order_relaxed);
    }
  }

  void AddToRecentRing(const FaultEvent& evt_data) noexcept {
    auto& slot = recent_ring_[recent_head_];
    slot.fault_index = evt_data.fault_index;
    slot.detail = evt_data.detail;
    slot.priority = evt_data.priority;
    slot.timestamp_us = evt_data.timestamp_us;
    recent_head_ = (recent_head_ + 1U) % kRecentRingSize;
    if (recent_count_ < kRecentRingSize) {
      ++recent_count_;
    }
  }

  void DispatchPerFaultEvent(uint16_t fault_index, uint32_t event_id) noexcept {
    for (uint32_t i = 0U; i < per_fault_hsm_count_; ++i) {
      if (per_fault_hsm_map_[i] == fault_index) {
        per_fault_hsms_[i].Dispatch(event_id);
        return;
      }
    }
  }

  // --- Fault table ---
  struct FaultTableEntry {
    uint32_t fault_code = 0U;
    uint32_t attr = 0U;
    uint32_t err_threshold = 1U;
    bool registered = false;
    std::atomic<uint32_t> occurrence_count{0U};
    FaultHookFn hook_fn = nullptr;
    void* hook_ctx = nullptr;
  };

  // --- Members ---
  FaultQueueSet<FaultEntry, QueueLevels, QueueDepth> queue_set_;
  std::array<FaultTableEntry, MaxFaults> table_{};

  static constexpr uint32_t kBitmapWords = (MaxFaults + 63U) / 64U;
  std::array<std::atomic<uint64_t>, kBitmapWords> active_bitmap_{};

  std::atomic<uint64_t> stats_total_reported_{0U};
  std::atomic<uint64_t> stats_total_processed_{0U};
  std::atomic<uint64_t> stats_total_dropped_{0U};
  std::array<std::atomic<uint64_t>, 4U> stats_reported_{};
  std::array<std::atomic<uint64_t>, 4U> stats_dropped_{};

  FaultHookFn default_hook_fn_ = nullptr;
  void* default_hook_ctx_ = nullptr;
  OverflowFn overflow_fn_ = nullptr;
  void* overflow_ctx_ = nullptr;
  ShutdownFn shutdown_fn_ = nullptr;
  void* shutdown_ctx_ = nullptr;
  BusNotifyFn bus_notify_fn_ = nullptr;
  void* bus_notify_ctx_ = nullptr;

  GlobalHsm global_hsm_;
  std::array<PerFaultHsm, MaxPerFaultHsm> per_fault_hsms_;
  std::array<uint16_t, MaxPerFaultHsm> per_fault_hsm_map_{};
  uint32_t per_fault_hsm_count_ = 0U;

  std::array<RecentFaultInfo, kRecentRingSize> recent_ring_{};
  uint32_t recent_head_ = 0U;
  uint32_t recent_count_ = 0U;

  bool shutdown_requested_ = false;
};

}  // namespace fccu

#endif  // FCCU_FCCU_HPP_
