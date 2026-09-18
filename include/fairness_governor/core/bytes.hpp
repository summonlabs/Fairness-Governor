// Fairness Governor - deterministic little-endian encoding primitives.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// All durable records and all wire frames are encoded with these helpers. The
// encoding is fixed-width, little-endian, and length-prefixed, so a reader can
// always detect truncation, contradiction, or trailing garbage rather than
// silently accepting a short read.
#ifndef FAIRNESS_GOVERNOR_CORE_BYTES_HPP
#define FAIRNESS_GOVERNOR_CORE_BYTES_HPP

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "fairness_governor/core/checked.hpp"
#include "fairness_governor/core/ids.hpp"
#include "fairness_governor/core/status.hpp"

namespace fairness_governor {

using ByteBuffer = std::vector<std::uint8_t>;

/// Append-only little-endian writer.
class ByteWriter {
 public:
  explicit ByteWriter(ByteBuffer& buffer) noexcept : buffer_(&buffer) {}

  void u8(std::uint8_t value) { buffer_->push_back(value); }

  void u16(std::uint16_t value) {
    buffer_->push_back(static_cast<std::uint8_t>(value & 0xFF));
    buffer_->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
  }

  void u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      buffer_->push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
  }

  void u64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      buffer_->push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
  }

  void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

  void bytes(const std::uint8_t* data, std::size_t size) {
    buffer_->insert(buffer_->end(), data, data + size);
  }

  void blob(const std::string& value) {
    u32(static_cast<std::uint32_t>(value.size()));
    bytes(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
  }

  [[nodiscard]] std::size_t size() const noexcept { return buffer_->size(); }

 private:
  ByteBuffer* buffer_;
};

/// Bounds-checked little-endian reader. Every read validates that enough bytes
/// remain; a reader can never run off the end of a hostile buffer.
class ByteReader {
 public:
  ByteReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - offset_; }
  [[nodiscard]] bool empty() const noexcept { return remaining() == 0; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

  [[nodiscard]] bool u8(std::uint8_t& out) {
    if (remaining() < 1) {
      return false;
    }
    out = data_[offset_++];
    return true;
  }

  [[nodiscard]] bool u16(std::uint16_t& out) {
    if (remaining() < 2) {
      return false;
    }
    out = static_cast<std::uint16_t>(data_[offset_]) |
          static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[offset_ + 1]) << 8);
    offset_ += 2;
    return true;
  }

  [[nodiscard]] bool u32(std::uint32_t& out) {
    if (remaining() < 4) {
      return false;
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      value |= static_cast<std::uint32_t>(data_[offset_ + static_cast<std::size_t>(i)]) << (8 * i);
    }
    offset_ += 4;
    out = value;
    return true;
  }

  [[nodiscard]] bool u64(std::uint64_t& out) {
    if (remaining() < 8) {
      return false;
    }
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value |= static_cast<std::uint64_t>(data_[offset_ + static_cast<std::size_t>(i)]) << (8 * i);
    }
    offset_ += 8;
    out = value;
    return true;
  }

  [[nodiscard]] bool i64(std::int64_t& out) {
    std::uint64_t raw = 0;
    if (!u64(raw)) {
      return false;
    }
    out = static_cast<std::int64_t>(raw);
    return true;
  }

  [[nodiscard]] bool raw(std::uint8_t* out, std::size_t count) {
    if (remaining() < count) {
      return false;
    }
    std::memcpy(out, data_ + offset_, count);
    offset_ += count;
    return true;
  }

  /// Reads a length-prefixed blob, rejecting any length above `max_size`.
  [[nodiscard]] bool blob(std::string& out, std::uint32_t max_size) {
    std::uint32_t length = 0;
    if (!u32(length)) {
      return false;
    }
    if (length > max_size) {
      return false;
    }
    if (remaining() < length) {
      return false;
    }
    out.assign(reinterpret_cast<const char*>(data_ + offset_), length);
    offset_ += length;
    return true;
  }

  [[nodiscard]] bool skip(std::size_t count) {
    if (remaining() < count) {
      return false;
    }
    offset_ += count;
    return true;
  }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t offset_{0};
};

/// Little-endian store of an unsigned value into a fixed-size buffer.
template <class T>
void store_le(std::uint8_t* out, T value) noexcept {
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    out[i] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(value) >> (8 * i)) & 0xFF);
  }
}

/// Little-endian load of an unsigned value from a fixed-size buffer.
template <class T>
[[nodiscard]] T load_le(const std::uint8_t* in) noexcept {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<std::uint64_t>(in[i]) << (8 * i);
  }
  return static_cast<T>(value);
}

[[nodiscard]] inline std::uint64_t checksum64(const std::uint8_t* data, std::size_t size,
                                              std::uint64_t seed = kFnvOffsetBasis) noexcept {
  return fnv1a64(data, size, seed);
}

}  // namespace fairness_governor

#endif  // FAIRNESS_GOVERNOR_CORE_BYTES_HPP
