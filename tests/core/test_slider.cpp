/**
 * @file test_slider.cpp
 * @brief Contract tests for the generic Record Tract slider.
 */

#include <fexma/record_tract/slider.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

using namespace fexma::wal;

namespace {
using Payload = std::array<std::byte, 8>;
Payload payload(std::uint64_t value) noexcept { Payload r{}; for (std::size_t i=0;i<r.size();++i) r[i]=static_cast<std::byte>((value>>(i*8u))&0xffu); return r; }

Position read_frontier(const Frontier& frontier) noexcept {
  return frontier.load(std::memory_order_acquire);
}

class RecordingModule final {
public:
  explicit RecordingModule(Position fail_at = static_cast<Position>(-1)) noexcept : fail_at_(fail_at) {}
  bool process(const RecordView& record) noexcept { if (record.position == fail_at_) return false; positions_[count_++] = record.position; return true; }
  void allow_all() noexcept { fail_at_ = static_cast<Position>(-1); }
  std::size_t count() const noexcept { return count_; }
  Position position(std::size_t i) const noexcept { return positions_[i]; }
private:
  Position fail_at_{}; std::array<Position, 8> positions_{}; std::size_t count_{};
};

class FrontierObservingModule final {
public:
  template <class Owner>
  void observe(const Owner& owner) noexcept {
    owner_ = &owner;
    get_frontier_ = [](const void* value) noexcept {
      return static_cast<const Owner*>(value)
          ->GetFrontier()
          .load(std::memory_order_acquire);
    };
  }

  bool process(const RecordView& record) noexcept {
    positions_[count_] = record.position;
    observed_frontiers_[count_] = get_frontier_(owner_);
    ++count_;
    return true;
  }

  std::size_t count() const noexcept { return count_; }
  Position position(std::size_t index) const noexcept {
    return positions_[index];
  }
  Position observed_frontier(std::size_t index) const noexcept {
    return observed_frontiers_[index];
  }

private:
  using GetFrontier = Position (*)(const void*) noexcept;

