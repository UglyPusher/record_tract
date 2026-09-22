#pragma once

/**
 * @file record_tape.hpp
 * @brief Bounded in-memory RecordTape with head and tail boundaries.
 */

#include <fexma/record_tract/record_tape_types.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fexma::record_tract {

struct RecordTapeConfig {
  std::uint32_t payload_size{};
  std::uint32_t capacity{};
  std::uint32_t alignment{default_alignment};
};

enum class RecordTapeOpenStatus : std::uint8_t {
  Ok,
  InvalidConfig,
  AllocationFailed,
  AlreadyOpen
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // Intentional cache-line boundary isolation.
#endif

class RecordTape final {
public:
  RecordTape() noexcept;
  ~RecordTape();

  RecordTape(const RecordTape&) = delete;
  RecordTape& operator=(const RecordTape&) = delete;
  RecordTape(RecordTape&&) = delete;
  RecordTape& operator=(RecordTape&&) = delete;

  [[nodiscard]] RecordTapeOpenStatus
  open(const RecordTapeConfig& config) noexcept;
  [[nodiscard]] PublishResult
  try_publish(std::span<const std::byte> payload) noexcept;
  [[nodiscard]] AccessResult try_view(Position position) const noexcept;

  // Cold-path topology wiring. The terminal Frontier reference is non-owning.
  void SetTailRef(const Frontier& frontier) noexcept;

  // Cold-path lifecycle: producer, terminal stage, and view users must be stopped.
  // The terminal Frontier must remain alive until the quiescent call to
  // `RecordTape::close()`. Closing the Tape clears the stored non-owning
  // reference. The terminal Frontier owner may be destroyed after `close()`
  // returns.
  void close() noexcept;

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] Position head() const noexcept;
  [[nodiscard]] std::uint32_t payload_size() const noexcept {
    return payload_size_;
  }
  [[nodiscard]] const Frontier& GetFrontier() const noexcept {
    return head_boundary_.value;
  }

private:
  static constexpr std::size_t cache_line_size = 64;

  struct alignas(cache_line_size) TapeBoundary {
    Frontier value{};
  };

  static_assert(std::atomic<Position>::is_always_lock_free);
  static_assert(alignof(TapeBoundary) == cache_line_size);
  static_assert(sizeof(TapeBoundary) == cache_line_size);

  class Buffer final {
  public:
    Buffer() = default;
    ~Buffer();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    [[nodiscard]] RecordTapeOpenStatus
    initialize(const RecordTapeConfig& config) noexcept;
    void release() noexcept;

    [[nodiscard]] std::span<std::byte>
    block_at_slot(std::uint32_t slot) noexcept;
    [[nodiscard]] std::span<const std::byte>
    block_at_slot(std::uint32_t slot) const noexcept;

  private:
    std::byte* data_{};
    std::size_t stride_{};
    std::uint32_t payload_size_{};
    std::uint32_t alignment_{default_alignment};
  };

  // Each boundary is a complete cache line; adjacent members therefore have
  // distinct cache-line storage.
  TapeBoundary head_boundary_{};
  const Frontier* tail_frontier_;
  Buffer buffer_{};
  std::uint32_t payload_size_{};
  std::uint32_t capacity_{};
  std::atomic<bool> open_{false};
  bool opened_once_{false};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace fexma::record_tract
