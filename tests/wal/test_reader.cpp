/**
 * @file test_wal_reader.cpp
 * @brief Contract tests for validated WAL reading and scanning.
 */

#include <fexma/wal/format.hpp>
#include <fexma/wal/persistence.hpp>
#include <fexma/wal/reader.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>

using namespace fexma::record_tract;
using namespace fexma::wal;

namespace {

constexpr WalConfig config{8, 8, 64, 11, StreamKind::Command,
                           71, 5, 101, 29};

[[nodiscard]] std::filesystem::path test_path(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

[[nodiscard]] std::array<std::byte, config.payload_size>
payload(std::uint64_t value) noexcept {
  std::array<std::byte, config.payload_size> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::byte>((value + index * 17u) & 0xffu);
  }
  return bytes;
}

[[nodiscard]] bool create_wal(const std::filesystem::path& path,
                              std::uint32_t records,
                              const WalConfig& wal_config) {
  std::filesystem::remove(path);
  RecordTape tape;
  Persistence persistence(tape, tape.GetFrontier(),
                          PersistencePolicy{records == 0 ? 1u : records});
  tape.SetTailRef(persistence.GetFrontier());
  if (tape.open({wal_config.payload_size, records == 0 ? 1u : records,
                 wal_config.alignment}) != RecordTapeOpenStatus::Ok) {
    return false;
  }
  const PhysicalWalConfig physical_config{
      wal_config.alignment,              wal_config.payload_schema_version,
      wal_config.stream_kind,
      wal_config.stream_id,              wal_config.epoch_id,
      wal_config.first_sequence,         wal_config.manifest_id};
  if (!persistence.open(path, physical_config).ok()) {
    return false;
  }
  for (std::uint32_t index = 0; index < records; ++index) {
    const auto bytes = payload(index + 1u);
    if (!tape.try_publish(bytes).ok()) {
      return false;
    }
  }
  const SliderStatus processed = persistence.process();
  return (records == 0 ? processed == SliderStatus::Empty
                       : processed == SliderStatus::Processed) &&
         persistence.close();
}

[[nodiscard]] bool create_wal(const std::filesystem::path& path,
                              std::uint32_t records) {
  return create_wal(path, records, config);
}

void overwrite(const std::filesystem::path& path, std::uint64_t offset,
               std::span<const std::byte> bytes) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekp(static_cast<std::streamoff>(offset));
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] bool validated_read_and_clean_scan() {
  const auto path = test_path("fexma_wal_reader_clean.wal");
  if (!create_wal(path, 3)) {
    return false;
  }

  WalReader reader;
  if (!reader.open(path, config).ok()) {
    return false;
  }
  std::array<std::byte, config.payload_size - 1u> too_small{};
  if (reader.read_next(too_small).status != ReadStatus::InvalidPayloadSize ||
      reader.next_sequence() != config.first_sequence) {
    return false;
  }

  std::array<std::byte, config.payload_size> out{};
  for (std::uint64_t index = 0; index < 3; ++index) {
    const ReadResult read = reader.read_next(out);
    if (!read.ok() || read.sequence != config.first_sequence + index ||
        out != payload(index + 1u)) {
      return false;
    }
  }
  if (reader.read_next(out).status != ReadStatus::EndOfLog ||
      !reader.close()) {
    return false;
  }

  const ScanResult scan = scan_wal(path, config);
  const std::uint64_t expected_offset =
      records_offset(config) + 3u * aligned_record_size(config);
  std::filesystem::remove(path);
  return scan.ok() && scan.records == 3 &&
         scan.last_sequence == config.first_sequence + 2u &&
         scan.last_valid_offset == expected_offset;
}

