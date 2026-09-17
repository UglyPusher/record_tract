#pragma once

/**
 * @file slider.hpp
 * @brief Generic synchronous mechanics for a stage over RecordTape records.
 */

#include <fexma/wal/record_tape.hpp>
#include <fexma/wal/record_tape_types.hpp>

#include <atomic>
#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>

namespace fexma::wal {

struct ExecutionPolicy final {
  std::size_t read_count{1};
  std::size_t publish_count{1};
};

enum class SliderStatus : std::uint8_t {
  Processed,
  Empty,
  UpstreamRegression,
  ViewUnavailable,
  ModuleFailed
};

struct SliderResult {
  SliderStatus status{SliderStatus::Empty};
  Position current{};
  std::uint64_t processed_count{};
  ViewStatus view_status{ViewStatus::Ok};

  [[nodiscard]] bool ok() const noexcept {
    return status == SliderStatus::Processed || status == SliderStatus::Empty;
  }
};

template <class Predecessor, class Module>
class Slider final {
public:
  Slider(const RecordTape& source, const Predecessor& predecessor,
         Module& module) noexcept
      : Slider(source, predecessor, module, ExecutionPolicy{}) {}

  Slider(const RecordTape& source, const Predecessor& predecessor,
         Module& module, ExecutionPolicy policy) noexcept
      : source_(source), predecessor_(predecessor), module_(module),
        policy_(normalize(policy)) {}

  Slider(const RecordTape& source, Module& module) noexcept
      requires std::same_as<Predecessor, RecordTape>
      : Slider(source, source, module, ExecutionPolicy{}) {}

  Slider(const RecordTape& source, Module& module, ExecutionPolicy policy) noexcept
      requires std::same_as<Predecessor, RecordTape>
      : Slider(source, source, module, policy) {}

  [[nodiscard]] SliderResult process_available() noexcept {
    Position current = GetFrontier();

    const Position available_end = predecessor_.GetFrontier();
    if (available_end < current) {
      return {SliderStatus::UpstreamRegression, current, 0};
    }
    if (available_end == current) {
      return {SliderStatus::Empty, current, 0};
    }

    std::uint64_t processed_count = 0;
    std::size_t since_publish = 0;
    while (current < available_end) {
      const Position pass_end =
          current + std::min<Position>(policy_.read_count, available_end - current);
      while (current < pass_end) {
        const AccessResult access = source_.try_view(current);
        if (!access.ok()) {
          flush(current, since_publish);
          return {SliderStatus::ViewUnavailable, current, processed_count,
                  access.status};
        }
        if (!module_.process(access.record)) {
          flush(current, since_publish);
          return {SliderStatus::ModuleFailed, current, processed_count};
        }

        ++current;
        ++processed_count;
        ++since_publish;
        if (since_publish == policy_.publish_count) {
          publish(current);
          since_publish = 0;
        }
      }
    }
    if (since_publish != 0) publish(current);
    return {SliderStatus::Processed, current, processed_count};
  }

  [[nodiscard]] Position GetFrontier() const noexcept {
    return frontier_.load(std::memory_order_acquire);
  }

  [[nodiscard]] Position current() const noexcept { return GetFrontier(); }
  // Cold-path initialization: process_available() must not be active.
  void reset_quiescent(Position initial) noexcept {
    frontier_.store(initial, std::memory_order_relaxed);
  }

private:
  [[nodiscard]] static constexpr ExecutionPolicy
  normalize(ExecutionPolicy policy) noexcept {
    return policy.read_count == 0 || policy.publish_count == 0
               ? ExecutionPolicy{}
               : policy;
  }

  void flush(Position current, std::size_t since_publish) noexcept {
    if (since_publish != 0) publish(current);
  }

  void publish(Position end) noexcept {
    frontier_.store(end, std::memory_order_release);
  }

  static_assert(std::atomic<Position>::is_always_lock_free);

  const RecordTape& source_;
  const Predecessor& predecessor_;
  Module& module_;
  const ExecutionPolicy policy_;
  alignas(64) std::atomic<Position> frontier_{};
};

template <class Module>
Slider(const RecordTape&, Module&) -> Slider<RecordTape, Module>;

template <class Module>
Slider(const RecordTape&, Module&, ExecutionPolicy)
    -> Slider<RecordTape, Module>;

} // namespace fexma::wal
