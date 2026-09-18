// Fairness Governor - bounded filesystem primitives.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/persist/fsutil.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#endif

namespace fairness_governor::fsutil {
namespace {

#ifdef _WIN32
[[nodiscard]] std::wstring widen(const std::string& utf8) {
  if (utf8.empty()) {
    return std::wstring();
  }
  const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
  if (length <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), length);
  return wide;
}

[[nodiscard]] Status last_error_status(std::string_view what) {
  const DWORD code = GetLastError();
  return Status::failure(StatusCode::IoError,
                         std::string(what) + " failed with win32 error " + to_decimal(code));
}
#endif

}  // namespace

std::uint64_t process_token() noexcept {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t count = counter.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
  const std::uint64_t pid = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
  const std::uint64_t pid = static_cast<std::uint64_t>(getpid());
#endif
  return (pid << 16) ^ (count * 0x9E3779B97F4A7C15ULL);
}

bool exists(const std::string& path) {
#ifdef _WIN32
  const DWORD attributes = GetFileAttributesW(widen(path).c_str());
  return attributes != INVALID_FILE_ATTRIBUTES;
#else
  struct stat info {};
  return ::stat(path.c_str(), &info) == 0;
#endif
}

bool is_directory(const std::string& path) {
#ifdef _WIN32
  const DWORD attributes = GetFileAttributesW(widen(path).c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0) {
    return false;
  }
  return S_ISDIR(info.st_mode);
#endif
}

Status ensure_directory(const std::string& path) {
  if (path.empty()) {
    return Status::success();
  }
  if (is_directory(path)) {
    return Status::success();
  }
  std::string partial;
  partial.reserve(path.size());
  std::size_t index = 0;
  if (path.size() >= 2 && path[1] == ':') {
    partial.append(path, 0, 2);
    index = 2;
  }
  while (index <= path.size()) {
    const std::size_t next = path.find_first_of("\\/", index);
    const std::size_t end = next == std::string::npos ? path.size() : next;
    partial.append(path, index, end - index);
    if (!partial.empty() && !is_directory(partial)) {
#ifdef _WIN32
      if (!CreateDirectoryW(widen(partial).c_str(), nullptr)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
          return last_error_status("CreateDirectory");
        }
      }
#else
      if (::mkdir(partial.c_str(), 0777) != 0 && errno != EEXIST) {
        return Status::failure(StatusCode::IoError, "mkdir failed");
      }
#endif
    }
    if (next == std::string::npos) {
      break;
    }
    partial.push_back(path[next]);
    index = next + 1;
  }
  return is_directory(path) ? Status::success()
                            : Status(StatusCode::IoError, "directory could not be created");
}

Status remove_file(const std::string& path) noexcept {
#ifdef _WIN32
  if (!DeleteFileW(widen(path).c_str())) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return Status::success();
    }
    return Status::failure(StatusCode::IoError,
                           "DeleteFile failed with win32 error " + to_decimal(code));
  }
  return Status::success();
#else
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    return Status(StatusCode::IoError, "unlink failed");
  }
  return Status::success();
#endif
}

Result<ByteBuffer> read_file(const std::string& path, std::uint64_t max_bytes) {
#ifdef _WIN32
  HANDLE handle = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return last_error_status("CreateFile(read)");
  }
  LARGE_INTEGER size {};
  if (!GetFileSizeEx(handle, &size)) {
    CloseHandle(handle);
    return last_error_status("GetFileSizeEx");
  }
  if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > max_bytes) {
    CloseHandle(handle);
    return Status(StatusCode::OversizedPayload, "file exceeds the permitted size");
  }
  ByteBuffer buffer(static_cast<std::size_t>(size.QuadPart));
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(buffer.size() - offset, static_cast<std::size_t>(1) << 20));
    DWORD read = 0;
    if (!ReadFile(handle, buffer.data() + offset, chunk, &read, nullptr)) {
      CloseHandle(handle);
      return last_error_status("ReadFile");
    }
    if (read == 0) {
      CloseHandle(handle);
      return Status(StatusCode::Truncated, "file shrank while being read");
    }
    offset += read;
  }
  CloseHandle(handle);
  return buffer;
#else
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return Status(StatusCode::IoError, "fopen failed");
  }
  std::fseek(file, 0, SEEK_END);
  const long length = std::ftell(file);
  if (length < 0 || static_cast<std::uint64_t>(length) > max_bytes) {
    std::fclose(file);
    return Status(StatusCode::OversizedPayload, "file exceeds the permitted size");
  }
  std::fseek(file, 0, SEEK_SET);
  ByteBuffer buffer(static_cast<std::size_t>(length));
  const std::size_t read = buffer.empty() ? 0 : std::fread(buffer.data(), 1, buffer.size(), file);
  std::fclose(file);
  if (read != buffer.size()) {
    return Status(StatusCode::Truncated, "short read");
  }
  return buffer;
#endif
}

Status write_file(const std::string& path, const ByteBuffer& data) {
#ifdef _WIN32
  HANDLE handle = CreateFileW(widen(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return last_error_status("CreateFile(write)");
  }
  std::size_t offset = 0;
  while (offset < data.size()) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(data.size() - offset, static_cast<std::size_t>(1) << 20));
    DWORD written = 0;
    if (!WriteFile(handle, data.data() + offset, chunk, &written, nullptr)) {
      CloseHandle(handle);
      return last_error_status("WriteFile");
    }
    offset += written;
  }
  if (!FlushFileBuffers(handle)) {
    CloseHandle(handle);
    return last_error_status("FlushFileBuffers");
  }
  CloseHandle(handle);
  return Status::success();
#else
  const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (descriptor < 0) {
    return Status(StatusCode::IoError, "open failed");
  }
  std::size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t written = ::write(descriptor, data.data() + offset, data.size() - offset);
    if (written <= 0) {
      ::close(descriptor);
      return Status(StatusCode::IoError, "write failed");
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(descriptor) != 0) {
    ::close(descriptor);
    return Status(StatusCode::IoError, "fsync failed");
  }
  ::close(descriptor);
  return Status::success();
#endif
}

Status replace_file(const std::string& from, const std::string& to) {
#ifdef _WIN32
  if (!MoveFileExW(widen(from).c_str(), widen(to).c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return last_error_status("MoveFileEx");
  }
  return Status::success();
#else
  if (::rename(from.c_str(), to.c_str()) != 0) {
    return Status(StatusCode::IoError, "rename failed");
  }
  return Status::success();
#endif
}

Status copy_file(const std::string& from, const std::string& to, std::uint64_t max_bytes) {
  Result<ByteBuffer> bytes = read_file(from, max_bytes);
  if (!bytes.ok()) {
    return bytes.status();
  }
  return write_file(to, bytes.value());
}

Status sync_directory(const std::string& path) noexcept {
#ifdef _WIN32
  (void)path;
  return Status::success();  // MOVEFILE_WRITE_THROUGH already orders the metadata update.
#else
  std::string directory = path;
  const std::size_t slash = directory.find_last_of('/');
  if (slash != std::string::npos) {
    directory.resize(slash);
  }
  if (directory.empty()) {
    directory = ".";
  }
  const int descriptor = ::open(directory.c_str(), O_RDONLY);
  if (descriptor < 0) {
    return Status(StatusCode::IoError, "directory open failed");
  }
  const int result = ::fsync(descriptor);
  ::close(descriptor);
  if (result != 0) {
    return Status(StatusCode::IoError, "directory fsync failed");
  }
  return Status::success();
#endif
}

}  // namespace fairness_governor::fsutil
