#include <fexma/record_tract/record_tape.hpp>
#include <fexma/record_tract/slider.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace core = fexma::record_tract;

namespace {

using Payload = std::array<std::byte, 8>;

class Module final {
public:
  [[nodiscard]] bool process(const core::RecordView& record) noexcept {
    processed_position_ = record.position;
    return record.payload.size() == sizeof(Payload);
  }

  [[nodiscard]] core::Position processed_position() const noexcept {
    return processed_position_;
  }

private:
  core::Position processed_position_{1};
};

} // namespace

int main() {
  core::RecordTape tape;
  Module module;
  core::Slider slider(tape, tape.GetFrontier(), module);
  tape.SetTailRef(slider.GetFrontier());

  if (tape.open({sizeof(Payload), 4, core::default_alignment}) !=
      core::RecordTapeOpenStatus::Ok) {
    return 1;
  }

  const Payload payload{};
  const core::PublishResult published =
      tape.try_publish(std::span<const std::byte>{payload});
  if (!published.ok() || published.position != 0 || tape.head() != 1) {
    return 2;
  }

  if (slider.process() != core::SliderStatus::Processed ||
      slider.current() != 1 || module.processed_position() != 0) {
    return 3;
  }

  tape.close();
  return tape.is_open() ? 4 : 0;
}
