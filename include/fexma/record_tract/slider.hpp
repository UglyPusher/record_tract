#pragma once

/**
 * @file slider.hpp
 * @brief Generic synchronous mechanics for a stage over RecordTape records.
 */

#include <fexma/record_tract/record_tape.hpp>
#include <fexma/record_tract/record_tape_types.hpp>

#include <atomic>
#include <algorithm>
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

template <class Module>
class Slider final {
public:
  Slider(const RecordTape& source, const Frontier& upstream_frontier,
         Module& module) noexcept
      : Slider(source, upstream_frontier, module, ExecutionPolicy{}) {}

  Slider(const RecordTape& source, const Frontier& upstream_frontier,
         Module& module, ExecutionPolicy policy) noexcept
      : source_(source), upstream_frontier_(&upstream_frontier), module_(module),
        policy_(normalize(policy)) {}

  [[nodiscard]] SliderResult process() noexcept {
    Position current = frontier_.load(std::memory_order_acquire);

    const Position available_end =
        upstream_frontier_->load(std::memory_order_acquire);
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

  [[nodiscard]] const Frontier& GetFrontier() const noexcept { return frontier_; }

  [[nodiscard]] Position current() const noexcept {
    return frontier_.load(std::memory_order_acquire);
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

  static_assert(Frontier::is_always_lock_free);

  const RecordTape& source_;
  const Frontier* upstream_frontier_;
  Module& module_;
  const ExecutionPolicy policy_;
  alignas(64) Frontier frontier_{};
};

} // namespace fexma::wal
