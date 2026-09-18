// Fairness Governor - frame protocol tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/link/frame.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;
using namespace fgtest;

namespace {

FrameHeader make_header(FrameKind kind, std::uint32_t payload_length) {
  FrameHeader header;
  header.kind = kind;
  header.session_id = 0x1122334455667788ULL;
  header.epoch = FabricEpoch::from_value(4);
  header.publisher = PublisherId::from_value(9);
  header.publisher_boot = BootId::from_value(0xAA);
  header.publisher_ordinal = 2;
  header.sequence = 7;
  header.payload_length = payload_length;
  return header;
}

}  // namespace

FG_TEST(frame, round_trips) {
  const ByteBuffer payload{1, 2, 3, 4, 5};
  const ByteBuffer frame = encode_frame(make_header(FrameKind::Evidence, 5), payload);
  FG_CHECK_EQ(frame.size(), kFrameHeaderSize + 5u);
  FrameHeader header;
  FG_CHECK_OK(decode_frame_header(frame.data(), kFrameHeaderSize, header, kMaxFramePayloadBytes));
  FG_CHECK_EQ(header.kind, FrameKind::Evidence);
  FG_CHECK_EQ(header.session_id, 0x1122334455667788ULL);
  FG_CHECK_EQ(header.epoch.value(), 4u);
  FG_CHECK_EQ(header.publisher.value(), 9u);
  FG_CHECK_EQ(header.publisher_ordinal, 2u);
  FG_CHECK_EQ(header.sequence, 7u);
  FG_CHECK_EQ(header.payload_length, 5u);
  FG_CHECK_OK(verify_frame_payload(header, frame.data() + kFrameHeaderSize, payload.size()));
}

FG_TEST(frame, header_size_is_exactly_its_declared_constant) {
  FG_CHECK_EQ(kFrameHeaderSize, 80u);
  FG_CHECK_EQ(kFrameProtocolVersion, 1u);
  FG_CHECK_EQ(kFrameMagic, 0x4B4C4746u);
}

FG_TEST(frame, structural_damage_is_rejected) {
  const ByteBuffer payload{9, 9, 9};
  ByteBuffer frame = encode_frame(make_header(FrameKind::Hello, 3), payload);
  FrameHeader header;

  ByteBuffer bad_magic = frame;
  bad_magic[0] = 0;
  FG_CHECK_EQ(decode_frame_header(bad_magic.data(), kFrameHeaderSize, header,
                                  kMaxFramePayloadBytes).code(),
              StatusCode::FrameInvalid);

  ByteBuffer bad_version = frame;
  bad_version[4] = 7;
  FG_CHECK_EQ(decode_frame_header(bad_version.data(), kFrameHeaderSize, header,
                                  kMaxFramePayloadBytes).code(),
              StatusCode::UnsupportedFormat);

  ByteBuffer bad_kind = frame;
  store_le<std::uint16_t>(bad_kind.data() + 6, 99);
  FG_CHECK_EQ(decode_frame_header(bad_kind.data(), kFrameHeaderSize, header,
                                  kMaxFramePayloadBytes).code(),
              StatusCode::FrameInvalid);

  ByteBuffer bad_flags = frame;
  store_le<std::uint16_t>(bad_flags.data() + 8, 0x8000);
  FG_CHECK_EQ(decode_frame_header(bad_flags.data(), kFrameHeaderSize, header,
                                  kMaxFramePayloadBytes).code(),
              StatusCode::FrameInvalid);

  ByteBuffer unsupported = frame;
  store_le<std::uint16_t>(unsupported.data() + 8, kFrameFlagUnsupportedEncoding);
  FG_CHECK_EQ(decode_frame_header(unsupported.data(), kFrameHeaderSize, header,
                                  kMaxFramePayloadBytes).code(),
              StatusCode::Unsupported);

  ByteBuffer bad_reserved = frame;
  bad_reserved[10] = 1;
  FG_CHECK_EQ(decode_frame_header(bad_reserved.data(), kFrameHeaderSize, header,
                                  kMaxFramePayloadBytes).code(),
              StatusCode::FrameInvalid);

  ByteBuffer bad_tail = frame;
  bad_tail[79] = 1;
  FG_CHECK_EQ(decode_frame_header(bad_tail.data(), kFrameHeaderSize, header,
                                  kMaxFramePayloadBytes).code(),
              StatusCode::FrameInvalid);

  ByteBuffer oversized = frame;
  store_le<std::uint32_t>(oversized.data() + 12, kMaxFramePayloadBytes + 1);
  FG_CHECK_EQ(decode_frame_header(oversized.data(), kFrameHeaderSize, header,
                                  kMaxFramePayloadBytes).code(),
              StatusCode::FrameTooLarge);

  FG_CHECK_EQ(decode_frame_header(frame.data(), kFrameHeaderSize - 1, header,
                                  kMaxFramePayloadBytes).code(),
              StatusCode::FrameInvalid);
}

