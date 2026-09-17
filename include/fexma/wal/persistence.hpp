#pragma once

/**
 * @file persistence.hpp
 * @brief Persistence mechanism over a RecordTape.
 */

#include <fexma/wal/record_tape_types.hpp>
#include <fexma/wal/record_tape.hpp>
#include <fexma/wal/slider.hpp>
#include <fexma/wal/types.hpp>

#include <atomic>
#include <filesystem>
#include <memory>

namespace fexma::wal {

namespace detail {
class PhysicalWalAdapter;
}

struct PhysicalWalConfig {
  std::uint32_t payload_size{};
  std::uint32_t alignment{wal_default_alignment};
  std::uint32_t payload_schema_version{};
  StreamKind stream_kind{StreamKind::Generic};
  StreamId stream_id{};
  EpochId epoch_id{};
  std::uint64_t first_sequence{1};
  ManifestId manifest_id{};
};

struct PersistencePolicy final {
  std::size_t sync_count{1};
};

class Persistence final {
public:
  explicit Persistence(const RecordTape& source,
                       PersistencePolicy policy = {}) noexcept;
  ~Persistence();

  Persistence(const Persistence&) = delete;
  Persistence& operator=(const Persistence&) = delete;
  Persistence(Persistence&&) = delete;
  Persistence& operator=(Persistence&&) = delete;

  [[nodiscard]] OpenResult open(const std::filesystem::path& path,
                                const PhysicalWalConfig& config) noexcept;
  [[nodiscard]] bool close() noexcept;
  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] bool failed() const noexcept;

  [[nodiscard]] SliderResult process_available() noexcept;
  [[nodiscard]] Position GetFrontier() const noexcept;
  [[nodiscard]] Position current() const noexcept { return GetFrontier(); }
  void reset_quiescent(Position initial) noexcept;

private:
  [[nodiscard]] bool append(const RecordView& record) noexcept;
  [[nodiscard]] bool sync() noexcept;
  void publish(Position end) noexcept;

  const RecordTape& source_;
  std::unique_ptr<detail::PhysicalWalAdapter> physical_wal_{};
  std::uint64_t first_sequence_{};
  std::atomic<bool> failed_{false};
  const PersistencePolicy policy_;
  alignas(64) std::atomic<Position> frontier_{};
};

} // namespace fexma::wal
