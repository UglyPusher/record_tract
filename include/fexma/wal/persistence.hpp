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
#include <filesystem>
#include <memory>

namespace fexma::wal {

struct PhysicalWalConfig {
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
  PersistenceCore(const fexma::record_tract::RecordTape& source,
                  PersistencePolicy policy) noexcept;
  ~PersistenceCore();

  PersistenceCore(const PersistenceCore&) = delete;
  PersistenceCore& operator=(const PersistenceCore&) = delete;

  [[nodiscard]] OpenResult open(const std::filesystem::path& path,
                                const PhysicalWalConfig& config) noexcept;
  [[nodiscard]] bool close() noexcept;
  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] bool failed() const noexcept;
  [[nodiscard]] fexma::record_tract::SliderStatus
  process_until(fexma::record_tract::Position available_end) noexcept;
  [[nodiscard]] const fexma::record_tract::Frontier& GetFrontier() const noexcept;

private:
  [[nodiscard]] bool append(
      const fexma::record_tract::RecordView& record) noexcept;
  [[nodiscard]] bool sync() noexcept;
  void publish(fexma::record_tract::Position end) noexcept;

  const fexma::record_tract::RecordTape& source_;
  std::unique_ptr<PhysicalWalAdapter> physical_wal_{};
  std::uint64_t first_sequence_{};
  std::atomic<bool> failed_{false};
  bool opened_once_{false};
  const PersistencePolicy policy_;
  alignas(64) fexma::record_tract::Frontier frontier_{};
};
} // namespace detail

class Persistence final {
public:
  Persistence(const fexma::record_tract::RecordTape& source,
              const fexma::record_tract::Frontier& upstream_frontier,
              PersistencePolicy policy = {}) noexcept
      : upstream_frontier_(&upstream_frontier), core_(source, policy) {}

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

  [[nodiscard]] fexma::record_tract::SliderStatus process() noexcept {
    const fexma::record_tract::Position available_end =
        upstream_frontier_->load(std::memory_order_acquire);
    return core_.process_until(available_end);
  }
  [[nodiscard]] const fexma::record_tract::Frontier& GetFrontier() const noexcept {
    return core_.GetFrontier();
  }
  [[nodiscard]] fexma::record_tract::Position current() const noexcept {
    return GetFrontier().load(std::memory_order_acquire);
  }

private:
  const fexma::record_tract::Frontier* upstream_frontier_;
  detail::PersistenceCore core_;
};

} // namespace fexma::wal
