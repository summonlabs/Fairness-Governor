// Fairness Governor - adversarial framed-transport handling (in-process link).
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "fairness_governor/link/link.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;

namespace {

struct LinkFixture {
  std::unique_ptr<FairnessGovernor> governor;
  std::unique_ptr<EvidenceLinkServer> server;
  std::thread serving;

  ~LinkFixture() {
    if (server) {
      server->request_stop();
    }
    if (serving.joinable()) {
      serving.join();
    }
  }

  void stop() {
    if (server) {
      server->request_stop();
    }
    if (serving.joinable()) {
      serving.join();
    }
  }
};

Result<std::unique_ptr<LinkFixture>> start_link(const FairnessPolicy& policy,
                                               const LinkServerOptions& requested = {}) {
  auto fixture = std::make_unique<LinkFixture>();
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  if (!opened.ok()) {
    return opened.status();
  }
  fixture->governor = opened.take();
  LinkServerOptions options = requested;
  options.port = 0;
  if (options.max_connections == 0) {
    options.max_connections = 4;
  }
  Result<std::unique_ptr<EvidenceLinkServer>> started =
      EvidenceLinkServer::start(*fixture->governor, options);
  if (!started.ok()) {
    return started.status();
  }
  fixture->server = started.take();
  fixture->serving = std::thread([raw = fixture->server.get()]() { raw->serve(); });
  return fixture;
}

FrameHeader header_for(FrameKind kind, FabricEpoch epoch, std::uint64_t publisher,
                       std::uint64_t sequence) {
  FrameHeader header;
  header.kind = kind;
  header.epoch = epoch;
  header.publisher = PublisherId::from_value(publisher);
  header.publisher_boot = BootId::from_value(0xB007);
  header.publisher_ordinal = 1;
  header.sequence = sequence;
  return header;
}

}  // namespace

FG_TEST(adversarial_transport, garbage_bytes_are_rejected_and_the_server_survives) {
  const FairnessPolicy policy = flat_policy(2);
  Result<std::unique_ptr<LinkFixture>> started = start_link(policy);
  FG_CHECK_OK(started.status());
  std::unique_ptr<LinkFixture>& fixture = started.value();

  Rng rng(0x7A9BULL);
  for (int iteration = 0; iteration < 128; ++iteration) {
    ByteBuffer garbage(1 + rng.below(kFrameHeaderSize + 64));
    for (std::uint8_t& byte : garbage) {
      byte = static_cast<std::uint8_t>(rng.below(256));
    }
    // Send and close without waiting for a reply: the point of this case is what
    // the server does with bytes it cannot frame, not what it answers.
    Result<std::unique_ptr<RawLinkSession>> raw =
        RawLinkSession::connect("127.0.0.1", fixture->server->port());
    if (!raw.ok()) {
      continue;
    }
    (void)raw.value()->send_raw(garbage);
    raw.value()->close();
  }
  // The server still accepts a well-formed publisher afterwards.
  LinkClientOptions options;
  options.port = fixture->server->port();
  options.epoch = policy.epoch;
  options.publisher = PublisherId::from_value(1);
  HelloAck ack;
  Result<std::unique_ptr<EvidenceLinkClient>> client =
      EvidenceLinkClient::connect(options, ack);
  FG_CHECK_OK(client.status());
  FG_CHECK(ack.accepted());
  client.value()->close();

  const LinkStats stats = fixture->server->stats();
  FG_CHECK(stats.frames_rejected > 0);
  FG_CHECK(stats.connections_accepted >= 1);
  fixture->stop();
  FG_CHECK_EQ(fixture->governor->staged_snapshot_count(), 0u);
  (void)fixture->governor->shutdown();
}

