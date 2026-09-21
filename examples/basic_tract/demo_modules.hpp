#pragma once

#include "demo_payload.hpp"

#include <fexma/record_tract/record_tape_types.hpp>

#include <cstdint>

namespace basic_tract_demo {

class PayloadValidationModule final {
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

class RollingHashModule final {
public:
  [[nodiscard]] bool process(const core::RecordView& record) noexcept {
    hash_ = hash_payload(hash_, record.payload);
    ++processed_count_;
    return true;
  }

  [[nodiscard]] core::Position processed() const noexcept {
    return processed_count_;
  }

  [[nodiscard]] std::uint64_t value() const noexcept { return hash_; }

private:
  std::uint64_t hash_{hash_seed};
  core::Position processed_count_{};
};

} // namespace basic_tract_demo
