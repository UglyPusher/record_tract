/**
 * @file test_wal_persistence_slider.cpp
 * @brief Contract tests for persistence attached through the generic slider.
 */

#include <fexma/wal/noop_module.hpp>
#include <fexma/wal/persistence.hpp>
#include <fexma/wal/reader.hpp>

#include "physical_wal_file.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

using namespace fexma::wal;

namespace {

using Payload = std::array<std::byte, 8>;

class PhysicalControlGuard final {
public:
  explicit PhysicalControlGuard(
      detail::PhysicalWalFileTestControl& control) noexcept {
    detail::set_physical_wal_file_test_control(&control);
  }

  ~PhysicalControlGuard() {
    detail::set_physical_wal_file_test_control(nullptr);
  }

  PhysicalControlGuard(const PhysicalControlGuard&) = delete;
  PhysicalControlGuard& operator=(const PhysicalControlGuard&) = delete;
};

[[nodiscard]] std::filesystem::path test_path(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

[[nodiscard]] Payload payload(std::uint64_t value) noexcept {
  Payload result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] =
        static_cast<std::byte>((value >> (index * 8u)) & 0xffu);
  }
  return result;
}

[[nodiscard]] bool equal(std::span<const std::byte> actual,
                         const Payload& expected) noexcept {
  if (actual.size() != expected.size()) return false;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (actual[index] != expected[index]) return false;
  }
  return true;
}

constexpr WalConfig wal_config{static_cast<std::uint32_t>(sizeof(Payload)),
                               8,
                               wal_default_alignment,
                               7,
                               StreamKind::Generic,
                               41,
                               9,
                               100,
                               12};

constexpr PhysicalWalConfig physical_config{
    wal_config.payload_size,           wal_config.alignment,
    wal_config.payload_schema_version, wal_config.stream_kind,
    wal_config.stream_id,              wal_config.epoch_id,
    wal_config.first_sequence,         wal_config.manifest_id};

[[nodiscard]] bool open(RecordTape& tape, Persistence& persistence,
                        const std::filesystem::path& path) {
  return tape.open({wal_config.payload_size, wal_config.capacity,
                   wal_config.alignment})
             .ok() &&
         persistence.open(path, physical_config).ok();
}

[[nodiscard]] bool publish(RecordTape& tape, std::uint64_t value) noexcept {
  const Payload bytes = payload(value);
  return tape.try_publish(std::span<const std::byte>{bytes}).ok();
}

[[nodiscard]] bool batches_sync_then_release_downstream() {
  const auto path = test_path("fexma_wal_persistence_slider_batches.wal");
  std::filesystem::remove(path);

  RecordTape tape;
  Persistence persistence(tape, PersistencePolicy{2});
  if (!open(tape, persistence, path)) return false;
  for (std::uint64_t value = 0; value < 5; ++value) {
    if (!publish(tape, value)) return false;
  }

  Persistence& persistence_slider = persistence;
  NoOpModule no_op;
  Slider no_op_slider(tape, persistence_slider, no_op);

  detail::PhysicalWalFileTestControl control{};
  PhysicalControlGuard guard(control);

  const SliderResult first = persistence_slider.process_available();
  if (!first.ok() || first.processed_count != 2 ||
      persistence_slider.GetFrontier() != 2 || control.append_calls != 2 ||
      control.sync_calls != 1 || !no_op_slider.process_available().ok() ||
      no_op_slider.GetFrontier() != 2 ||
      tape.reclaim(no_op_slider.GetFrontier()) != ReclaimStatus::Ok) {
    return false;
  }

  const SliderResult second = persistence_slider.process_available();
  if (!second.ok() || second.processed_count != 2 ||
      persistence_slider.GetFrontier() != 4 || control.append_calls != 4 ||
      control.sync_calls != 2 || !no_op_slider.process_available().ok() ||
      no_op_slider.GetFrontier() != 4 ||
      tape.reclaim(no_op_slider.GetFrontier()) != ReclaimStatus::Ok) {
    return false;
  }

  const SliderResult third = persistence_slider.process_available();
  const SliderResult empty = persistence_slider.process_available();
  if (!third.ok() || third.processed_count != 1 ||
      empty.status != SliderStatus::Empty || persistence_slider.GetFrontier() != 5 ||
      control.append_calls != 5 || control.sync_calls != 3 ||
      !no_op_slider.process_available().ok() ||
      no_op_slider.GetFrontier() != 5 ||
      tape.reclaim(no_op_slider.GetFrontier()) != ReclaimStatus::Ok ||
      tape.tail() != 5 || tape.head() != 5) {
    return false;
  }

  if (!persistence.close()) return false;
  tape.close();

  WalReader reader;
  if (!reader.open(path, wal_config).ok()) return false;
  Payload output{};
  for (std::uint64_t value = 0; value < 5; ++value) {
    const ReadResult record = reader.read_next(output);
    if (!record.ok() || record.sequence != wal_config.first_sequence + value ||
        !equal(output, payload(value))) {
      return false;
    }
  }
  const bool reader_valid =
      reader.read_next(output).status == ReadStatus::EndOfLog && reader.close();
  const ScanResult scan = scan_wal(path, wal_config);
  const bool valid = reader_valid && scan.status == ScanStatus::Clean &&
                     scan.records == 5 &&
                     scan.last_sequence == wal_config.first_sequence + 4;
  std::filesystem::remove(path);
  return valid;
}

