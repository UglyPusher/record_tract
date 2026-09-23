/**
 * @file demo_app.cpp
 * @brief Three-thread RecordTape -> Slider -> Slider demo application.
 *
 * A producer publishes bounded records to the tape. The validation slider
 * follows the tape Head, the hash slider follows the validation frontier,
 * and the hash frontier serves as Tail so processed slots can be reclaimed.
 * Each runtime role executes on its own thread.
 */

#include "demo_modules.hpp"
#include "demo_payload.hpp"

#include <fexma/record_tract/record_tape.hpp>
#include <fexma/record_tract/slider.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <thread>

namespace {

namespace core = fexma::record_tract;
namespace demo = basic_tract_demo;

constexpr std::uint32_t capacity = 64;
constexpr core::Position record_count = 4096;
constexpr core::RecordTapeConfig tape_config{
    .payload_size = demo::payload_size,
    .capacity = capacity,
    .alignment = core::default_alignment,
};
constexpr core::ExecutionPolicy policy{
    .read_count = 8,
    .publish_count = 4,
};

using ValidationSlider = core::Slider<demo::PayloadValidationModule>;
using HashSlider = core::Slider<demo::RollingHashModule>;

void run_producer(core::RecordTape& tape, std::atomic<bool>& failed);
void run_validation_stage(ValidationSlider& slider,
                          std::atomic<bool>& failed);
void run_hash_stage(HashSlider& slider, std::atomic<bool>& failed);

} // namespace

int main() {
  core::RecordTape tape;
  demo::PayloadValidationModule validation_module;
  demo::RollingHashModule hash_module;

  // Tape Head is the upstream frontier for the first stage.
  core::Slider validation_slider(tape, tape.GetFrontier(), validation_module,
                                 policy);
  // The validation frontier releases records to the hash stage.
  core::Slider hash_slider(tape, validation_slider.GetFrontier(), hash_module,
                           policy);

  // The terminal frontier is Tail, so slots can be reclaimed after hashing.
  tape.SetTailRef(hash_slider.GetFrontier());

  if (tape.open(tape_config) != core::RecordTapeOpenStatus::Ok) {
    return 1;
  }

  std::atomic<bool> failed{false};

  std::thread producer_thread(run_producer, std::ref(tape), std::ref(failed));
  std::thread validation_thread(run_validation_stage,
                                std::ref(validation_slider), std::ref(failed));
  std::thread hash_thread(run_hash_stage, std::ref(hash_slider),
                           std::ref(failed));

  producer_thread.join();
  validation_thread.join();
  hash_thread.join();

  const bool valid =
      !failed.load(std::memory_order_acquire) &&
      validation_module.processed() == record_count &&
      hash_module.processed() == record_count &&
      hash_module.value() == demo::expected_hash(record_count) &&
      validation_slider.current() == record_count &&
      hash_slider.current() == tape.head() && tape.head() == record_count;

  tape.close();
  return valid ? 0 : 2;
}

namespace {

void run_producer(core::RecordTape& tape, std::atomic<bool>& failed) {
  for (core::Position position = 0; position < record_count;) {
    if (failed.load(std::memory_order_acquire)) return;

    const demo::DemoMessage message{position,
                                   position * 3u + 0x5a5a5a5au};
    const demo::Payload payload = demo::encode(message);
    const core::PublishResult result =
        tape.try_publish(std::span<const std::byte>{payload});

    switch (result.status) {
    case core::PublishStatus::Ok:
      if (result.position != position) {
        failed.store(true, std::memory_order_release);
        return;
      }
      ++position;
      break;
    case core::PublishStatus::Full:
      // Tail has not advanced far enough to reuse the next bounded slot.
      std::this_thread::yield();
      break;
    case core::PublishStatus::InvalidPayloadSize:
    case core::PublishStatus::PositionExhausted:
    case core::PublishStatus::Closed:
      failed.store(true, std::memory_order_release);
      return;
    }
  }
}

void run_validation_stage(ValidationSlider& slider,
                          std::atomic<bool>& failed) {
  while (!failed.load(std::memory_order_acquire) &&
         slider.current() < record_count) {
    switch (slider.process()) {
    case core::SliderStatus::Processed:
      continue;
    case core::SliderStatus::Empty:
      // Head has not published another record for this stage yet.
      std::this_thread::yield();
      continue;
    case core::SliderStatus::ModuleFailed:
      failed.store(true, std::memory_order_release);
      return;
    }
  }
}

void run_hash_stage(HashSlider& slider, std::atomic<bool>& failed) {
  while (!failed.load(std::memory_order_acquire) &&
         slider.current() < record_count) {
    switch (slider.process()) {
    case core::SliderStatus::Processed:
      continue;
    case core::SliderStatus::Empty:
      // The validation slider has not released another record yet.
      std::this_thread::yield();
      continue;
    case core::SliderStatus::ModuleFailed:
      failed.store(true, std::memory_order_release);
      return;
    }
  }
}

} // namespace
