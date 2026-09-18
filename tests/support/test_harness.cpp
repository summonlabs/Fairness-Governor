// Fairness Governor - test harness implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "test_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "fairness_governor/core/limits.hpp"
#include "fairness_governor/core/status.hpp"
#include "fairness_governor/model/outcome.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fgtest {

namespace {

thread_local std::vector<std::string> g_failures;

struct TestFailure {
  explicit TestFailure(std::string message) : what(std::move(message)) {}
  std::string what;
};

}  // namespace

void fail(const char* file, int line, const std::string& message) {
  const std::string location = std::string(file) + ":" + std::to_string(line);
  g_failures.push_back(location + ": " + message);
  throw TestFailure(message);
}

Registry& Registry::instance() {
  static Registry registry;
  return registry;
}

void Registry::add(const char* suite, const char* name, TestFunction function) {
  cases_.push_back(TestCase{suite, name, function});
}

std::string describe(std::uint64_t value) { return std::to_string(value); }
std::string describe(std::int64_t value) { return std::to_string(value); }
std::string describe(std::uint32_t value) { return std::to_string(value); }
std::string describe(std::int32_t value) { return std::to_string(value); }
std::string describe(bool value) { return value ? "true" : "false"; }
std::string describe(const std::string& value) { return value; }
std::string describe(const char* value) { return value == nullptr ? "(null)" : value; }
std::string describe(fairness_governor::StatusCode code) {
  return std::string(fairness_governor::to_string(code));
}
std::string describe(fairness_governor::Outcome outcome) {
  return std::string(fairness_governor::to_string(outcome));
}
std::string describe(fairness_governor::ReasonCode code) {
  return std::string(fairness_governor::to_string(code));
}

std::string env_or(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return fallback;
  }
  return value;
}

std::string make_temp_directory(const std::string& tag) {
  const std::string base = env_or("FG_TEST_TMP_DIR", "fg-test-tmp");
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  std::error_code error;
  for (int attempt = 0; attempt < 32; ++attempt) {
    std::filesystem::path candidate =
        std::filesystem::path(base) / (tag + "-" + std::to_string(stamp) + "-" +
                                       std::to_string(attempt));
    if (std::filesystem::create_directories(candidate, error)) {
      return candidate.string();
    }
    error.clear();
  }
  return {};
}

void remove_directory(const std::string& path) {
  if (path.empty()) {
    return;
  }
  std::error_code error;
  std::filesystem::remove_all(path, error);
}

namespace {

std::string quote_argument(const std::string& argument) {
#ifdef _WIN32
  std::string out = "\"";
  for (const char character : argument) {
    if (character == '"') {
      out += "\\\"";
    } else {
      out += character;
    }
  }
  out += '"';
  return out;
#else
  std::string out = "'";
  for (const char character : argument) {
    if (character == '\'') {
      out += "'\\''";
    } else {
      out += character;
    }
  }
  out += "'";
  return out;
#endif
}

}  // namespace

std::string quote(const std::string& argument) { return quote_argument(argument); }

struct ChildProcess::Impl {
#ifdef _WIN32
  HANDLE process{nullptr};
  HANDLE read_pipe{nullptr};
  DWORD exit_code{static_cast<DWORD>(-1)};
  bool exited{false};
#else
  pid_t pid{-1};
  int read_fd{-1};
  int status{0};
  bool exited{false};
#endif
  std::string buffer;
};

ChildProcess::~ChildProcess() {
  if (impl_ != nullptr) {
    terminate();
    delete impl_;
  }
}

bool ChildProcess::start(const std::string& command_line, std::string& error) {
  if (impl_ != nullptr) {
    terminate();
    delete impl_;
  }
  impl_ = new Impl();
#ifdef _WIN32
  SECURITY_ATTRIBUTES attributes {};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &attributes, 0)) {
    error = "CreatePipe failed";
    return false;
  }
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW startup {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_end;
  startup.hStdError = write_end;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION info {};
  std::wstring wide(command_line.begin(), command_line.end());
  std::vector<wchar_t> mutable_command(wide.begin(), wide.end());
  mutable_command.push_back(L'\0');
  const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0,
                                      nullptr, nullptr, &startup, &info);
  CloseHandle(write_end);
  if (!created) {
    CloseHandle(read_end);
    error = "CreateProcess failed with error " + std::to_string(GetLastError());
    return false;
  }
  CloseHandle(info.hThread);
  impl_->process = info.hProcess;
  impl_->read_pipe = read_end;
  return true;
#else
  int fds[2] = {-1, -1};
  if (pipe(fds) != 0) {
    error = "pipe failed";
    return false;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    error = "fork failed";
    return false;
  }
  if (pid == 0) {
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[0]);
    close(fds[1]);
    execl("/bin/sh", "sh", "-c", command_line.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  close(fds[1]);
  const int flags = fcntl(fds[0], F_GETFL, 0);
  fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
  impl_->pid = pid;
  impl_->read_fd = fds[0];
  return true;
#endif
}

bool ChildProcess::running() const noexcept {
  if (impl_ == nullptr) {
    return false;
  }
  if (impl_->exited) {
    return false;
  }
#ifdef _WIN32
  if (impl_->process == nullptr) {
    return false;
  }
  const DWORD state = WaitForSingleObject(impl_->process, 0);
  if (state == WAIT_OBJECT_0) {
    GetExitCodeProcess(impl_->process, &impl_->exit_code);
    impl_->exited = true;
    return false;
  }
  return true;
#else
  if (impl_->pid <= 0) {
    return false;
  }
  int status = 0;
  const pid_t result = waitpid(impl_->pid, &status, WNOHANG);
  if (result == impl_->pid) {
    impl_->status = status;
    impl_->exited = true;
    return false;
  }
  return true;
#endif
}