FG_TEST(adversarial_transport, a_stale_epoch_handshake_is_refused) {
  const FairnessPolicy policy = flat_policy(2);
  Result<std::unique_ptr<LinkFixture>> started = start_link(policy);
  FG_CHECK_OK(started.status());
  std::unique_ptr<LinkFixture>& fixture = started.value();

  LinkClientOptions options;
  options.port = fixture->server->port();
  options.epoch = FabricEpoch::from_value(policy.epoch.value() + 3);
  options.publisher = PublisherId::from_value(2);
  HelloAck ack;
  Result<std::unique_ptr<EvidenceLinkClient>> client =
      EvidenceLinkClient::connect(options, ack);
  FG_CHECK(!client.ok());
  FG_CHECK_EQ(client.code(), StatusCode::StaleEpoch);
  FG_CHECK_EQ(ack.code, StatusCode::StaleEpoch);
  FG_CHECK(fixture->server->stats().handshakes_rejected >= 1);
  fixture->stop();
  (void)fixture->governor->shutdown();
}

FG_TEST(adversarial_transport, a_handshake_without_an_incarnation_is_refused) {
  const FairnessPolicy policy = flat_policy(2);
  Result<std::unique_ptr<LinkFixture>> started = start_link(policy);
  FG_CHECK_OK(started.status());
  std::unique_ptr<LinkFixture>& fixture = started.value();

  FrameHeader header = header_for(FrameKind::Hello, policy.epoch, 3, 0);
  header.publisher_boot = BootId::none();
  ByteBuffer response;
  FG_CHECK_OK(raw_send("127.0.0.1", fixture->server->port(), encode_frame(header, ByteBuffer{}),
                       &response, 1));
  FG_CHECK(fixture->server->stats().handshakes_rejected >= 1);
  fixture->stop();
  (void)fixture->governor->shutdown();
}

FG_TEST(adversarial_transport, a_corrupted_frame_checksum_is_rejected) {
  const FairnessPolicy policy = flat_policy(2);
  Result<std::unique_ptr<LinkFixture>> started = start_link(policy);
  FG_CHECK_OK(started.status());
  std::unique_ptr<LinkFixture>& fixture = started.value();

  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 10);
  observe(evidence, 2, 1, 20);
  ByteBuffer frame = encode_frame(header_for(FrameKind::Evidence, policy.epoch, 4, 0),
                                  codec::encode_evidence(evidence));
  const std::size_t payload_start = kFrameHeaderSize;
  frame[payload_start] ^= 0xFF;
  ByteBuffer response;
  (void)raw_send("127.0.0.1", fixture->server->port(), frame, &response, 1);
  const LinkStats stats = fixture->server->stats();
  FG_CHECK(stats.checksum_failures >= 1);
  FG_CHECK_EQ(fixture->governor->staged_snapshot_count(), 0u);
  fixture->stop();
  (void)fixture->governor->shutdown();
}

FG_TEST(adversarial_transport, an_oversized_declared_length_is_rejected_before_allocation) {
  const FairnessPolicy policy = flat_policy(2);
  Result<std::unique_ptr<LinkFixture>> started = start_link(policy);
  FG_CHECK_OK(started.status());
  std::unique_ptr<LinkFixture>& fixture = started.value();

  ByteBuffer frame = encode_frame(header_for(FrameKind::Evidence, policy.epoch, 5, 0), ByteBuffer{});
  store_le<std::uint32_t>(frame.data() + 12, 0xFFFFFFFFu);
  ByteBuffer response;
  (void)raw_send("127.0.0.1", fixture->server->port(), frame, &response, 1);
  FG_CHECK(fixture->server->stats().oversized_frames >= 1);
  fixture->stop();
  (void)fixture->governor->shutdown();
}

FG_TEST(adversarial_transport, a_truncated_frame_leaves_the_server_healthy) {
  const FairnessPolicy policy = flat_policy(2);
  Result<std::unique_ptr<LinkFixture>> started = start_link(policy);
  FG_CHECK_OK(started.status());
  std::unique_ptr<LinkFixture>& fixture = started.value();

  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 10);
  observe(evidence, 2, 1, 20);
  ByteBuffer frame = encode_frame(header_for(FrameKind::Evidence, policy.epoch, 6, 0),
                                  codec::encode_evidence(evidence));
  frame.resize(kFrameHeaderSize + 3);
  ByteBuffer response;
  (void)raw_send("127.0.0.1", fixture->server->port(), frame, &response, 1);
  FG_CHECK(fixture->server->stats().frames_rejected >= 1);
  FG_CHECK_EQ(fixture->governor->staged_snapshot_count(), 0u);

  // A correct publisher is still served afterwards.
  LinkClientOptions options;
  options.port = fixture->server->port();
  options.epoch = policy.epoch;
  options.publisher = PublisherId::from_value(7);
  HelloAck ack;
  Result<std::unique_ptr<EvidenceLinkClient>> client =
      EvidenceLinkClient::connect(options, ack);
  FG_CHECK_OK(client.status());
  Result<EvidenceAck> submitted = client.value()->submit(evidence);
  FG_CHECK_OK(submitted.status());
  FG_CHECK(submitted.value().accepted());
  FG_CHECK_EQ(fixture->governor->staged_snapshot_count(), 1u);
  client.value()->close();
  fixture->stop();
  (void)fixture->governor->shutdown();
}