FG_TEST(frame, checksum_covers_the_payload) {
  ByteBuffer frame = encode_frame(make_header(FrameKind::Evidence, 3), ByteBuffer{1, 2, 3});
  FrameHeader header;
  FG_CHECK_OK(decode_frame_header(frame.data(), kFrameHeaderSize, header, kMaxFramePayloadBytes));
  frame[kFrameHeaderSize] ^= 0xFF;
  FG_CHECK_EQ(verify_frame_payload(header, frame.data() + kFrameHeaderSize, 3).code(),
              StatusCode::ChecksumMismatch);
  FG_CHECK_EQ(verify_frame_payload(header, frame.data() + kFrameHeaderSize, 2).code(),
              StatusCode::Truncated);
}

FG_TEST(frame, hello_ack_round_trips) {
  HelloAck ack;
  ack.code = StatusCode::StaleEpoch;
  ack.session_id = 42;
  ack.epoch = FabricEpoch::from_value(3);
  ack.max_payload = 4096;
  ack.server_boot = BootId::from_value(0xFEED);
  ack.server_ordinal = 4;
  ack.policy_id = FairnessPolicyId::from_value(1);
  ack.policy_generation = FairnessPolicyGeneration::from_value(5);
  ack.accounting_generation = 9;
  ack.detail = "publisher epoch is behind";
  const ByteBuffer encoded = encode_hello_ack(ack);
  HelloAck decoded;
  FG_CHECK_OK(decode_hello_ack(encoded, decoded));
  FG_CHECK_EQ(decoded.code, StatusCode::StaleEpoch);
  FG_CHECK_EQ(decoded.session_id, 42u);
  FG_CHECK_EQ(decoded.epoch.value(), 3u);
  FG_CHECK_EQ(decoded.policy_generation.value(), 5u);
  FG_CHECK_EQ(decoded.accounting_generation, 9u);
  FG_CHECK_EQ(decoded.detail, std::string("publisher epoch is behind"));
  FG_CHECK(!decoded.accepted());
}

FG_TEST(frame, hello_ack_rejects_damage) {
  HelloAck ack;
  ack.code = StatusCode::Ok;
  ack.detail = "ok";
  ByteBuffer encoded = encode_hello_ack(ack);
  HelloAck decoded;
  encoded.resize(encoded.size() - 1);
  FG_CHECK_EQ(decode_hello_ack(encoded, decoded).code(), StatusCode::FrameInvalid);
  encoded = encode_hello_ack(ack);
  encoded.push_back(0);
  FG_CHECK_EQ(decode_hello_ack(encoded, decoded).code(), StatusCode::FrameInvalid);
}

FG_TEST(frame, evidence_ack_round_trips) {
  EvidenceAck ack;
  ack.code = StatusCode::Duplicate;
  ack.snapshot = EvidenceSnapshotId::from_value(11);
  ack.generation = EvidenceSnapshotGeneration::from_value(2);
  ack.accounting_generation = 3;
  ack.disposition = 1;
  ack.detail = "already ingested";
  EvidenceAck decoded;
  FG_CHECK_OK(decode_evidence_ack(encode_evidence_ack(ack), decoded));
  FG_CHECK_EQ(decoded.snapshot.value(), 11u);
  FG_CHECK_EQ(decoded.disposition, 1u);
  FG_CHECK(decoded.accepted());
}

FG_TEST(frame, unknown_status_codes_are_rejected) {
  ByteBuffer payload;
  ByteWriter writer(payload);
  writer.u16(60000);
  writer.u64(0);
  writer.u64(0);
  writer.u32(0);
  writer.u64(0);
  writer.u64(0);
  writer.u64(0);
  writer.u64(0);
  writer.u64(0);
  writer.blob(std::string());
  HelloAck decoded;
  FG_CHECK_EQ(decode_hello_ack(payload, decoded).code(), StatusCode::FrameInvalid);
}
