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
constexpr core::ExecutionPolicy policy{8, 4};

using ValidationSlider = core::Slider<demo::PayloadValidationModule>;
using HashSlider = core::Slider<demo::RollingHashModule>;

void run_producer(core::RecordTape& tape, std::atomic<bool>& failed,
                  std::atomic<bool>& producer_done);
void run_validation_stage(ValidationSlider& slider,
                          std::atomic<bool>& failed);
void run_hash_stage(HashSlider& slider, std::atomic<bool>& failed);

} // namespace

int main() {
  core::RecordTape tape;
  demo::PayloadValidationModule validation;
  demo::RollingHashModule hashing;

  core::Slider validation_slider(tape, tape.GetFrontier(), validation, policy);
  core::Slider hash_slider(tape, validation_slider.GetFrontier(), hashing,
                           policy);

  // The last completed frontier is the boundary for reclaiming tape slots.
  tape.SetTailRef(hash_slider.GetFrontier());

  if (tape.open({demo::payload_size, capacity, core::default_alignment}) !=
      core::RecordTapeOpenStatus::Ok) {
    return 1;
  }

  std::atomic<bool> failed{false};
  std::atomic<bool> producer_done{false};

  std::thread producer(run_producer, std::ref(tape), std::ref(failed),
                       std::ref(producer_done));
  std::thread validation_stage(run_validation_stage,
                               std::ref(validation_slider), std::ref(failed));
  std::thread hash_stage(run_hash_stage, std::ref(hash_slider),
                         std::ref(failed));

  producer.join();
  validation_stage.join();
  hash_stage.join();

  const bool valid =
      !failed.load(std::memory_order_acquire) &&
      producer_done.load(std::memory_order_acquire) &&
      validation.processed() == record_count &&
      hashing.processed() == record_count &&
      hashing.value() == demo::expected_hash(record_count) &&
      validation_slider.current() == record_count &&
      hash_slider.current() == record_count && tape.head() == record_count &&
      tape.tail() == record_count;

  tape.close();
  return valid ? 0 : 2;
}

namespace {

void run_producer(core::RecordTape& tape, std::atomic<bool>& failed,
                  std::atomic<bool>& producer_done) {
  for (core::Position position = 0; position < record_count;) {
    if (failed.load(std::memory_order_acquire)) return;

    const demo::Payload payload = demo::make_payload(position);
    const core::PublishResult result =
        tape.try_publish(std::span<const std::byte>{payload});

    if (result.status == core::PublishStatus::Ok) {
      if (result.position != position) {
        failed.store(true, std::memory_order_release);
        return;
      }
      ++position;
    } else if (result.status == core::PublishStatus::Full) {
      // Tail has not advanced far enough to reuse the next bounded slot.
      std::this_thread::yield();
    } else {
      failed.store(true, std::memory_order_release);
      return;
    }
  }
  producer_done.store(true, std::memory_order_release);
}

void run_validation_stage(ValidationSlider& slider,
                          std::atomic<bool>& failed) {
  while (!failed.load(std::memory_order_acquire) &&
         slider.current() < record_count) {
    const core::SliderStatus status = slider.process();
    if (status == core::SliderStatus::Processed) continue;
    if (status == core::SliderStatus::Empty) {
      // Head has not published another record for this stage yet.
      std::this_thread::yield();
      continue;
    }
    failed.store(true, std::memory_order_release);
    return;
  }
}

void run_hash_stage(HashSlider& slider, std::atomic<bool>& failed) {
  while (!failed.load(std::memory_order_acquire) &&
         slider.current() < record_count) {
    const core::SliderStatus status = slider.process();
    if (status == core::SliderStatus::Processed) continue;
    if (status == core::SliderStatus::Empty) {
      // The validation slider has not released another record yet.
      std::this_thread::yield();
      continue;
    }
    failed.store(true, std::memory_order_release);
    return;
  }
}

} // namespace
