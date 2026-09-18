// Fairness Governor - framed evidence transport protocol.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Fixed 80-byte frame header, little-endian:
//
//   0  magic            4   "FGLK"
//   4  format_version   2
//   6  kind             2
//   8  flags            2
//   10 reserved         2   must be zero
//   12 payload_length   4
//   16 session_id       8
//   24 epoch            8
//   32 publisher_id     8
//   40 publisher_boot   8
//   48 publisher_ordinal 8
//   56 sequence         8
//   64 frame_checksum   8   over header[0,64) with this field zeroed, then payload
//   72 reserved         8   must be zero
//   80 payload
//
// A frame is rejected before its payload is trusted: wrong magic, wrong version,
// non-zero reserved fields, a payload larger than the negotiated bound, a bad
// checksum, or trailing bytes. The reader never allocates on a hostile length.
#ifndef FAIRNESS_GOVERNOR_LINK_FRAME_HPP
#define FAIRNESS_GOVERNOR_LINK_FRAME_HPP

#include <cstdint>
#include <string>

#include "fairness_governor/core/bytes.hpp"
#include "fairness_governor/core/ids.hpp"
#include "fairness_governor/core/status.hpp"
#include "fairness_governor/model/evidence.hpp"
#include "fairness_governor/model/policy.hpp"

namespace fairness_governor {

enum class FrameKind : std::uint16_t {
  Hello = 1,
  HelloAck = 2,
  Evidence = 3,
  EvidenceAck = 4,
  Goodbye = 5,
  Error = 6,
};

[[nodiscard]] constexpr const char* frame_kind_name(FrameKind kind) noexcept {
  switch (kind) {
    case FrameKind::Hello: return "HELLO";
    case FrameKind::HelloAck: return "HELLO_ACK";
    case FrameKind::Evidence: return "EVIDENCE";
    case FrameKind::EvidenceAck: return "EVIDENCE_ACK";
    case FrameKind::Goodbye: return "GOODBYE";
    case FrameKind::Error: return "ERROR";
  }
  return "UNKNOWN";
}

/// Frame flag bits.
enum FrameFlags : std::uint16_t {
  kFrameFlagNone = 0,
  /// The sender requests that the server stop after acknowledging this frame.
  kFrameFlagRequestShutdown = 1u << 0,
  /// The payload is a compressed/alternate encoding; always rejected in v1.
  kFrameFlagUnsupportedEncoding = 1u << 1,
};

inline constexpr std::uint32_t kFrameMagic = 0x4B4C4746u;  // 'F','G','L','K'
inline constexpr std::uint64_t kFrameHeaderSize = 80;
inline constexpr std::uint16_t kFrameProtocolVersion = 1;

struct FrameHeader {
  FrameKind kind{FrameKind::Hello};
  std::uint16_t flags{0};
  std::uint32_t payload_length{0};
  std::uint64_t session_id{0};
  FabricEpoch epoch{};
  PublisherId publisher{};
  BootId publisher_boot{};
  std::uint64_t publisher_ordinal{0};
  SequenceNumber sequence{0};
  std::uint64_t frame_checksum{0};

  [[nodiscard]] Incarnation publisher_incarnation() const noexcept {
    return Incarnation{publisher_boot, publisher_ordinal};
  }
};

/// Encodes a header and payload into one contiguous buffer.
[[nodiscard]] ByteBuffer encode_frame(const FrameHeader& header, const ByteBuffer& payload);

/// Decodes a header from exactly kFrameHeaderSize bytes.
[[nodiscard]] Status decode_frame_header(const std::uint8_t* data, std::size_t size,
                                         FrameHeader& out, std::uint32_t max_payload);

/// Verifies payload length and checksum for an already decoded header.
[[nodiscard]] Status verify_frame_payload(const FrameHeader& header, const std::uint8_t* payload,
                                          std::size_t size);

/// Handshake acknowledgement payload.
struct HelloAck {
  StatusCode code{StatusCode::Ok};
  std::uint64_t session_id{0};
  FabricEpoch epoch{};
  std::uint32_t max_payload{kMaxFramePayloadBytes};
  BootId server_boot{};
  std::uint64_t server_ordinal{0};
  FairnessPolicyId policy_id{};
  FairnessPolicyGeneration policy_generation{};
  std::uint64_t accounting_generation{0};
  std::string detail;

  [[nodiscard]] bool accepted() const noexcept { return code == StatusCode::Ok; }
};

/// Evidence acknowledgement payload.
struct EvidenceAck {
  StatusCode code{StatusCode::Ok};
  EvidenceSnapshotId snapshot{};
  EvidenceSnapshotGeneration generation{};
  std::uint64_t accounting_generation{0};
  std::uint8_t disposition{0};  ///< 0 = staged, 1 = duplicate ignored
  std::string detail;

  [[nodiscard]] bool accepted() const noexcept {
    return code == StatusCode::Ok || code == StatusCode::Duplicate;
  }
};

[[nodiscard]] ByteBuffer encode_hello_ack(const HelloAck& ack);
[[nodiscard]] Status decode_hello_ack(const ByteBuffer& bytes, HelloAck& out);

[[nodiscard]] ByteBuffer encode_evidence_ack(const EvidenceAck& ack);
[[nodiscard]] Status decode_evidence_ack(const ByteBuffer& bytes, EvidenceAck& out);

/// Maximum encoded size of a detail string carried in an error frame.
inline constexpr std::uint32_t kMaxFrameDetailBytes = 512;

}  // namespace fairness_governor

#endif  // FAIRNESS_GOVERNOR_LINK_FRAME_HPP
