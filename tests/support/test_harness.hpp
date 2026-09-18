// Fairness Governor - minimal deterministic test harness.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Deliberately small and dependency-free. There are no timeouts anywhere in the
// harness: a test that hangs is a defect to diagnose, not something to mask.
#ifndef FG_TEST_HARNESS_HPP
#define FG_TEST_HARNESS_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace fairness_governor {
enum class StatusCode : std::uint16_t;
enum class Outcome : std::uint8_t;
enum class ReasonCode : std::uint16_t;
}  // namespace fairness_governor

namespace fgtest {

using TestFunction = void (*)();

// Value rendering used by FG_CHECK_EQ failure messages.
[[nodiscard]] std::string describe(std::uint64_t value);
[[nodiscard]] std::string describe(std::int64_t value);
[[nodiscard]] std::string describe(std::uint32_t value);
[[nodiscard]] std::string describe(std::int32_t value);
[[nodiscard]] std::string describe(bool value);
[[nodiscard]] std::string describe(const std::string& value);
[[nodiscard]] std::string describe(const char* value);
[[nodiscard]] std::string describe(fairness_governor::StatusCode code);
[[nodiscard]] std::string describe(fairness_governor::Outcome outcome);
[[nodiscard]] std::string describe(fairness_governor::ReasonCode code);

/// Renders any remaining comparable value. Strongly typed identities and
/// generations render through their underlying value; plain integrals render
/// directly; anything else renders as a placeholder rather than failing to
/// compile inside a check macro.
template <class T>
[[nodiscard]] std::string describe(const T& value) {
  if constexpr (requires { value.value(); }) {
    return describe(static_cast<std::uint64_t>(value.value()));
  } else if constexpr (std::is_enum_v<T>) {
    return describe(static_cast<std::int64_t>(value));
  } else if constexpr (std::is_convertible_v<T, std::uint64_t>) {
    return describe(static_cast<std::uint64_t>(value));
  } else if constexpr (std::is_convertible_v<T, std::int64_t>) {
    return describe(static_cast<std::int64_t>(value));
  } else {
    return "<value>";
  }
}

/// Quotes a command-line argument for the host shell.
[[nodiscard]] std::string quote(const std::string& argument);

struct TestCase {
  const char* suite;
  const char* name;
  TestFunction function;
};

class Registry {
 public:
  static Registry& instance();

  void add(const char* suite, const char* name, TestFunction function);
  [[nodiscard]] const std::vector<TestCase>& cases() const noexcept { return cases_; }

 private:
  std::vector<TestCase> cases_;
};

struct Registrar {
  Registrar(const char* suite, const char* name, TestFunction function) {
    Registry::instance().add(suite, name, function);
  }
};

/// Records the current test failure.
void fail(const char* file, int line, const std::string& message);

/// Runs every registered test and returns the process exit code.
int run_all(int argc, char** argv);

/// Deterministic 64-bit PRNG (splitmix64) used by every seeded randomized test.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  [[nodiscard]] std::uint64_t below(std::uint64_t bound) noexcept {
    return bound == 0 ? 0 : next() % bound;
  }

  [[nodiscard]] std::uint64_t in_range(std::uint64_t low, std::uint64_t high) noexcept {
    return low + below(high - low + 1);
  }

  [[nodiscard]] bool chance(std::uint32_t percent) noexcept { return below(100) < percent; }

 private:
  std::uint64_t state_;
};

/// Returns the configured environment value or the fallback.
[[nodiscard]] std::string env_or(const char* name, const std::string& fallback);

/// Creates a fresh temporary directory and returns its path.
[[nodiscard]] std::string make_temp_directory(const std::string& tag);

/// Removes a directory tree created by make_temp_directory.
void remove_directory(const std::string& path);

/// Runs a child process, captures stdout, and returns its exit code.
struct ChildResult {
  int exit_code{-1};
  std::string output;
  bool started{false};
  std::string error;
};

[[nodiscard]] ChildResult run_child(const std::string& command_line, bool capture_output);

/// Starts a child process without waiting for it.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  [[nodiscard]] bool start(const std::string& command_line, std::string& error);
  [[nodiscard]] bool running() const noexcept;
  /// Reads whatever the child has written so far, without blocking beyond the
  /// bounded poll the implementation uses.
  [[nodiscard]] std::string drain_output();
  /// Waits up to `millis` for exit; returns true when the child exited.
  [[nodiscard]] bool wait_for(int millis);
  void terminate();
  [[nodiscard]] int exit_code() const noexcept;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace fgtest

#define FG_TEST(suite, name)                                              \
  static void fg_test_##suite##_##name();                                 \
  static ::fgtest::Registrar fg_registrar_##suite##_##name(               \
      #suite, #name, &fg_test_##suite##_##name);                          \
  static void fg_test_##suite##_##name()

#define FG_CHECK(condition)                                                          \
  do {                                                                               \
    if (!(condition)) {                                                              \
      ::fgtest::fail(__FILE__, __LINE__, "check failed: " #condition);               \
    }                                                                                \
  } while (false)

#define FG_CHECK_MSG(condition, message)                                             \
  do {                                                                               \
    if (!(condition)) {                                                              \
      ::fgtest::fail(__FILE__, __LINE__,                                             \
                     std::string("check failed: " #condition " - ") + (message));    \
    }                                                                                \
  } while (false)

#define FG_CHECK_EQ(a, b)                                                            \
  do {                                                                               \
    const auto fg_lhs = (a);                                                         \
    const auto fg_rhs = (b);                                                         \
    if (!(fg_lhs == fg_rhs)) {                                                       \
      ::fgtest::fail(__FILE__, __LINE__,                                             \
                     std::string("expected equality: " #a " == " #b " (got ") +      \
                         ::fgtest::describe(fg_lhs) + " vs " + ::fgtest::describe(fg_rhs) + \
                         ")");                                                       \
    }                                                                                \
  } while (false)

#define FG_CHECK_OK(expr)                                                            \
  do {                                                                               \
    const auto fg_status = (expr);                                                   \
    if (!fg_status.ok()) {                                                           \
      ::fgtest::fail(__FILE__, __LINE__,                                             \
                     std::string("expected success: " #expr " -> ") +                \
                         std::string(fg_status.to_string()));                        \
    }                                                                                \
  } while (false)

#define FG_CHECK_CODE(expr, expected)                                                \
  do {                                                                               \
    const auto fg_status = (expr);                                                   \
    if (fg_status.code() != (expected)) {                                            \
      ::fgtest::fail(__FILE__, __LINE__,                                             \
                     std::string("unexpected status code for " #expr ": got ") +     \
                         std::string(::fairness_governor::to_string(fg_status.code())) + \
                         " want " +                                                  \
                         std::string(::fairness_governor::to_string(expected)));     \
    }                                                                                \
  } while (false)

#endif  // FG_TEST_HARNESS_HPP
