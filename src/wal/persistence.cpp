/**
 * @file persistence.cpp
 * @brief Persistence mechanism over a RecordTape implementation.
 */

#include <fexma/wal/format.hpp>
#include <fexma/wal/persistence.hpp>

#include "physical_wal_adapter.hpp"

#include <limits>
#include <new>

namespace fexma::wal {

detail::PersistenceCore::PersistenceCore(const RecordTape& source,
                                         PersistencePolicy policy) noexcept
    : source_(source), policy_(policy.sync_count == 0 ? PersistencePolicy{} : policy) {}

detail::PersistenceCore::~PersistenceCore() { (void)close(); }

OpenResult detail::PersistenceCore::open(
    const std::filesystem::path& path,
    const PhysicalWalConfig& config) noexcept {
  if (is_open()) return {OpenStatus::AlreadyOpen};
  const WalConfig adapter_config{
      config.payload_size, 1, config.alignment, config.payload_schema_version,
      config.stream_kind, config.stream_id, config.epoch_id,
      config.first_sequence, config.manifest_id};
  if (!valid_config(adapter_config)) return {OpenStatus::InvalidConfig};

  physical_wal_.reset(new (std::nothrow) detail::PhysicalWalAdapter{});
  if (!physical_wal_) return {OpenStatus::AllocationFailed};
  const OpenStatus status = physical_wal_->create(path, adapter_config);
  if (status != OpenStatus::Ok) {
    physical_wal_.reset();
    return {status};
  }
  first_sequence_ = config.first_sequence;
  failed_.store(false, std::memory_order_relaxed);
  return {OpenStatus::Ok};
}

bool detail::PersistenceCore::append(const RecordView& record) noexcept {
  if (!is_open() || failed_.load(std::memory_order_acquire) ||
      record.position > std::numeric_limits<std::uint64_t>::max() -
                            first_sequence_ ||
      !physical_wal_->append_record(first_sequence_ + record.position,
                                    record.payload)) {
    failed_.store(true, std::memory_order_release);
    return false;
  }
  return true;
}

bool detail::PersistenceCore::sync() noexcept {
  if (!is_open() || failed_.load(std::memory_order_acquire) ||
      !physical_wal_->sync()) {
    failed_.store(true, std::memory_order_release);
    return false;
  }
  return true;
}

bool detail::PersistenceCore::close() noexcept {
  if (!physical_wal_) return true;
  const bool closed = physical_wal_->close();
  physical_wal_.reset();
  return closed;
}

bool detail::PersistenceCore::is_open() const noexcept {
  return physical_wal_ && physical_wal_->is_open();
}

bool detail::PersistenceCore::failed() const noexcept {
  return failed_.load(std::memory_order_acquire);
}

SliderResult detail::PersistenceCore::process_until(Position available_end) noexcept {
  Position current = GetFrontier();
  if (available_end < current) {
    return {SliderStatus::UpstreamRegression, current, 0};
  }
  if (available_end == current) {
    return {SliderStatus::Empty, current, 0};
  }

  const Position available_count = available_end - current;
  const Position count = available_count < policy_.sync_count
                             ? available_count
                             : policy_.sync_count;
  const Position batch_end = current + count;
  std::uint64_t processed_count = 0;
  while (current < batch_end) {
    const AccessResult access = source_.try_view(current);
    if (!access.ok()) {
      return {SliderStatus::ViewUnavailable, current, processed_count,
              access.status};
    }
    if (!append(access.record)) {
      return {SliderStatus::ModuleFailed, current, processed_count};
    }
    ++current;
    ++processed_count;
  }

  if (!sync()) {
    return {SliderStatus::ModuleFailed, current, processed_count};
  }
  publish(current);
  return {SliderStatus::Processed, current, processed_count};
}

Position detail::PersistenceCore::GetFrontier() const noexcept {
  return frontier_.load(std::memory_order_acquire);
}

void detail::PersistenceCore::reset_quiescent(Position initial) noexcept {
  frontier_.store(initial, std::memory_order_relaxed);
}

void detail::PersistenceCore::publish(Position end) noexcept {
  frontier_.store(end, std::memory_order_release);
}

} // namespace fexma::wal
