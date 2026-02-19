/**
 * @file fault_queue_set.hpp
 * @brief Multi-level priority SPSC queue set for fault entry buffering.
 *
 * Wraps spsc::Ringbuffer instances into a priority-aware queue set.
 * Higher priority queues (lower index) are drained first.
 *
 * Design patterns from newosp fault_collector.hpp:
 * - Priority admission control (60%/80%/99% thresholds)
 * - Per-priority queue depth monitoring
 */

#ifndef FCCU_FAULT_QUEUE_SET_HPP_
#define FCCU_FAULT_QUEUE_SET_HPP_

#include "spsc/ringbuffer.hpp"

#include <cstdint>

#include <array>

namespace fccu {

// ============================================================================
// FaultQueueSet - Multi-level SPSC Priority Queue Set
// ============================================================================

/**
 * @brief Multi-level priority queue set using SPSC ringbuffers.
 *
 * @tparam T         Element type (must be trivially copyable)
 * @tparam Levels    Number of priority levels (default: 4)
 * @tparam LevelSize Capacity per level (must be power of 2, default: 32)
 *
 * Priority convention: level 0 = highest priority (Critical),
 * level Levels-1 = lowest priority (Low).
 *
 * Thread safety: exactly one producer thread, one consumer thread (SPSC).
 */
template <typename T, uint32_t Levels = 4U, uint32_t LevelSize = 32U>
class FaultQueueSet {
  static_assert(Levels > 0U && Levels <= 8U, "Levels must be 1..8");
  static_assert(LevelSize > 0U && (LevelSize & (LevelSize - 1U)) == 0U, "LevelSize must be power of 2");

 public:
  using IndexT = std::size_t;

  // --- Priority Admission Thresholds (from newosp pattern) ---
  static constexpr uint32_t kLowThreshold = (LevelSize * 60U) / 100U;     ///< 60% full
  static constexpr uint32_t kMediumThreshold = (LevelSize * 80U) / 100U;  ///< 80% full
  static constexpr uint32_t kHighThreshold = (LevelSize * 99U) / 100U;    ///< 99% full

  /**
   * @brief Push an item into the specified priority level queue.
   * @param level Priority level (0 = highest)
   * @param item  Item to enqueue
   * @return true if successfully enqueued, false if queue full or invalid level
   */
  bool Push(uint8_t level, const T& item) noexcept {
    if (level >= Levels) {
      return false;
    }
    return queues_[level].Push(item);
  }

  /**
   * @brief Push with priority admission control (newosp pattern).
   *
   * Admission thresholds based on the target queue's fill level:
   * - Critical (level 0): always admit if physically possible
   * - High (level 1): admit if queue < 99% full
   * - Medium (level 2): admit if queue < 80% full
   * - Low (level 3+): admit if queue < 60% full
   *
   * @param level Priority level (0 = highest)
   * @param item  Item to enqueue
   * @return true if admitted and enqueued
   */
  bool PushWithAdmission(uint8_t level, const T& item) noexcept {
    if (level >= Levels) {
      return false;
    }
    if (!AdmitByPriority(level, static_cast<uint32_t>(queues_[level].Size()))) {
      return false;
    }
    return queues_[level].Push(item);
  }

  /**
   * @brief Pop the highest-priority available item.
   *
   * Scans from level 0 (highest) to level Levels-1 (lowest), returning
   * the first available item.
   *
   * @param[out] item      Popped item
   * @param[out] out_level Priority level of the popped item
   * @return true if an item was dequeued
   */
  bool Pop(T& item, uint8_t& out_level) noexcept {
    for (uint8_t i = 0U; i < static_cast<uint8_t>(Levels); ++i) {
      if (queues_[i].Pop(item)) {
        out_level = i;
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Check if all queues are empty.
   */
  bool IsEmpty() const noexcept {
    for (uint32_t i = 0U; i < Levels; ++i) {
      if (!queues_[i].IsEmpty()) {
        return false;
      }
    }
    return true;
  }

  /**
   * @brief Get the current size of a specific priority level queue.
   */
  IndexT Size(uint8_t level) const noexcept {
    if (level >= Levels) {
      return 0;
    }
    return queues_[level].Size();
  }

  /**
   * @brief Get total items across all priority levels.
   */
  IndexT TotalSize() const noexcept {
    IndexT total = 0;
    for (uint32_t i = 0U; i < Levels; ++i) {
      total += queues_[i].Size();
    }
    return total;
  }

  /**
   * @brief Get available slots in a specific priority level queue.
   */
  IndexT Available(uint8_t level) const noexcept {
    if (level >= Levels) {
      return 0;
    }
    return queues_[level].Available();
  }

  /**
   * @brief Capacity per level (compile-time constant).
   */
  static constexpr uint32_t Capacity() noexcept { return LevelSize; }

  /**
   * @brief Number of priority levels (compile-time constant).
   */
  static constexpr uint32_t LevelCount() noexcept { return Levels; }

 private:
  /**
   * @brief Priority-based admission control (newosp pattern).
   *
   * @param level         Priority level
   * @param current_depth Current queue depth for this level
   * @return true if the item should be admitted
   */
  static bool AdmitByPriority(uint8_t level, uint32_t current_depth) noexcept {
    // Critical: always admit if physically possible
    if (level == 0U) {
      return true;
    }
    if (level == 1U) {
      return current_depth < kHighThreshold;  // < 99%
    }
    if (level == 2U) {
      return current_depth < kMediumThreshold;  // < 80%
    }
    // Low priority (level 3+)
    return current_depth < kLowThreshold;  // < 60%
  }

  std::array<spsc::Ringbuffer<T, LevelSize>, Levels> queues_;
};

}  // namespace fccu

#endif  // FCCU_FAULT_QUEUE_SET_HPP_