std::string ChildProcess::drain_output() {
  if (impl_ == nullptr) {
    return {};
  }
#ifdef _WIN32
  if (impl_->read_pipe == nullptr) {
    return {};
  }
  DWORD available = 0;
  for (;;) {
    if (!PeekNamedPipe(impl_->read_pipe, nullptr, 0, nullptr, &available, nullptr)) {
      break;
    }
    if (available == 0) {
      break;
    }
    char chunk[4096];
    DWORD read = 0;
    const DWORD request = available < sizeof(chunk) ? available : static_cast<DWORD>(sizeof(chunk));
    if (!ReadFile(impl_->read_pipe, chunk, request, &read, nullptr) || read == 0) {
      break;
    }
    impl_->buffer.append(chunk, read);
  }
#else
  if (impl_->read_fd < 0) {
    return {};
  }
  char chunk[4096];
  for (;;) {
    const ssize_t read = ::read(impl_->read_fd, chunk, sizeof(chunk));
    if (read <= 0) {
      break;
    }
    impl_->buffer.append(chunk, static_cast<std::size_t>(read));
  }
#endif
  return impl_->buffer;
}

bool ChildProcess::wait_for(int millis) {
  if (impl_ == nullptr) {
    return true;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(millis);
  for (;;) {
    (void)drain_output();
    if (!running()) {
      (void)drain_output();
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void ChildProcess::terminate() {
  if (impl_ == nullptr || impl_->exited) {
    return;
  }
#ifdef _WIN32
  if (impl_->process != nullptr) {
    TerminateProcess(impl_->process, 3);
    WaitForSingleObject(impl_->process, 5000);
    GetExitCodeProcess(impl_->process, &impl_->exit_code);
    CloseHandle(impl_->process);
    impl_->process = nullptr;
  }
  if (impl_->read_pipe != nullptr) {
    CloseHandle(impl_->read_pipe);
    impl_->read_pipe = nullptr;
  }
#else
  if (impl_->pid > 0) {
    kill(impl_->pid, SIGKILL);
    int status = 0;
    waitpid(impl_->pid, &status, 0);
    impl_->status = status;
    impl_->pid = -1;
  }
  if (impl_->read_fd >= 0) {
    close(impl_->read_fd);
    impl_->read_fd = -1;
  }
#endif
  impl_->exited = true;
}

int ChildProcess::exit_code() const noexcept {
  if (impl_ == nullptr) {
    return -1;
  }
#ifdef _WIN32
  return static_cast<int>(impl_->exit_code);
#else
  if (WIFEXITED(impl_->status)) {
    return WEXITSTATUS(impl_->status);
  }
  return -1;
#endif
}

ChildResult run_child(const std::string& command_line, bool capture_output) {
  ChildResult result;
  if (!capture_output) {
#ifdef _WIN32
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info {};
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &startup, &info)) {
      result.error = "CreateProcess failed";
      return result;
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    result.exit_code = static_cast<int>(code);
    result.started = true;
    return result;
#else
    const int code = std::system(command_line.c_str());
    result.exit_code = code;
    result.started = true;
    return result;
#endif
  }
  ChildProcess process;
  if (!process.start(command_line, result.error)) {
    return result;
  }
  result.started = true;
  (void)process.wait_for(0);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
  while (process.running()) {
    (void)process.drain_output();
    if (std::chrono::steady_clock::now() >= deadline) {
      result.error = "child did not exit";
      process.terminate();
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  (void)process.drain_output();
  result.output = process.drain_output();
  result.exit_code = process.exit_code();
  return result;
}

int run_all(int argc, char** argv) {
  std::string filter;
  bool list_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--list") {
      list_only = true;
    } else if (argument.rfind("--filter=", 0) == 0) {
      filter = argument.substr(9);
    }
  }

  const std::vector<TestCase>& cases = Registry::instance().cases();
  if (list_only) {
    for (const TestCase& test : cases) {
      std::cout << test.suite << "." << test.name << "\n";
    }
    return 0;
  }

  std::size_t executed = 0;
  std::size_t failed = 0;
  std::vector<std::string> failures;
  for (const TestCase& test : cases) {
    const std::string full_name = std::string(test.suite) + "." + test.name;
    if (!filter.empty() && full_name.find(filter) == std::string::npos) {
      continue;
    }
    ++executed;
    g_failures.clear();
    try {
      test.function();
    } catch (const TestFailure&) {
      // Recorded by fail().
    } catch (const std::exception& error) {
      g_failures.push_back(full_name + ": unexpected exception: " + error.what());
    } catch (...) {
      g_failures.push_back(full_name + ": unexpected non-standard exception");
    }
    if (g_failures.empty()) {
      std::cout << "[PASS] " << full_name << "\n";
    } else {
      ++failed;
      std::cout << "[FAIL] " << full_name << "\n";
      for (const std::string& message : g_failures) {
        std::cout << "       " << message << "\n";
        failures.push_back(full_name + ": " + message);
      }
    }
    std::cout.flush();
  }

  std::cout << "\n" << (executed - failed) << "/" << executed << " tests passed\n";
  if (failed != 0) {
    std::cout << "failures:\n";
    for (const std::string& message : failures) {
      std::cout << "  " << message << "\n";
    }
    return 1;
  }
  return executed == 0 ? 2 : 0;
}

}  // namespace fgtest

int main(int argc, char** argv) { return ::fgtest::run_all(argc, argv); }
