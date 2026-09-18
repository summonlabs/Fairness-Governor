// Fairness Governor - frame encoding and validation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/link/frame.hpp"

#include <algorithm>

#include "fairness_governor/core/checked.hpp"

namespace fairness_governor {
namespace {

constexpr std::size_t kOffsetMagic = 0;
constexpr std::size_t kOffsetVersion = 4;
constexpr std::size_t kOffsetKind = 6;
constexpr std::size_t kOffsetFlags = 8;
constexpr std::size_t kOffsetReserved = 10;
constexpr std::size_t kOffsetPayloadLength = 12;
constexpr std::size_t kOffsetSession = 16;
constexpr std::size_t kOffsetEpoch = 24;
constexpr std::size_t kOffsetPublisher = 32;
constexpr std::size_t kOffsetPublisherBoot = 40;
constexpr std::size_t kOffsetPublisherOrdinal = 48;
constexpr std::size_t kOffsetSequence = 56;
constexpr std::size_t kOffsetChecksum = 64;
constexpr std::size_t kOffsetReservedTail = 72;
/// The checksum covers the first 64 header bytes and then the payload. The
/// checksum field itself lives at offset 64, just past the covered region, so it
/// never contributes to its own value and needs no masking. The buffer below is
/// exactly the covered size: writing the checksum field into it would run eight
/// bytes past the end.
constexpr std::size_t kChecksumCoverage = 64;
static_assert(kOffsetChecksum == kChecksumCoverage,
              "the checksum field must begin exactly where the covered region ends");

constexpr std::uint16_t kKnownFlags =
    static_cast<std::uint16_t>(kFrameFlagRequestShutdown | kFrameFlagUnsupportedEncoding);

[[nodiscard]] bool known_kind(std::uint16_t raw) noexcept {
  return raw >= static_cast<std::uint16_t>(FrameKind::Hello) &&
         raw <= static_cast<std::uint16_t>(FrameKind::Error);
}

[[nodiscard]] std::uint64_t frame_checksum(const std::uint8_t* header, const std::uint8_t* payload,
                                           std::size_t payload_size) {
  std::uint8_t copy[kChecksumCoverage];
  std::copy(header, header + kChecksumCoverage, copy);
  std::uint64_t hash = fnv1a64(copy, sizeof(copy));
  hash = fnv1a64(payload, payload_size, hash);
  return hash;
}

void write_header(std::uint8_t* out, const FrameHeader& header) {
  std::fill(out, out + kFrameHeaderSize, static_cast<std::uint8_t>(0));
  store_le<std::uint32_t>(out + kOffsetMagic, kFrameMagic);
  store_le<std::uint16_t>(out + kOffsetVersion, kFrameProtocolVersion);
  store_le<std::uint16_t>(out + kOffsetKind, static_cast<std::uint16_t>(header.kind));
  store_le<std::uint16_t>(out + kOffsetFlags, header.flags);
  store_le<std::uint32_t>(out + kOffsetPayloadLength, header.payload_length);
  store_le<std::uint64_t>(out + kOffsetSession, header.session_id);
  store_le<std::uint64_t>(out + kOffsetEpoch, header.epoch.value());
  store_le<std::uint64_t>(out + kOffsetPublisher, header.publisher.value());
  store_le<std::uint64_t>(out + kOffsetPublisherBoot, header.publisher_boot.value());
  store_le<std::uint64_t>(out + kOffsetPublisherOrdinal, header.publisher_ordinal);
  store_le<std::uint64_t>(out + kOffsetSequence, header.sequence);
}

}  // namespace

ByteBuffer encode_frame(const FrameHeader& header, const ByteBuffer& payload) {
  ByteBuffer frame(kFrameHeaderSize + payload.size(), 0);
  FrameHeader effective = header;
  effective.payload_length = static_cast<std::uint32_t>(payload.size());
  write_header(frame.data(), effective);
  if (!payload.empty()) {
    std::copy(payload.begin(), payload.end(), frame.begin() + kFrameHeaderSize);
  }
  const std::uint64_t checksum =
      frame_checksum(frame.data(), frame.data() + kFrameHeaderSize, payload.size());
  store_le<std::uint64_t>(frame.data() + kOffsetChecksum, checksum);
  return frame;
}

Status decode_frame_header(const std::uint8_t* data, std::size_t size, FrameHeader& out,
                           std::uint32_t max_payload) {
  if (size != kFrameHeaderSize) {
    return Status(StatusCode::FrameInvalid, "frame header is not exactly its fixed size");
  }
  if (load_le<std::uint32_t>(data + kOffsetMagic) != kFrameMagic) {
    return Status(StatusCode::FrameInvalid, "frame magic mismatch");
  }
  if (load_le<std::uint16_t>(data + kOffsetVersion) != kFrameProtocolVersion) {
    return Status(StatusCode::UnsupportedFormat, "unsupported frame protocol version");
  }
  const std::uint16_t kind = load_le<std::uint16_t>(data + kOffsetKind);
  if (!known_kind(kind)) {
    return Status(StatusCode::FrameInvalid, "unknown frame kind");
  }
  const std::uint16_t flags = load_le<std::uint16_t>(data + kOffsetFlags);
  if ((flags & ~kKnownFlags) != 0) {
    return Status(StatusCode::FrameInvalid, "frame carries unknown flags");
  }
  if ((flags & kFrameFlagUnsupportedEncoding) != 0) {
    return Status(StatusCode::Unsupported, "frame requests an unsupported payload encoding");
  }
  if (load_le<std::uint16_t>(data + kOffsetReserved) != 0 ||
      load_le<std::uint64_t>(data + kOffsetReservedTail) != 0) {
    return Status(StatusCode::FrameInvalid, "frame reserved fields must be zero");
  }
  const std::uint32_t length = load_le<std::uint32_t>(data + kOffsetPayloadLength);
  if (length > max_payload) {
    return Status(StatusCode::FrameTooLarge, "frame payload exceeds the negotiated bound");
  }
  FrameHeader header;
  header.kind = static_cast<FrameKind>(kind);
  header.flags = flags;
  header.payload_length = length;
  header.session_id = load_le<std::uint64_t>(data + kOffsetSession);
  header.epoch = FabricEpoch::from_value(load_le<std::uint64_t>(data + kOffsetEpoch));
  header.publisher = PublisherId::from_value(load_le<std::uint64_t>(data + kOffsetPublisher));
  header.publisher_boot = BootId::from_value(load_le<std::uint64_t>(data + kOffsetPublisherBoot));
  header.publisher_ordinal = load_le<std::uint64_t>(data + kOffsetPublisherOrdinal);
  header.sequence = load_le<std::uint64_t>(data + kOffsetSequence);
  header.frame_checksum = load_le<std::uint64_t>(data + kOffsetChecksum);
  out = header;
  return Status::success();
}

Status verify_frame_payload(const FrameHeader& header, const std::uint8_t* payload,
                            std::size_t size) {
  if (size != header.payload_length) {
    return Status(StatusCode::Truncated, "frame payload length mismatch");
  }
  std::uint8_t header_copy[kChecksumCoverage];
  store_le<std::uint32_t>(header_copy + kOffsetMagic, kFrameMagic);
  store_le<std::uint16_t>(header_copy + kOffsetVersion, kFrameProtocolVersion);
  store_le<std::uint16_t>(header_copy + kOffsetKind, static_cast<std::uint16_t>(header.kind));
  store_le<std::uint16_t>(header_copy + kOffsetFlags, header.flags);
  store_le<std::uint16_t>(header_copy + kOffsetReserved, 0);
  store_le<std::uint32_t>(header_copy + kOffsetPayloadLength, header.payload_length);
  store_le<std::uint64_t>(header_copy + kOffsetSession, header.session_id);
  store_le<std::uint64_t>(header_copy + kOffsetEpoch, header.epoch.value());
  store_le<std::uint64_t>(header_copy + kOffsetPublisher, header.publisher.value());
  store_le<std::uint64_t>(header_copy + kOffsetPublisherBoot, header.publisher_boot.value());
  store_le<std::uint64_t>(header_copy + kOffsetPublisherOrdinal, header.publisher_ordinal);
  store_le<std::uint64_t>(header_copy + kOffsetSequence, header.sequence);
  if (frame_checksum(header_copy, payload, size) != header.frame_checksum) {
    return Status(StatusCode::ChecksumMismatch, "frame checksum mismatch");
  }
  return Status::success();
}

// --- Control payloads --------------------------------------------------------

ByteBuffer encode_hello_ack(const HelloAck& ack) {
  ByteBuffer buffer;
  ByteWriter writer(buffer);
  writer.u16(static_cast<std::uint16_t>(ack.code));
  writer.u64(ack.session_id);
  writer.u64(ack.epoch.value());
  writer.u32(ack.max_payload);
  writer.u64(ack.server_boot.value());
  writer.u64(ack.server_ordinal);
  writer.u64(ack.policy_id.value());
  writer.u64(ack.policy_generation.value());
  writer.u64(ack.accounting_generation);
  writer.blob(ack.detail);
  return buffer;
}

Status decode_hello_ack(const ByteBuffer& bytes, HelloAck& out) {
  ByteReader reader(bytes.data(), bytes.size());
  HelloAck ack;
  std::uint16_t code = 0;
  std::uint64_t raw = 0;
  bool ok = reader.u16(code);
  ok = ok && reader.u64(ack.session_id);
  ok = ok && reader.u64(raw);
  ack.epoch = FabricEpoch::from_value(raw);
  ok = ok && reader.u32(ack.max_payload);
  ok = ok && reader.u64(raw);
  ack.server_boot = BootId::from_value(raw);
  ok = ok && reader.u64(ack.server_ordinal);
  ok = ok && reader.u64(raw);
  ack.policy_id = FairnessPolicyId::from_value(raw);
  ok = ok && reader.u64(raw);
  ack.policy_generation = FairnessPolicyGeneration::from_value(raw);
  ok = ok && reader.u64(ack.accounting_generation);
  ok = ok && reader.blob(ack.detail, kMaxFrameDetailBytes);
  if (!ok || !reader.empty()) {
    return Status(StatusCode::FrameInvalid, "hello acknowledgement is malformed");
  }
  if (code > static_cast<std::uint16_t>(StatusCode::Busy)) {
    return Status(StatusCode::FrameInvalid, "hello acknowledgement carries an unknown status");
  }
  ack.code = static_cast<StatusCode>(code);
  out = std::move(ack);
  return Status::success();
}

ByteBuffer encode_evidence_ack(const EvidenceAck& ack) {
  ByteBuffer buffer;
  ByteWriter writer(buffer);
  writer.u16(static_cast<std::uint16_t>(ack.code));
  writer.u64(ack.snapshot.value());
  writer.u64(ack.generation.value());
  writer.u64(ack.accounting_generation);
  writer.u8(ack.disposition);
  writer.blob(ack.detail);
  return buffer;
}

Status decode_evidence_ack(const ByteBuffer& bytes, EvidenceAck& out) {
  ByteReader reader(bytes.data(), bytes.size());
  EvidenceAck ack;
  std::uint16_t code = 0;
  std::uint64_t raw = 0;
  bool ok = reader.u16(code);
  ok = ok && reader.u64(raw);
  ack.snapshot = EvidenceSnapshotId::from_value(raw);
  ok = ok && reader.u64(raw);
  ack.generation = EvidenceSnapshotGeneration::from_value(raw);
  ok = ok && reader.u64(ack.accounting_generation) && reader.u8(ack.disposition);
  ok = ok && reader.blob(ack.detail, kMaxFrameDetailBytes);
  if (!ok || !reader.empty()) {
    return Status(StatusCode::FrameInvalid, "evidence acknowledgement is malformed");
  }
  if (code > static_cast<std::uint16_t>(StatusCode::Busy)) {
    return Status(StatusCode::FrameInvalid, "evidence acknowledgement carries an unknown status");
  }
  ack.code = static_cast<StatusCode>(code);
  out = std::move(ack);
  return Status::success();
}

}  // namespace fairness_governor