[[nodiscard]] bool identity_mismatch_is_rejected() {
  const auto path = test_path("fexma_wal_reader_identity.wal");
  if (!create_wal(path, 1)) {
    return false;
  }
  WalConfig wrong = config;
  ++wrong.epoch_id;
  WalReader reader;
  const ReaderOpenResult opened = reader.open(path, wrong);
  std::filesystem::remove(path);
  return opened.status == ReaderOpenStatus::Failed &&
         opened.error == WalReadError::IdentityMismatch;
}

[[nodiscard]] bool header_corruption_is_rejected() {
  const auto path = test_path("fexma_wal_reader_header_crc.wal");
  if (!create_wal(path, 1)) {
    return false;
  }
  const std::array corrupt{std::byte{0xff}};
  overwrite(path, 60, corrupt);
  WalReader reader;
  const ReaderOpenResult opened = reader.open(path, config);
  std::filesystem::remove(path);
  return opened.status == ReaderOpenStatus::Failed &&
         opened.error == WalReadError::InvalidFileHeaderCrc;
}

[[nodiscard]] bool incomplete_tail_is_classified() {
  const auto path = test_path("fexma_wal_reader_incomplete_tail.wal");
  if (!create_wal(path, 2)) {
    return false;
  }
  const std::uint64_t first_end =
      records_offset(config) + aligned_record_size(config);
  std::filesystem::resize_file(path, first_end + 7u);
  const ScanResult scan = scan_wal(path, config);
  std::filesystem::remove(path);
  return scan.status == ScanStatus::IncompleteTail &&
         scan.error == WalReadError::IncompleteRecordHeader &&
         scan.records == 1 && scan.last_sequence == config.first_sequence &&
         scan.last_valid_offset == first_end;
}

[[nodiscard]] bool payload_corruption_is_rejected() {
  const auto path = test_path("fexma_wal_reader_payload_crc.wal");
  if (!create_wal(path, 1)) {
    return false;
  }
  const std::array corrupt{std::byte{0xee}};
  overwrite(path, records_offset(config) + physical_record_header_size,
            corrupt);
  const ScanResult scan = scan_wal(path, config);
  std::filesystem::remove(path);
  return scan.status == ScanStatus::Corrupted &&
         scan.error == WalReadError::InvalidPayloadCrc && scan.records == 0 &&
         scan.last_valid_offset == records_offset(config);
}

[[nodiscard]] bool record_header_corruption_is_rejected() {
  const auto path = test_path("fexma_wal_reader_record_header_crc.wal");
  if (!create_wal(path, 1)) {
    return false;
  }
  const std::array corrupt{std::byte{0xff}};
  overwrite(path, records_offset(config) + 20u, corrupt);
  const ScanResult scan = scan_wal(path, config);
  std::filesystem::remove(path);
  return scan.status == ScanStatus::Corrupted &&
         scan.error == WalReadError::InvalidRecordHeaderCrc;
}

[[nodiscard]] bool incomplete_payload_and_padding_are_classified() {
  const auto payload_path = test_path("fexma_wal_reader_incomplete_payload.wal");
  if (!create_wal(payload_path, 1)) {
    return false;
  }
  std::filesystem::resize_file(
      payload_path, records_offset(config) + physical_record_header_size + 3u);
  const ScanResult payload_scan = scan_wal(payload_path, config);
  std::filesystem::remove(payload_path);
  if (payload_scan.status != ScanStatus::IncompleteTail ||
      payload_scan.error != WalReadError::IncompletePayload) {
    return false;
  }

  const auto padding_path = test_path("fexma_wal_reader_incomplete_padding.wal");
  if (!create_wal(padding_path, 1)) {
    return false;
  }
  std::filesystem::resize_file(
      padding_path, records_offset(config) + physical_record_header_size +
                        config.payload_size + 3u);
  const ScanResult padding_scan = scan_wal(padding_path, config);
  std::filesystem::remove(padding_path);
  return padding_scan.status == ScanStatus::IncompleteTail &&
         padding_scan.error == WalReadError::IncompletePadding;
}

