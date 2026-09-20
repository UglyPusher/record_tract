/**
 * @file main.cpp
 * @brief Minimal concurrent RecordTape -> Slider composition.
 */

#include <fexma/record_tract/record_tape.hpp>
#include <fexma/record_tract/slider.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>

namespace core = fexma::wal;

namespace {

constexpr std::uint32_t payload_size = 16;
constexpr std::uint32_t capacity = 64;
constexpr core::Position record_count = 4096;

using Payload = std::array<std::byte, payload_size>;

class Module final {
public:
  [[nodiscard]] bool process(const core::RecordView&) noexcept {
    ++processed_;
    return true;
  }

  [[nodiscard]] core::Position processed() const noexcept {
    return processed_;
  }

private:
  core::Position processed_{};
};

} // namespace

int main() {
  core::RecordTape tape;
  Module module;
  core::Slider slider(tape, tape.GetFrontier(), module);
  tape.SetTailRef(slider.GetFrontier());
  if (tape.open({payload_size, capacity, core::default_alignment}) !=
      core::RecordTapeOpenStatus::Ok) {
    return 1;
  }

  std::atomic<bool> failed{false};
  std::atomic<bool> producer_done{false};

  std::thread producer([&] {
    for (core::Position position = 0; position < record_count;) {
      if (failed.load(std::memory_order_acquire)) return;

      const Payload payload{};
      const core::PublishResult result =
          tape.try_publish(std::span<const std::byte>{payload});

      if (result.status == core::PublishStatus::Ok) {
        if (result.position != position) {
          failed.store(true, std::memory_order_release);
          return;
        }
        ++position;
      } else if (result.status == core::PublishStatus::Full) {
        std::this_thread::yield();
      } else {
        failed.store(true, std::memory_order_release);
        return;
      }
    }

    producer_done.store(true, std::memory_order_release);
  });

  std::thread executor([&] {
    for (;;) {
      if (failed.load(std::memory_order_acquire)) return;

      const core::SliderStatus result = slider.process();
      if (result != core::SliderStatus::Processed &&
          result != core::SliderStatus::Empty) {
        failed.store(true, std::memory_order_release);
        return;
      }

      if (producer_done.load(std::memory_order_acquire) &&
          slider.current() == tape.head()) {
        return;
      }

      if (result == core::SliderStatus::Empty) {
        std::this_thread::yield();
      }
    }
  });

  producer.join();
  executor.join();

  const bool valid =
      !failed.load(std::memory_order_acquire) &&
      producer_done.load(std::memory_order_acquire) &&
      module.processed() == record_count &&
      slider.current() == record_count && tape.head() == record_count &&
      tape.tail() == record_count;

  tape.close();
  return valid ? 0 : 2;
}