FG_TEST(adversarial_transport, a_skipped_and_a_replayed_sequence_are_rejected) {
  const FairnessPolicy policy = flat_policy(2);
  Result<std::unique_ptr<LinkFixture>> started = start_link(policy);
  FG_CHECK_OK(started.status());
  std::unique_ptr<LinkFixture>& fixture = started.value();

  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 10);
  observe(evidence, 2, 1, 20);
  const ByteBuffer evidence_payload = codec::encode_evidence(evidence);

  // A session that skips sequence 1 must be refused.
  {
    Result<std::unique_ptr<RawLinkSession>> raw =
        RawLinkSession::connect("127.0.0.1", fixture->server->port());
    FG_CHECK_OK(raw.status());
    std::unique_ptr<RawLinkSession>& session = raw.value();
    FG_CHECK_OK(session->send_raw(
        encode_frame(header_for(FrameKind::Hello, policy.epoch, 8, 0), ByteBuffer{})));
    Result<ByteBuffer> ack_payload = session->receive();
    FG_CHECK_OK(ack_payload.status());
    HelloAck ack;
    FG_CHECK_OK(decode_hello_ack(ack_payload.value(), ack));
    FG_CHECK(ack.accepted());

    FrameHeader skipped = header_for(FrameKind::Evidence, policy.epoch, 8, 5);
    skipped.session_id = ack.session_id;
    FG_CHECK_OK(session->send_raw(encode_frame(skipped, evidence_payload)));
    Result<ByteBuffer> response = session->receive();
    FG_CHECK_OK(response.status());
    ByteReader reader(response.value().data(), response.value().size());
    std::uint16_t code = 0;
    std::string detail;
    FG_CHECK(reader.u16(code));
    FG_CHECK(reader.blob(detail, kMaxFrameDetailBytes));
    FG_CHECK_EQ(static_cast<StatusCode>(code), StatusCode::SequenceViolation);
    session->close();
  }

  // A session that replays an accepted sequence must be refused.
  {
    Result<std::unique_ptr<RawLinkSession>> raw =
        RawLinkSession::connect("127.0.0.1", fixture->server->port());
    FG_CHECK_OK(raw.status());
    std::unique_ptr<RawLinkSession>& session = raw.value();
    FG_CHECK_OK(session->send_raw(
        encode_frame(header_for(FrameKind::Hello, policy.epoch, 9, 0), ByteBuffer{})));
    Result<ByteBuffer> ack_payload = session->receive();
    FG_CHECK_OK(ack_payload.status());
    HelloAck ack;
    FG_CHECK_OK(decode_hello_ack(ack_payload.value(), ack));
    FG_CHECK(ack.accepted());

    FrameHeader first = header_for(FrameKind::Evidence, policy.epoch, 9, 1);
    first.session_id = ack.session_id;
    FG_CHECK_OK(session->send_raw(encode_frame(first, evidence_payload)));
    Result<ByteBuffer> first_response = session->receive();
    FG_CHECK_OK(first_response.status());
    EvidenceAck first_ack;
    FG_CHECK_OK(decode_evidence_ack(first_response.value(), first_ack));
    FG_CHECK(first_ack.accepted());

    FrameHeader replay = header_for(FrameKind::Evidence, policy.epoch, 9, 1);
    replay.session_id = ack.session_id;
    FG_CHECK_OK(session->send_raw(encode_frame(replay, evidence_payload)));
    Result<ByteBuffer> response = session->receive();
    FG_CHECK_OK(response.status());
    ByteReader reader(response.value().data(), response.value().size());
    std::uint16_t code = 0;
    std::string detail;
    FG_CHECK(reader.u16(code));
    FG_CHECK(reader.blob(detail, kMaxFrameDetailBytes));
    FG_CHECK_EQ(static_cast<StatusCode>(code), StatusCode::SequenceViolation);
    session->close();
  }

  FG_CHECK(fixture->server->stats().sequence_violations >= 2);
  fixture->stop();
  (void)fixture->governor->shutdown();
}

