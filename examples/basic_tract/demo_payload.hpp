/**
 * @file demo_payload.hpp
 * @brief Deterministic record format and hashing helpers for the basic demo.
 *
 * Every 16-byte payload stores its tape position and a derived value as two
 * little-endian integers. The same representation is used to calculate the
 * expected final hash during validation.
 */

#pragma once

#include <fexma/record_tract/record_tape_types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace basic_tract_demo {

namespace core = fexma::record_tract;

inline constexpr std::uint32_t payload_size = 16;
inline constexpr std::uint64_t hash_seed = 0x6a09e667f3bcc909ull;
inline constexpr std::uint64_t hash_prime = 0x100000001b3ull;

using Payload = std::array<std::byte, payload_size>;

inline void store_u64(std::span<std::byte> bytes, std::size_t offset,
                      std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes[offset + index] =
        static_cast<std::byte>((value >> (index * 8u)) & 0xffu);
  }
}

[[nodiscard]] inline std::uint64_t
load_u64(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  std::uint64_t value{};
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint64_t>(
                 std::to_integer<unsigned char>(bytes[offset + index]))
             << (index * 8u);
  }
  return value;
}

[[nodiscard]] inline Payload make_payload(core::Position position) noexcept {
  Payload bytes{};
  store_u64(bytes, 0, position);
  store_u64(bytes, sizeof(position), position * 3u + 0x5a5a5a5au);
  return bytes;
}

[[nodiscard]] inline std::uint64_t
hash_payload(std::uint64_t state,
             std::span<const std::byte> payload) noexcept {
  for (const std::byte byte : payload) {
    state ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(byte));
    state *= hash_prime;
    state ^= state >> 29u;
  }
  return state;
}

[[nodiscard]] inline std::uint64_t
expected_hash(core::Position count) noexcept {
  std::uint64_t hash = hash_seed;
  for (core::Position position = 0; position < count; ++position) {
    hash = hash_payload(hash, make_payload(position));
  }
  return hash;
}

} // namespace basic_tract_demo
