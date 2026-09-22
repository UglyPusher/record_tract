/**
 * @file test_record_tape_set_tail_ref.cpp
 * @brief Portable fail-fast tests for RecordTape topology freezing.
 */

#include <fexma/record_tract/record_tape.hpp>

#include <cstdlib>
#include <cstring>
#include <exception>

using namespace fexma::record_tract;

namespace {

void terminate_successfully() noexcept { std::_Exit(0); }

[[nodiscard]] bool is_scenario(const char* actual, const char* expected) {
  return std::strcmp(actual, expected) == 0;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2 ||
      (!is_scenario(argv[1], "open") &&
       !is_scenario(argv[1], "closed"))) {
    return 2;
  }

  std::set_terminate(terminate_successfully);

  Frontier configured_terminal{};
  Frontier replacement_terminal{};
  RecordTape tape;
  tape.SetTailRef(configured_terminal);
  if (tape.open({16, 1, default_alignment}) != RecordTapeOpenStatus::Ok) {
    return 3;
  }

  if (is_scenario(argv[1], "closed")) tape.close();
  tape.SetTailRef(replacement_terminal);
  return 4;
}
