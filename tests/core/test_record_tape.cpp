/**
 * @file test_record_tape.cpp
 * @brief Contract tests for the persistence-free bounded RecordTape.
 */

#include <fexma/record_tract/record_tape.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <thread>

using namespace fexma::record_tract;

namespace {

using Payload = std::array<std::byte, 16>;

[[nodiscard]] Payload payload(Position position) noexcept {
  Payload bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] =
        static_cast<std::byte>((position + index * 37u) & 0xffu);
  }
  return bytes;
}

[[nodiscard]] bool opens_without_physical_storage() {
  RecordTape tape;
  if (tape.open({}) != RecordTapeOpenStatus::InvalidConfig ||
      tape.open({16, 0, 64}) != RecordTapeOpenStatus::InvalidConfig ||
      tape.open({16, 4, 24}) != RecordTapeOpenStatus::InvalidConfig ||
      tape.open({16, 4, 64}) != RecordTapeOpenStatus::Ok ||
      tape.open({16, 4, 64}) != RecordTapeOpenStatus::AlreadyOpen) {
    return false;
  }

  const auto unexpected_path =
      std::filesystem::temp_directory_path() /
      "fexma_record_tape_has_no_file.wal";
  std::filesystem::remove(unexpected_path);
  const bool no_file = !std::filesystem::exists(unexpected_path);
  tape.close();
  tape.close();
  return no_file && !tape.is_open() &&
         tape.try_publish(payload(0)).status == PublishStatus::Closed &&
         tape.try_view(0).status == ViewStatus::Closed;
}

[[nodiscard]] bool defaults_to_head_as_terminal_frontier() {
  RecordTape tape;
  if (tape.open({16, 1, 64}) != RecordTapeOpenStatus::Ok ||
      !tape.try_publish(payload(0)).ok() ||
      tape.tail() != tape.head() ||
      tape.try_view(0).status != ViewStatus::Reclaimed ||
      !tape.try_publish(payload(1)).ok()) {
    return false;
  }
  tape.close();
  return true;
}

[[nodiscard]] bool follows_terminal_frontier_for_reuse() {
  Frontier terminal{};
  RecordTape tape;
  tape.SetTailRef(terminal);
  if (tape.open({16, 3, 64}) != RecordTapeOpenStatus::Ok) return false;

  for (Position position = 0; position < 3; ++position) {
    const PublishResult published = tape.try_publish(payload(position));
    if (!published.ok() || published.position != position) return false;
  }
  if (tape.try_publish(payload(3)).status != PublishStatus::Full) {
    return false;
  }

  terminal.store(2, std::memory_order_release);
  if (tape.tail() != 2 ||
      tape.try_view(1).status != ViewStatus::Reclaimed ||
      !tape.try_publish(payload(3)).ok() ||
      !tape.try_publish(payload(4)).ok()) {
    return false;
  }

  for (Position position = 2; position < 5; ++position) {
    const AccessResult access = tape.try_view(position);
    if (!access.ok() || access.record.position != position ||
        access.record.payload.size() != 16) {
      return false;
    }
    for (std::size_t index = 0; index < access.record.payload.size(); ++index) {
      if (access.record.payload[index] != payload(position)[index]) return false;
    }
  }
  terminal.store(5, std::memory_order_release);
  if (tape.tail() != 5 || tape.tail() != tape.head()) return false;
  tape.close();
  return true;
}

[[nodiscard]] bool producer_and_terminal_frontier_wrap_concurrently() {
  constexpr Position count = 50'000;
  Frontier terminal{};
  RecordTape tape;
  tape.SetTailRef(terminal);
  if (tape.open({16, 128, 64}) != RecordTapeOpenStatus::Ok) return false;
  std::atomic<bool> failed{false};

  std::thread producer([&] {
    for (Position position = 0; position < count;) {
      const PublishResult published = tape.try_publish(payload(position));
      if (published.ok()) {
        if (published.position != position) failed = true;
        ++position;
      } else if (published.status != PublishStatus::Full) {
        failed = true;
        return;
      } else {
        std::this_thread::yield();
      }
    }
  });

  std::thread terminal_stage([&] {
    for (Position position = 0; position < count;) {
      const AccessResult access = tape.try_view(position);
      if (access.status == ViewStatus::Unpublished) {
        std::this_thread::yield();
        continue;
      }
      if (!access.ok() || access.record.position != position) {
        failed = true;
        return;
      }
      const Payload expected = payload(position);
      for (std::size_t index = 0; index < expected.size(); ++index) {
        if (access.record.payload[index] != expected[index]) {
          failed = true;
          return;
        }
      }
      terminal.store(position + 1, std::memory_order_release);
      ++position;
    }
  });

  producer.join();
  terminal_stage.join();
  const bool valid = !failed && tape.tail() == count && tape.head() == count;
  tape.close();
  return valid;
}

} // namespace

int main() {
  if (!opens_without_physical_storage()) return 1;
  if (!defaults_to_head_as_terminal_frontier()) return 2;
  if (!follows_terminal_frontier_for_reuse()) return 3;
  if (!producer_and_terminal_frontier_wrap_concurrently()) return 4;
  return 0;
}
