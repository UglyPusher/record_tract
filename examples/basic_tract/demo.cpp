/**
 * @file demo.cpp
 * @brief Three-thread RecordTape -> Slider -> Slider composition.
 */

#include "demo.hpp"

#include "demo_modules.hpp"
#include "demo_payload.hpp"

#include <fexma/record_tract/record_tape.hpp>
#include <fexma/record_tract/slider.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <thread>

namespace basic_tract_demo {

namespace {

constexpr std::uint32_t capacity = 64;
constexpr core::Position record_count = 4096;
constexpr core::ExecutionPolicy policy{8, 4};

using ValidationSlider = core::Slider<PayloadValidationModule>;
using HashSlider = core::Slider<RollingHashModule>;

void run_producer(core::RecordTape& tape, std::atomic<bool>& failed,
                  std::atomic<bool>& producer_done);
void run_validation_stage(ValidationSlider& slider,
                          std::atomic<bool>& failed);
void run_hash_stage(HashSlider& slider, std::atomic<bool>& failed);

} // namespace

int run_demo() {
  core::RecordTape tape;
  PayloadValidationModule module_a;
  RollingHashModule module_b;

  core::Slider slider_a(tape, tape.GetFrontier(), module_a, policy);
  core::Slider slider_b(tape, slider_a.GetFrontier(), module_b, policy);

  // The last completed frontier is the boundary for reclaiming tape slots.
  tape.SetTailRef(slider_b.GetFrontier());

  if (tape.open({payload_size, capacity, core::default_alignment}) !=
      core::RecordTapeOpenStatus::Ok) {
    return 1;
  }

  std::atomic<bool> failed{false};
  std::atomic<bool> producer_done{false};

  std::thread producer(run_producer, std::ref(tape), std::ref(failed),
                       std::ref(producer_done));
  std::thread stage_a(run_validation_stage, std::ref(slider_a),
                      std::ref(failed));
  std::thread stage_b(run_hash_stage, std::ref(slider_b), std::ref(failed));

  producer.join();
  stage_a.join();
  stage_b.join();

  const bool valid =
      !failed.load(std::memory_order_acquire) &&
      producer_done.load(std::memory_order_acquire) &&
      module_a.processed() == record_count &&
      module_b.processed() == record_count &&
      module_b.value() == expected_hash(record_count) &&
      slider_a.current() == record_count &&
      slider_b.current() == record_count && tape.head() == record_count &&
      tape.tail() == record_count;

  tape.close();
  return valid ? 0 : 2;
}

namespace {

void run_producer(core::RecordTape& tape, std::atomic<bool>& failed,
                  std::atomic<bool>& producer_done) {
  for (core::Position position = 0; position < record_count;) {
    if (failed.load(std::memory_order_acquire)) return;

    const Payload payload = make_payload(position);
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
      // Slider A has not released another record for this stage yet.
      std::this_thread::yield();
      continue;
    }
    failed.store(true, std::memory_order_release);
    return;
  }
}

} // namespace

} // namespace basic_tract_demo
