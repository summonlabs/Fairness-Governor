// Fairness Governor - minimal bounded filesystem primitives.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef FAIRNESS_GOVERNOR_PERSIST_FSUTIL_HPP
#define FAIRNESS_GOVERNOR_PERSIST_FSUTIL_HPP

#include <cstdint>
#include <string>

#include "fairness_governor/core/bytes.hpp"
#include "fairness_governor/core/status.hpp"

namespace fairness_governor::fsutil {

[[nodiscard]] bool exists(const std::string& path);
[[nodiscard]] bool is_directory(const std::string& path);
[[nodiscard]] Status ensure_directory(const std::string& path);

/// Best-effort cleanup. Not [[nodiscard]] on purpose: every caller is removing a
/// temporary or backup artifact whose absence is the desired end state, and a
/// failure to remove one is reported through the recovery notes of the next open
/// rather than discarded silently.
Status remove_file(const std::string& path) noexcept;

/// Reads at most `max_bytes`; a larger file is rejected rather than truncated.
[[nodiscard]] Result<ByteBuffer> read_file(const std::string& path, std::uint64_t max_bytes);

/// Creates or truncates `path` and flushes its contents to stable storage.
[[nodiscard]] Status write_file(const std::string& path, const ByteBuffer& data);

/// Replaces `to` with `from` atomically where the platform allows it, then
/// flushes the containing directory.
[[nodiscard]] Status replace_file(const std::string& from, const std::string& to);

/// Copies `from` to `to`, flushing the result.
[[nodiscard]] Status copy_file(const std::string& from, const std::string& to,
                               std::uint64_t max_bytes);

/// Flushes the directory entry changes for `path`'s parent. Best-effort for
/// the same reason as remove_file: the durability guarantee comes from the
/// atomic replace plus FlushFileBuffers/fsync of the record itself.
Status sync_directory(const std::string& path) noexcept;

/// Returns a process-unique token used to name temporary files.
[[nodiscard]] std::uint64_t process_token() noexcept;

}  // namespace fairness_governor::fsutil

#endif  // FAIRNESS_GOVERNOR_PERSIST_FSUTIL_HPP
