/**
 * @file main.cpp
 * @brief Three-thread RecordTape -> Slider -> Slider composition.
 */

#include <fexma/record_tract/record_tape.hpp>
#include <fexma/record_tract/slider.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>

namespace core = fexma::record_tract;

namespace {

constexpr std::uint32_t payload_size = 16;
constexpr std::uint32_t capacity = 64;
constexpr core::Position record_count = 4096;

using Payload = std::array<std::byte, payload_size>;

void store_u64(std::span<std::byte> bytes, std::size_t offset,
               std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes[offset + index] =
        static_cast<std::byte>((value >> (index * 8u)) & 0xffu);
  }
}

[[nodiscard]] std::uint64_t load_u64(std::span<const std::byte> bytes,
                                     std::size_t offset) noexcept {
  std::uint64_t value{};
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint64_t>(
                 std::to_integer<unsigned char>(bytes[offset + index]))
             << (index * 8u);
  }
  return value;
}

[[nodiscard]] Payload make_payload(core::Position position) noexcept {
  Payload bytes{};
  store_u64(bytes, 0, position);
  store_u64(bytes, sizeof(position), position * 3u + 0x5a5a5a5au);
  return bytes;
}

class ValidatingModule final {
public:
  [[nodiscard]] bool process(const core::RecordView& record) noexcept {
    if (record.payload.size() != payload_size ||
        record.position != expected_position_ ||
        load_u64(record.payload, 0) != expected_position_ ||
        load_u64(record.payload, sizeof(expected_position_)) !=
            expected_position_ * 3u + 0x5a5a5a5au) {
      return false;
    }

    ++expected_position_;
    ++processed_count_;
    return true;
  }

  [[nodiscard]] core::Position processed() const noexcept {
    return processed_count_;
  }

private:
  core::Position expected_position_{};
  core::Position processed_count_{};
};

} // namespace

int main() {
  constexpr core::ExecutionPolicy policy{8, 4};

  core::RecordTape tape;
  ValidatingModule module_a;
  ValidatingModule module_b;

  core::Slider slider_a(tape, tape.GetFrontier(), module_a, policy);
  core::Slider slider_b(tape, slider_a.GetFrontier(), module_b, policy);
  tape.SetTailRef(slider_b.GetFrontier());

  if (tape.open({payload_size, capacity, core::default_alignment}) !=
      core::RecordTapeOpenStatus::Ok) {
    return 1;
  }

  std::atomic<bool> failed{false};
  std::atomic<bool> producer_done{false};

  std::thread producer([&] {
    for (core::Position position = 0; position < record_count;) {
      if (failed.load(std::memory_order_acquire)) return;

      const Payload bytes = make_payload(position);
      const core::PublishResult result =
          tape.try_publish(std::span<const std::byte>{bytes});
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

  std::thread executor_a([&] {
    while (!failed.load(std::memory_order_acquire) &&
           slider_a.current() < record_count) {
      const core::SliderStatus result = slider_a.process();
      if (result == core::SliderStatus::Processed) continue;
      if (result == core::SliderStatus::Empty) {
        std::this_thread::yield();
        continue;
      }
      failed.store(true, std::memory_order_release);
      return;
    }
  });

  std::thread executor_b([&] {
    while (!failed.load(std::memory_order_acquire) &&
           slider_b.current() < record_count) {
      const core::SliderStatus result = slider_b.process();
      if (result == core::SliderStatus::Processed) continue;
      if (result == core::SliderStatus::Empty) {
        std::this_thread::yield();
        continue;
      }
      failed.store(true, std::memory_order_release);
      return;
    }
  });

  producer.join();
  executor_a.join();
  executor_b.join();

  const bool valid =
      !failed.load(std::memory_order_acquire) &&
      producer_done.load(std::memory_order_acquire) &&
      module_a.processed() == record_count &&
      module_b.processed() == record_count &&
      slider_a.current() == record_count &&
      slider_b.current() == record_count && tape.head() == record_count &&
      tape.tail() == record_count;

  tape.close();
  return valid ? 0 : 2;
}
