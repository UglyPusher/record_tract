#pragma once

/**
 * @file persistence_slider.hpp
 * @brief Static persistence stage over a RecordTape.
 */

#include <fexma/wal/persistence.hpp>
#include <fexma/wal/slider.hpp>
#include <fexma/wal/record_tape_types.hpp>

#include <atomic>

namespace fexma::wal {

class PersistenceSlider final {
public:
  PersistenceSlider(const RecordTape& source, PersistenceModule& persistence,
                    Position maximum_count = 0) noexcept
      : source_(source), persistence_(persistence), maximum_count_(maximum_count) {}

  [[nodiscard]] SliderResult process_available() noexcept {
    Position current = GetFrontier();
    const Position available_end = source_.head();
    if (available_end < current) {
      return {SliderStatus::UpstreamRegression, current, 0};
    }
    if (available_end == current || maximum_count_ == 0) {
      return {SliderStatus::Empty, current, 0};
    }

    const Position available_count = available_end - current;
    const Position count = available_count < maximum_count_
                               ? available_count
                               : maximum_count_;
    const Position batch_end = current + count;
    std::uint64_t processed_count = 0;
    while (current < batch_end) {
      const AccessResult access = source_.try_view(current);
      if (!access.ok()) {
        return {SliderStatus::ViewUnavailable, current, processed_count,
                access.status};
      }
      if (!persistence_.process(access.record)) {
        return {SliderStatus::ModuleFailed, current, processed_count};
      }
      ++current;
      ++processed_count;
    }

    if (!persistence_.sync() || !publish(current)) {
      return {SliderStatus::ModuleFailed, current, processed_count};
    }
    return {SliderStatus::Processed, current, processed_count};
  }

  [[nodiscard]] Position GetFrontier() const noexcept {
    return frontier_.load(std::memory_order_acquire);
  }

  [[nodiscard]] Position current() const noexcept { return GetFrontier(); }

  void reset_quiescent(Position initial) noexcept {
    frontier_.store(initial, std::memory_order_relaxed);
  }

  void set_maximum_count(Position maximum_count) noexcept {
    maximum_count_ = maximum_count;
  }

private:
  [[nodiscard]] bool publish(Position end) noexcept {
    const Position current = frontier_.load(std::memory_order_relaxed);
    if (end < current) return false;
    frontier_.store(end, std::memory_order_release);
    return true;
  }

  const RecordTape& source_;
  PersistenceModule& persistence_;
  Position maximum_count_{};
  alignas(64) std::atomic<Position> frontier_{};
};

} // namespace fexma::wal