[[nodiscard]] bool sequence_gap_is_rejected() {
  const auto path = test_path("fexma_wal_reader_sequence_gap.wal");
  if (!create_wal(path, 1)) {
    return false;
  }

  RecordHeader header{};
  header.sequence = config.first_sequence + 1u;
  const auto original_payload = payload(1);
  header.payload_crc32 = crc32_bytes(original_payload.data(),
                                     original_payload.size());
  header.header_crc32 = record_header_crc32(header);
  const auto bytes = serialize_record_header(header);
  overwrite(path, records_offset(config), bytes);

  const ScanResult scan = scan_wal(path, config);
  std::filesystem::remove(path);
  return scan.status == ScanStatus::Corrupted &&
         scan.error == WalReadError::UnexpectedSequence;
}

[[nodiscard]] bool non_zero_padding_is_rejected() {
  const auto path = test_path("fexma_wal_reader_padding.wal");
  if (!create_wal(path, 1)) {
    return false;
  }
  const std::array corrupt{std::byte{1}};
  overwrite(path, records_offset(config) + physical_record_header_size +
                      config.payload_size,
            corrupt);
  const ScanResult scan = scan_wal(path, config);
  std::filesystem::remove(path);
  return scan.status == ScanStatus::Corrupted &&
         scan.error == WalReadError::NonZeroPadding;
}

[[nodiscard]] bool empty_wal_is_clean_end_of_log() {
  const auto path = test_path("fexma_wal_reader_empty.wal");
  if (!create_wal(path, 0)) {
    return false;
  }

  WalReader reader;
  std::array<std::byte, config.payload_size> output{};
  const bool reader_valid = reader.open(path, config).ok() &&
                            reader.read_next(output).status ==
                                ReadStatus::EndOfLog &&
                            reader.close();
  const ScanResult scan = scan_wal(path, config);
  std::filesystem::remove(path);
  return reader_valid && scan.status == ScanStatus::Clean &&
         scan.records == 0 && scan.last_sequence == 0 &&
         scan.last_valid_offset == records_offset(config);
}

[[nodiscard]] bool maximum_last_sequence_is_a_valid_record() {
  const auto path = test_path("fexma_wal_reader_max_sequence.wal");
  WalConfig terminal = config;
  terminal.first_sequence = std::numeric_limits<std::uint64_t>::max();
  if (!create_wal(path, 1, terminal)) {
    return false;
  }

  WalReader reader;
  std::array<std::byte, config.payload_size> output{};
  const ReaderOpenResult opened = reader.open(path, terminal);
  const ReadResult first = reader.read_next(output);
  const ReadResult end = reader.read_next(output);
  const bool reader_valid = opened.ok() && first.status == ReadStatus::Record &&
                            first.sequence == terminal.first_sequence &&
                            end.status == ReadStatus::EndOfLog &&
                            reader.close();
  const ScanResult scan = scan_wal(path, terminal);
  std::filesystem::remove(path);
  return reader_valid && scan.status == ScanStatus::Clean &&
         scan.records == 1 &&
         scan.last_sequence == std::numeric_limits<std::uint64_t>::max() &&
         scan.last_valid_offset ==
             records_offset(terminal) + aligned_record_size(terminal);
}

} // namespace

int main() {
  if (!validated_read_and_clean_scan()) return 1;
  if (!identity_mismatch_is_rejected()) return 2;
  if (!header_corruption_is_rejected()) return 3;
  if (!incomplete_tail_is_classified()) return 4;
  if (!payload_corruption_is_rejected()) return 5;
  if (!record_header_corruption_is_rejected()) return 6;
  if (!incomplete_payload_and_padding_are_classified()) return 7;
  if (!sequence_gap_is_rejected()) return 8;
  if (!non_zero_padding_is_rejected()) return 9;
  if (!empty_wal_is_clean_end_of_log()) return 10;
  if (!maximum_last_sequence_is_a_valid_record()) return 11;
  return 0;
}