  const void* owner_{};
  GetFrontier get_frontier_{};
  std::array<Position, 8> positions_{};
  std::array<Position, 8> observed_frontiers_{};
  std::size_t count_{};
};

bool open_tape(RecordTape& tape) { return tape.open({sizeof(Payload), 4, default_alignment}).ok(); }
bool publish(RecordTape& tape, std::uint64_t value) { const Payload bytes=payload(value); return tape.try_publish(std::span<const std::byte>{bytes}).ok(); }

bool processes_available_range_in_order() {
  RecordTape tape;
  RecordingModule module;
  Slider slider(tape, tape.GetFrontier(), module);
  tape.SetTailRef(slider.GetFrontier());
  if (!open_tape(tape)||!publish(tape,1)||!publish(tape,2)||!publish(tape,3)) return false;
  const SliderResult result=slider.process();
  if(result.status!=SliderStatus::Processed||result.processed_count!=3||result.current!=3||read_frontier(slider.GetFrontier())!=3||module.count()!=3) return false;
  for(Position p=0;p<3;++p) if(module.position(static_cast<std::size_t>(p))!=p) return false;
  return slider.process().status==SliderStatus::Empty;
}

bool chained_sliders_use_explicit_frontiers() {
  RecordTape tape;
  RecordingModule first_module;
  RecordingModule second_module;
  Slider first(tape, tape.GetFrontier(), first_module);
  Slider second(tape, first.GetFrontier(), second_module);
  tape.SetTailRef(second.GetFrontier());
  if (!open_tape(tape) || !publish(tape, 1) || !publish(tape, 2) ||
      !publish(tape, 3)) {
    return false;
  }
  const SliderResult first_result = first.process();
  const SliderResult second_result = second.process();
  return first_result.status == SliderStatus::Processed &&
         second_result.status == SliderStatus::Processed &&
         second_result.current == 3 &&
         read_frontier(first.GetFrontier()) == 3 &&
         read_frontier(second.GetFrontier()) == 3 &&
         first_module.count() == 3 && second_module.count() == 3;
}

bool respects_upstream_and_retries_module_failure() {
  RecordTape tape;
  Frontier upstream{2}; RecordingModule module(1); Slider slider(tape,upstream,module);
  tape.SetTailRef(slider.GetFrontier());
  if(!open_tape(tape)||!publish(tape,1)||!publish(tape,2)||!publish(tape,3)) return false;
  const SliderResult failed=slider.process(); if(failed.status!=SliderStatus::ModuleFailed||failed.processed_count!=1||read_frontier(slider.GetFrontier())!=1||module.count()!=1) return false;
  module.allow_all(); const SliderResult retried=slider.process(); if(retried.status!=SliderStatus::Processed||retried.processed_count!=1||retried.current!=2||read_frontier(slider.GetFrontier())!=2) return false;
  upstream.store(3, std::memory_order_release); const SliderResult last=slider.process(); return last.status==SliderStatus::Processed&&last.processed_count==1&&last.current==3&&read_frontier(slider.GetFrontier())==3;
}

bool reports_unavailable_views_without_publication() {
  Frontier terminal{};
  RecordTape tape;
  tape.SetTailRef(terminal);
  Frontier upstream{1}; RecordingModule module; Slider reclaimed(tape,upstream,module);
  if(!open_tape(tape)||!publish(tape,1)) return false;
  terminal.store(1, std::memory_order_release);
  const SliderResult rr=reclaimed.process(); if(rr.status!=SliderStatus::ViewUnavailable||rr.view_status!=ViewStatus::Reclaimed||rr.processed_count!=0||read_frontier(reclaimed.GetFrontier())!=0) return false;
  RecordTape second; Frontier second_terminal{}; second.SetTailRef(second_terminal); if(!open_tape(second)) return false; Frontier ahead{1}; RecordingModule second_module; Slider unpublished(second,ahead,second_module); const SliderResult ur=unpublished.process();
  return ur.status==SliderStatus::ViewUnavailable&&ur.view_status==ViewStatus::Unpublished&&read_frontier(unpublished.GetFrontier())==0&&second_module.count()==0;
}

bool execution_policy_preserves_default_and_supports_batches() {
  RecordTape tape;
  RecordingModule explicit_default;
  Slider default_slider(tape, tape.GetFrontier(), explicit_default, ExecutionPolicy{1, 1});
  tape.SetTailRef(default_slider.GetFrontier());
  if (!open_tape(tape) || !publish(tape, 1) || !publish(tape, 2) ||
      !publish(tape, 3)) return false;
  if (default_slider.process().processed_count != 3 ||
      read_frontier(default_slider.GetFrontier()) != 3) return false;

  RecordTape batched_tape;
  RecordingModule batched(2);
  Slider batched_slider(batched_tape, batched_tape.GetFrontier(), batched, ExecutionPolicy{2, 2});
  batched_tape.SetTailRef(batched_slider.GetFrontier());
  if (!open_tape(batched_tape) || !publish(batched_tape, 1) ||
      !publish(batched_tape, 2) || !publish(batched_tape, 3)) return false;
  const SliderResult first = batched_slider.process();
  return first.status == SliderStatus::ModuleFailed &&
         first.processed_count == 2 && read_frontier(batched_slider.GetFrontier()) == 2;
}

bool execution_policy_read_count_does_not_limit_one_call() {
  RecordTape tape;
  RecordingModule module;
  Slider slider(tape, tape.GetFrontier(), module, ExecutionPolicy{2, 4});
  tape.SetTailRef(slider.GetFrontier());
  if (!tape.open({sizeof(Payload), 8, default_alignment}).ok()) return false;
  for (std::uint64_t value = 0; value < 5; ++value) {
    if (!publish(tape, value)) return false;
  }

  const SliderResult result = slider.process();
  if (result.status != SliderStatus::Processed ||
      result.processed_count != 5 || result.current != 5 ||
      read_frontier(slider.GetFrontier()) != 5 || module.count() != 5) {
    return false;
  }
  for (Position position = 0; position < 5; ++position) {
    if (module.position(static_cast<std::size_t>(position)) != position) {
      return false;
    }
  }
  return true;
}

bool execution_policy_publishes_cadence_and_partial_progress() {
  RecordTape tape;
  FrontierObservingModule module;
  Slider slider(tape, tape.GetFrontier(), module, ExecutionPolicy{3, 2});
  tape.SetTailRef(slider.GetFrontier());
  if (!tape.open({sizeof(Payload), 8, default_alignment}).ok() ||
      !publish(tape, 1) || !publish(tape, 2) ||
      !publish(tape, 3) || !publish(tape, 4) || !publish(tape, 5)) {
    return false;
  }

  module.observe(slider);
  const SliderResult result = slider.process();
  constexpr std::array<Position, 5> expected_frontiers{0, 0, 2, 2, 4};
  if (result.status != SliderStatus::Processed ||
      result.processed_count != 5 || result.current != 5 ||
      read_frontier(slider.GetFrontier()) != 5 || module.count() != 5) {
    return false;
  }
  for (std::size_t index = 0; index < expected_frontiers.size(); ++index) {
    if (module.position(index) != index ||
        module.observed_frontier(index) != expected_frontiers[index]) {
      return false;
    }
  }
  return true;
}

bool execution_policy_flushes_residual_before_failure() {
  RecordTape tape;
  RecordingModule module(3);
  Slider slider(tape, tape.GetFrontier(), module, ExecutionPolicy{8, 4});
  tape.SetTailRef(slider.GetFrontier());
  if (!tape.open({sizeof(Payload), 8, default_alignment}).ok()) return false;
  for (std::uint64_t value = 0; value < 5; ++value) {
    if (!publish(tape, value)) return false;
  }

  const SliderResult failed = slider.process();
  if (failed.status != SliderStatus::ModuleFailed ||
      failed.processed_count != 3 || failed.current != 3 ||
      read_frontier(slider.GetFrontier()) != 3 || module.count() != 3) {
    return false;
  }

  module.allow_all();
  const SliderResult completed = slider.process();
  return completed.status == SliderStatus::Processed &&
         completed.processed_count == 2 && completed.current == 5 &&
         read_frontier(slider.GetFrontier()) == 5 && module.count() == 5;
}

bool policy_observes_canonical_publication(ExecutionPolicy policy) {
  RecordTape tape;
  FrontierObservingModule module;
  Slider slider(tape, tape.GetFrontier(), module, policy);
  tape.SetTailRef(slider.GetFrontier());
  if (!open_tape(tape) || !publish(tape, 1) || !publish(tape, 2) ||
      !publish(tape, 3)) {
    return false;
  }

  module.observe(slider);
  const SliderResult result = slider.process();
  constexpr std::array<Position, 3> expected_frontiers{0, 1, 2};
  if (result.status != SliderStatus::Processed ||
      result.processed_count != 3 || result.current != 3 ||
      read_frontier(slider.GetFrontier()) != 3 || module.count() != 3) {
    return false;
  }
  for (std::size_t index = 0; index < expected_frontiers.size(); ++index) {
    if (module.position(index) != index ||
        module.observed_frontier(index) != expected_frontiers[index]) {
      return false;
    }
  }
  return true;
}

bool execution_policy_invalid_values_normalize_to_canonical_default() {
  return policy_observes_canonical_publication(ExecutionPolicy{0, 4}) &&
         policy_observes_canonical_publication(ExecutionPolicy{4, 0}) &&
         policy_observes_canonical_publication(ExecutionPolicy{0, 0});
}
} // namespace

int main() {
  if (!processes_available_range_in_order()) return 1;
  if (!chained_sliders_use_explicit_frontiers()) return 2;
  if (!respects_upstream_and_retries_module_failure()) return 3;
  if (!reports_unavailable_views_without_publication()) return 4;
  if (!execution_policy_preserves_default_and_supports_batches()) return 5;
  if (!execution_policy_read_count_does_not_limit_one_call()) return 6;
  if (!execution_policy_publishes_cadence_and_partial_progress()) return 7;
  if (!execution_policy_flushes_residual_before_failure()) return 8;
  if (!execution_policy_invalid_values_normalize_to_canonical_default())
    return 9;
  return 0;
}
