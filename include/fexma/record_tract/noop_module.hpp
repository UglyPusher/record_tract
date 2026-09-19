#pragma once

/**
 * @file noop_module.hpp
 * @brief Trivial successful module for proving Core slider composition.
 */

#include <fexma/record_tract/record_tape_types.hpp>

namespace fexma::wal {

class NoOpModule final {
public:
  [[nodiscard]] bool process(const RecordView&) noexcept { return true; }
};

} // namespace fexma::wal