FG_TEST(adversarial_transport, a_peer_that_never_speaks_is_dropped_by_its_idle_budget) {
  const FairnessPolicy policy = flat_policy(2);
  LinkServerOptions options;
  options.receive_poll_ms = 10;
  options.idle_polls = 3;
  Result<std::unique_ptr<LinkFixture>> started = start_link(policy, options);
  FG_CHECK_OK(started.status());
  std::unique_ptr<LinkFixture>& fixture = started.value();

  Result<std::unique_ptr<RawLinkSession>> raw =
      RawLinkSession::connect("127.0.0.1", fixture->server->port());
  FG_CHECK_OK(raw.status());
  // Say nothing at all. The server must retire the connection rather than let it
  // hold a slot forever.
  Result<ByteBuffer> response = raw.value()->receive();
  FG_CHECK(!response.ok());
  FG_CHECK(response.code() == StatusCode::LinkClosed ||
           response.code() == StatusCode::TransportError);
  raw.value()->close();
  // The abandoned connection is counted as exactly that, not as a rejected
  // handshake and not as accepted evidence.
  FG_CHECK_EQ(fixture->server->stats().handshakes_rejected, 0u);
  FG_CHECK(fixture->server->stats().connections_idle_dropped >= 1);
  FG_CHECK_EQ(fixture->governor->staged_snapshot_count(), 0u);
  fixture->stop();
  (void)fixture->governor->shutdown();
}

FG_TEST(adversarial_transport, shutdown_races_a_running_accept_loop_safely) {
  const FairnessPolicy policy = flat_policy(2);
  Result<std::unique_ptr<LinkFixture>> started = start_link(policy);
  FG_CHECK_OK(started.status());
  std::unique_ptr<LinkFixture>& fixture = started.value();

  // Open connections that stay open without completing a handshake, then shut
  // down while the accept loop is still running. Shutdown must drain the accept
  // loop and join every worker without deadlocking.
  std::vector<std::unique_ptr<RawLinkSession>> idle;
  for (int index = 0; index < 3; ++index) {
    Result<std::unique_ptr<RawLinkSession>> raw =
        RawLinkSession::connect("127.0.0.1", fixture->server->port());
    if (raw.ok()) {
      idle.push_back(raw.take());
    }
  }
  (void)fixture->server->shutdown();
  FG_CHECK(fixture->server->stopped());
  idle.clear();
  if (fixture->serving.joinable()) {
    fixture->serving.join();
  }
  FG_CHECK_EQ(fixture->governor->staged_snapshot_count(), 0u);
  (void)fixture->governor->shutdown();
}

FG_TEST(adversarial_transport, the_connection_budget_is_enforced) {
  const FairnessPolicy policy = flat_policy(2);
  Result<std::unique_ptr<LinkFixture>> started = start_link(policy);
  FG_CHECK_OK(started.status());
  std::unique_ptr<LinkFixture>& fixture = started.value();

  std::vector<std::unique_ptr<EvidenceLinkClient>> clients;
  std::uint32_t accepted = 0;
  for (std::uint32_t i = 0; i < 8; ++i) {
    LinkClientOptions options;
    options.port = fixture->server->port();
    options.epoch = policy.epoch;
    options.publisher = PublisherId::from_value(100 + i);
    HelloAck ack;
    Result<std::unique_ptr<EvidenceLinkClient>> client =
        EvidenceLinkClient::connect(options, ack);
    if (client.ok()) {
      ++accepted;
      clients.push_back(client.take());
    }
  }
  FG_CHECK(accepted <= 4u);
  clients.clear();
  fixture->stop();
  (void)fixture->governor->shutdown();
}
