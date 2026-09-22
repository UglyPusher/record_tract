/**
 * @file test_core_fail_fast.cpp
 * @brief Portable fail-fast contract tests for Core runtime invariants.
 */

#include <fexma/record_tract/record_tape.hpp>
#include <fexma/record_tract/slider.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <span>

using namespace fexma::record_tract;

namespace {

using Payload = std::array<std::byte, 16>;

void terminate_successfully() noexcept { std::_Exit(0); }

[[nodiscard]] bool is_scenario(const char* actual, const char* expected) {
  return std::strcmp(actual, expected) == 0;
}

[[nodiscard]] Payload payload(Position position) noexcept {
  Payload result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] =
        static_cast<std::byte>((position + index * 37u) & 0xffu);
  }
  return result;
}

class NoOpModule final {
public:
  [[nodiscard]] bool process(const RecordView&) noexcept { return true; }
};

[[nodiscard]] bool open_tape(RecordTape& tape, std::uint32_t capacity) {
  return tape.open({static_cast<std::uint32_t>(sizeof(Payload)), capacity,
                    default_alignment}) == RecordTapeOpenStatus::Ok;
}

[[nodiscard]] bool publish(RecordTape& tape, Position position) noexcept {
  const Payload bytes = payload(position);
  const PublishResult result =
      tape.try_publish(std::span<const std::byte>{bytes});
  return result.status == PublishStatus::Ok && result.position == position;
}

int set_tail_ref_open() {
  Frontier configured_terminal{};
  Frontier replacement_terminal{};
  RecordTape tape;
  tape.SetTailRef(configured_terminal);
  if (!open_tape(tape, 1)) return 3;

  std::set_terminate(terminate_successfully);
  tape.SetTailRef(replacement_terminal);
  return 4;
}

int set_tail_ref_closed() {
  Frontier configured_terminal{};
  Frontier replacement_terminal{};
  RecordTape tape;
  tape.SetTailRef(configured_terminal);
  if (!open_tape(tape, 1)) return 3;
  tape.close();

  std::set_terminate(terminate_successfully);
  tape.SetTailRef(replacement_terminal);
  return 4;
}

int reopen_closed() {
  RecordTape tape;
  if (!open_tape(tape, 1)) return 3;
  tape.close();

  std::set_terminate(terminate_successfully);
  (void)tape.open({static_cast<std::uint32_t>(sizeof(Payload)), 1,
                   default_alignment});
  return 4;
}

int zero_read_count() {
  RecordTape tape;
  NoOpModule module;

  std::set_terminate(terminate_successfully);
  [[maybe_unused]] Slider slider(tape, tape.GetFrontier(), module,
                                 ExecutionPolicy{0, 1});
  return 4;
}

int zero_publish_count() {
  RecordTape tape;
  NoOpModule module;

  std::set_terminate(terminate_successfully);
  [[maybe_unused]] Slider slider(tape, tape.GetFrontier(), module,
                                 ExecutionPolicy{1, 0});
  return 4;
}

int tail_ahead_of_head() {
  Frontier terminal{1};
  RecordTape tape;
  tape.SetTailRef(terminal);
  if (!open_tape(tape, 1) || tape.head() != 0) return 3;

  const Payload bytes = payload(0);
  std::set_terminate(terminate_successfully);
  (void)tape.try_publish(std::span<const std::byte>{bytes});
  return 4;
}

int retained_range_exceeds_capacity() {
  Frontier terminal{};
  RecordTape tape;
  tape.SetTailRef(terminal);
  if (!open_tape(tape, 2) || !publish(tape, 0) || !publish(tape, 1)) {
    return 3;
  }

  terminal.store(2, std::memory_order_release);
  if (!publish(tape, 2) || !publish(tape, 3) || tape.head() != 4) return 4;

  terminal.store(0, std::memory_order_release);
  const Payload bytes = payload(4);
  std::set_terminate(terminate_successfully);
  (void)tape.try_publish(std::span<const std::byte>{bytes});
  return 5;
}

int slider_upstream_regression() {
  Frontier upstream{};
  RecordTape tape;
  NoOpModule module;
  Slider slider(tape, upstream, module);
  tape.SetTailRef(slider.GetFrontier());
  if (!open_tape(tape, 1) || !publish(tape, 0)) return 3;

  upstream.store(1, std::memory_order_release);
  if (slider.process() != SliderStatus::Processed ||
      slider.current() != 1) {
    return 4;
  }
  upstream.store(0, std::memory_order_release);

  std::set_terminate(terminate_successfully);
  (void)slider.process();
  return 5;
}

int slider_view_unavailable() {
  Frontier upstream{};
  RecordTape tape;
  NoOpModule module;
  Slider slider(tape, upstream, module);
  if (!open_tape(tape, 1) || tape.head() != 0 || slider.current() != 0) {
    return 3;
  }

  upstream.store(1, std::memory_order_release);
  std::set_terminate(terminate_successfully);
  (void)slider.process();
  return 4;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;

  if (is_scenario(argv[1], "set_tail_ref_open")) {
    return set_tail_ref_open();
  }
  if (is_scenario(argv[1], "set_tail_ref_closed")) {
    return set_tail_ref_closed();
  }
  if (is_scenario(argv[1], "reopen_closed")) return reopen_closed();
  if (is_scenario(argv[1], "zero_read_count")) return zero_read_count();
  if (is_scenario(argv[1], "zero_publish_count")) {
    return zero_publish_count();
  }
  if (is_scenario(argv[1], "tail_ahead_of_head")) {
    return tail_ahead_of_head();
  }
  if (is_scenario(argv[1], "retained_range_exceeds_capacity")) {
    return retained_range_exceeds_capacity();
  }
  if (is_scenario(argv[1], "slider_upstream_regression")) {
    return slider_upstream_regression();
  }
  if (is_scenario(argv[1], "slider_view_unavailable")) {
    return slider_view_unavailable();
  }
  return 2;
}
