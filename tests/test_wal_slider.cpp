#include <fexma/wal/slider.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

using namespace fexma::wal;

namespace {
using Payload = std::array<std::byte, 8>;
Payload payload(std::uint64_t value) noexcept { Payload r{}; for (std::size_t i=0;i<r.size();++i) r[i]=static_cast<std::byte>((value>>(i*8u))&0xffu); return r; }

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

class Upstream final {
public:
  explicit Upstream(Position value = 0) noexcept : value_(value) {}
  Position GetFrontier() const noexcept { return value_; }
  void publish(Position value) noexcept { value_ = value; }
private: Position value_{};
};

bool open_tape(RecordTape& tape) { return tape.open({sizeof(Payload), 4, default_alignment}).ok(); }
bool publish(RecordTape& tape, std::uint64_t value) { const Payload bytes=payload(value); return tape.try_publish(std::span<const std::byte>{bytes}).ok(); }

bool processes_available_range_in_order() {
  RecordTape tape; if (!open_tape(tape)||!publish(tape,1)||!publish(tape,2)||!publish(tape,3)) return false;
  RecordingModule module; Slider slider(tape,module); const SliderResult result=slider.process_available();
  if(result.status!=SliderStatus::Processed||result.processed_count!=3||result.current!=3||slider.GetFrontier()!=3||module.count()!=3) return false;
  for(Position p=0;p<3;++p) if(module.position(static_cast<std::size_t>(p))!=p) return false;
  return slider.process_available().status==SliderStatus::Empty;
}

bool respects_upstream_and_retries_module_failure() {
  RecordTape tape; if(!open_tape(tape)||!publish(tape,1)||!publish(tape,2)||!publish(tape,3)) return false;
  Upstream upstream(2); RecordingModule module(1); Slider slider(tape,upstream,module);
  const SliderResult failed=slider.process_available(); if(failed.status!=SliderStatus::ModuleFailed||failed.processed_count!=1||slider.GetFrontier()!=1||module.count()!=1) return false;
  module.allow_all(); const SliderResult retried=slider.process_available(); if(retried.status!=SliderStatus::Processed||retried.processed_count!=1||retried.current!=2||slider.GetFrontier()!=2) return false;
  upstream.publish(3); const SliderResult last=slider.process_available(); return last.status==SliderStatus::Processed&&last.processed_count==1&&last.current==3&&slider.GetFrontier()==3;
}

bool rejects_regressed_upstream() {
  RecordTape tape; if(!open_tape(tape)||!publish(tape,1)) return false;
  Upstream upstream; RecordingModule module; Slider slider(tape,upstream,module); slider.reset_quiescent(1);
  return slider.process_available().status==SliderStatus::UpstreamRegression&&module.count()==0&&slider.GetFrontier()==1;
}

bool reports_unavailable_views_without_publication() {
  RecordTape tape; if(!open_tape(tape)||!publish(tape,1)) return false;
  Upstream upstream(1); RecordingModule module; Slider reclaimed(tape,upstream,module); if(tape.reclaim(1)!=ReclaimStatus::Ok) return false;
  const SliderResult rr=reclaimed.process_available(); if(rr.status!=SliderStatus::ViewUnavailable||rr.view_status!=ViewStatus::Reclaimed||rr.processed_count!=0||reclaimed.GetFrontier()!=0) return false;
  RecordTape second; if(!open_tape(second)) return false; Upstream ahead(1); RecordingModule second_module; Slider unpublished(second,ahead,second_module); const SliderResult ur=unpublished.process_available();
  return ur.status==SliderStatus::ViewUnavailable&&ur.view_status==ViewStatus::Unpublished&&unpublished.GetFrontier()==0&&second_module.count()==0;
}

bool execution_policy_preserves_default_and_supports_batches() {
  RecordTape tape;
  if (!open_tape(tape) || !publish(tape, 1) || !publish(tape, 2) ||
      !publish(tape, 3)) return false;
  RecordingModule explicit_default;
  Slider default_slider(tape, explicit_default, ExecutionPolicy{1, 1});
  if (default_slider.process_available().processed_count != 3 ||
      default_slider.GetFrontier() != 3) return false;

  RecordTape batched_tape;
  if (!open_tape(batched_tape) || !publish(batched_tape, 1) ||
      !publish(batched_tape, 2) || !publish(batched_tape, 3)) return false;
  RecordingModule batched(2);
  Slider batched_slider(batched_tape, batched, ExecutionPolicy{2, 2});
  const SliderResult first = batched_slider.process_available();
  return first.status == SliderStatus::ModuleFailed &&
         first.processed_count == 2 && batched_slider.GetFrontier() == 2;
}

bool execution_policy_publishes_cadence_and_partial_progress() {
  RecordTape tape;
  if (!tape.open({sizeof(Payload), 8, default_alignment}).ok() ||
      !publish(tape, 1) || !publish(tape, 2) ||
      !publish(tape, 3) || !publish(tape, 4) || !publish(tape, 5) ||
      !publish(tape, 6)) return false;
  RecordingModule module(4);
  Slider slider(tape, module, ExecutionPolicy{8, 4});
  const SliderResult failed = slider.process_available();
  if (failed.status != SliderStatus::ModuleFailed || failed.processed_count != 4 ||
      slider.GetFrontier() != 4) return false;
  module.allow_all();
  const SliderResult completed = slider.process_available();
  return completed.status == SliderStatus::Processed &&
         completed.processed_count == 2 && slider.GetFrontier() == 6;
}

bool execution_policy_zero_counts_fall_back_to_default() {
  RecordTape tape;
  if (!open_tape(tape) || !publish(tape, 1)) return false;
  RecordingModule module;
  Slider zero_read(tape, module, ExecutionPolicy{0, 1});
  Slider zero_publish(tape, module, ExecutionPolicy{1, 0});
  const SliderResult read_result = zero_read.process_available();
  const SliderResult publish_result = zero_publish.process_available();
  return read_result.status == SliderStatus::Processed &&
         read_result.processed_count == 1 && zero_read.GetFrontier() == 1 &&
         publish_result.status == SliderStatus::Processed &&
         publish_result.processed_count == 1 && zero_publish.GetFrontier() == 1;
}
} // namespace

int main() {
  if (!processes_available_range_in_order()) return 1;
  if (!respects_upstream_and_retries_module_failure()) return 2;
  if (!rejects_regressed_upstream()) return 3;
  if (!reports_unavailable_views_without_publication()) return 4;
  if (!execution_policy_preserves_default_and_supports_batches()) return 5;
  if (!execution_policy_publishes_cadence_and_partial_progress()) return 6;
  if (!execution_policy_zero_counts_fall_back_to_default()) return 7;
  return 0;
}
