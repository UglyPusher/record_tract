#pragma once

/**
 * @file persistence.hpp
 * @brief Persistence mechanism over a RecordTape.
 */

#include <fexma/record_tract/record_tape_types.hpp>
#include <fexma/record_tract/record_tape.hpp>
#include <fexma/record_tract/slider.hpp>
#include <fexma/wal/types.hpp>

#include <atomic>
#include <concepts>
#include <filesystem>
#include <memory>

namespace fexma::wal {

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

namespace detail {
class PhysicalWalAdapter;
class PersistenceCore final {
public:
  PersistenceCore(const RecordTape& source, PersistencePolicy policy) noexcept;
  ~PersistenceCore();

  PersistenceCore(const PersistenceCore&) = delete;
  PersistenceCore& operator=(const PersistenceCore&) = delete;

  [[nodiscard]] OpenResult open(const std::filesystem::path& path,
                                const PhysicalWalConfig& config) noexcept;
  [[nodiscard]] bool close() noexcept;
  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] bool failed() const noexcept;
  [[nodiscard]] SliderResult process_until(Position available_end) noexcept;
  [[nodiscard]] Position GetFrontier() const noexcept;
  void reset_quiescent(Position initial) noexcept;

private:
  [[nodiscard]] bool append(const RecordView& record) noexcept;
  [[nodiscard]] bool sync() noexcept;
  void publish(Position end) noexcept;

  const RecordTape& source_;
  std::unique_ptr<PhysicalWalAdapter> physical_wal_{};
  std::uint64_t first_sequence_{};
  std::atomic<bool> failed_{false};
  const PersistencePolicy policy_;
  alignas(64) std::atomic<Position> frontier_{};
};
} // namespace detail

template <class Predecessor>
class Persistence final {
public:
  Persistence(const RecordTape& source, const Predecessor& predecessor,
              PersistencePolicy policy = {}) noexcept
      : predecessor_(predecessor), core_(source, policy) {}

  explicit Persistence(const RecordTape& source,
                       PersistencePolicy policy = {}) noexcept
      requires std::same_as<Predecessor, RecordTape>
      : Persistence(source, source, policy) {}

  ~Persistence() = default;

  Persistence(const Persistence&) = delete;
  Persistence& operator=(const Persistence&) = delete;
  Persistence(Persistence&&) = delete;
  Persistence& operator=(Persistence&&) = delete;

  [[nodiscard]] OpenResult open(const std::filesystem::path& path,
                                const PhysicalWalConfig& wal_config_value) noexcept {
    return core_.open(path, wal_config_value);
  }
  [[nodiscard]] bool close() noexcept { return core_.close(); }
  [[nodiscard]] bool is_open() const noexcept { return core_.is_open(); }
  [[nodiscard]] bool failed() const noexcept { return core_.failed(); }

  [[nodiscard]] SliderResult process_available() noexcept {
    return core_.process_until(predecessor_.GetFrontier());
  }
  [[nodiscard]] Position GetFrontier() const noexcept {
    return core_.GetFrontier();
  }
  [[nodiscard]] Position current() const noexcept { return GetFrontier(); }
  void reset_quiescent(Position initial) noexcept {
    core_.reset_quiescent(initial);
  }

private:
  const Predecessor& predecessor_;
  detail::PersistenceCore core_;
};

Persistence(const RecordTape&) -> Persistence<RecordTape>;

Persistence(const RecordTape&, PersistencePolicy) -> Persistence<RecordTape>;

} // namespace fexma::wal