[[nodiscard]] bool strict_default_policy_processes_one(PersistencePolicy policy,
                                                       const char* name) {
  const auto path = test_path(name);
  std::filesystem::remove(path);
  RecordTape tape;
  Persistence persistence(tape, policy);
  if (!open(tape, persistence, path) || !publish(tape, 0) ||
      !publish(tape, 1)) {
    return false;
  }
  detail::PhysicalWalFileTestControl control{};
  bool valid = false;
  {
    PhysicalControlGuard guard(control);
    const SliderResult first = persistence.process_available();
    const SliderResult second = persistence.process_available();
    valid = first.status == SliderStatus::Processed &&
            first.processed_count == 1 && persistence.GetFrontier() == 2 &&
            second.status == SliderStatus::Processed &&
            second.processed_count == 1 && control.sync_calls == 2;
  }
  valid = valid && persistence.close();
  tape.close();
  std::filesystem::remove(path);
  return valid;
}

[[nodiscard]] bool sync_policy_eight_drains_twenty_three() {
  const auto path = test_path("fexma_wal_persistence_policy_eight.wal");
  std::filesystem::remove(path);
  RecordTape tape;
  Persistence persistence(tape, PersistencePolicy{8});
  if (!tape.open({wal_config.payload_size, 32, wal_config.alignment}).ok() ||
      !persistence.open(path, physical_config).ok()) {
    return false;
  }
  for (std::uint64_t value = 0; value < 23; ++value) {
    if (!publish(tape, value)) return false;
  }
  const SliderResult first = persistence.process_available();
  const Position first_frontier = persistence.GetFrontier();
  const SliderResult second = persistence.process_available();
  const Position second_frontier = persistence.GetFrontier();
  const SliderResult third = persistence.process_available();
  const Position third_frontier = persistence.GetFrontier();
  const SliderResult empty = persistence.process_available();
  const bool valid = first.status == SliderStatus::Processed &&
                     first.processed_count == 8 && first_frontier == 8 &&
                     second.status == SliderStatus::Processed &&
                     second.processed_count == 8 && second_frontier == 16 &&
                     third.status == SliderStatus::Processed &&
                     third.processed_count == 7 && third_frontier == 23 &&
                     empty.status == SliderStatus::Empty;
  (void)persistence.close();
  tape.close();
  std::filesystem::remove(path);
  return valid;
}

[[nodiscard]] bool append_failure_does_not_publish() {
  const auto path = test_path("fexma_wal_persistence_slider_append_fail.wal");
  std::filesystem::remove(path);

  RecordTape tape;
  Persistence persistence(tape, PersistencePolicy{2});
  if (!open(tape, persistence, path) || !publish(tape, 0) || !publish(tape, 1)) {
    return false;
  }
  Persistence& slider = persistence;

  detail::PhysicalWalFileTestControl control{};
  control.fail_append_call = 0;
  PhysicalControlGuard guard(control);
  const SliderResult failed = slider.process_available();
  const bool valid = failed.status == SliderStatus::ModuleFailed &&
                     failed.processed_count == 0 && slider.current() == 0 &&
                     slider.GetFrontier() == 0 && persistence.failed() &&
                     control.append_calls == 1 && control.sync_calls == 0;
  (void)persistence.close();
  tape.close();
  std::filesystem::remove(path);
  return valid;
}

[[nodiscard]] bool sync_failure_hides_batch_but_durable_prefix_drains() {
  const auto path = test_path("fexma_wal_persistence_slider_sync_fail.wal");
  std::filesystem::remove(path);

  RecordTape tape;
  Persistence persistence(tape, PersistencePolicy{2});
  if (!open(tape, persistence, path) || !publish(tape, 0)) return false;

  Persistence& persistence_slider = persistence;
  NoOpModule no_op;
  Slider no_op_slider(tape, persistence_slider, no_op);

  detail::PhysicalWalFileTestControl initial_control{};
  {
    PhysicalControlGuard guard(initial_control);
    if (!persistence_slider.process_available().ok()) return false;
  }
  if (persistence_slider.GetFrontier() != 1 || !publish(tape, 1) || !publish(tape, 2)) {
    return false;
  }

  detail::PhysicalWalFileTestControl failure_control{};
  failure_control.fail_sync_call = 0;
  SliderResult failed{};
  {
    PhysicalControlGuard guard(failure_control);
    failed = persistence_slider.process_available();
  }
  if (failed.status != SliderStatus::ModuleFailed ||
      failed.processed_count != 2 || persistence_slider.GetFrontier() != 1 ||
      !persistence.failed() || failure_control.append_calls != 2 ||
      failure_control.sync_calls != 1) {
    return false;
  }

  const SliderResult drained = no_op_slider.process_available();
  const SliderResult hidden = no_op_slider.process_available();
  const bool valid = drained.ok() && drained.processed_count == 1 &&
                     hidden.status == SliderStatus::Empty &&
                     no_op_slider.GetFrontier() == 1 &&
                     tape.reclaim(no_op_slider.GetFrontier()) ==
                         ReclaimStatus::Ok &&
                     tape.tail() == 1 && tape.head() == 3;
  (void)persistence.close();
  tape.close();
  std::filesystem::remove(path);
  return valid;
}

} // namespace

int main() {
  if (!strict_default_policy_processes_one(PersistencePolicy{},
                                           "fexma_wal_persistence_default.wal"))
    return 1;
  if (!strict_default_policy_processes_one(
          PersistencePolicy{0}, "fexma_wal_persistence_zero.wal"))
    return 2;
  if (!sync_policy_eight_drains_twenty_three()) return 3;
  if (!batches_sync_then_release_downstream()) return 4;
  if (!append_failure_does_not_publish()) return 5;
  if (!sync_failure_hides_batch_but_durable_prefix_drains()) return 6;
  return 0;
}
