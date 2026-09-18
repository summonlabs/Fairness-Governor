// Fairness Governor - real framed transport over loopback TCP.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The evidence link is the only way dynamic service evidence enters a governor
// from another process. It is a real OS socket carrying real framed messages
// between real OS processes. Handshake performs epoch and incarnation fencing:
// a publisher whose epoch is behind is rejected, and a publisher may not reuse
// another publisher's identity from a different boot.
#ifndef FAIRNESS_GOVERNOR_LINK_LINK_HPP
#define FAIRNESS_GOVERNOR_LINK_LINK_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "fairness_governor/core/status.hpp"
#include "fairness_governor/eval/governor.hpp"
#include "fairness_governor/link/frame.hpp"

namespace fairness_governor {

/// Aggregate link counters. Reported by the link tool and asserted by tests.
struct LinkStats {
  std::uint64_t connections_accepted{0};
  std::uint64_t connections_rejected{0};
  std::uint64_t handshakes_completed{0};
  std::uint64_t handshakes_rejected{0};
  std::uint64_t frames_received{0};
  std::uint64_t frames_rejected{0};
  std::uint64_t evidence_accepted{0};
  std::uint64_t evidence_duplicate{0};
  std::uint64_t evidence_rejected{0};
  std::uint64_t checksum_failures{0};
  std::uint64_t oversized_frames{0};
  std::uint64_t sequence_violations{0};
  std::uint64_t windows_committed{0};
  std::uint64_t evaluations_failed{0};
  std::uint64_t bytes_received{0};
  std::uint64_t bytes_sent{0};
  std::uint64_t shutdown_requests{0};
  std::uint64_t connections_idle_dropped{0};
};

struct LinkServerOptions {
  std::string bind_address{"127.0.0.1"};
  /// 0 requests an ephemeral port; the bound port is reported by port().
  std::uint16_t port{0};
  /// Hard bound on concurrently served connections.
  std::uint32_t max_connections{4};
  std::uint32_t accept_backlog{8};
  /// Bound on a single frame payload (never above kMaxFramePayloadBytes).
  std::uint32_t max_payload{kMaxFramePayloadBytes};
  /// Milliseconds a worker blocks on receive before re-checking the stop flag.
  std::uint32_t receive_poll_ms{200};
  /// Consecutive idle polls tolerated before a connection is dropped. This is a
  /// resource bound, not a correctness deadline: a peer that stops sending must
  /// not be able to hold a connection slot indefinitely.
  std::uint32_t idle_polls{50};
  /// When true, every newly staged snapshot is immediately evaluated and its
  /// window durably committed. This turns the link into a complete governance
  /// loop; when false the link only stages evidence for a separate evaluator.
  bool evaluate_on_ingest{false};
};

/// Serves framed evidence submissions to one governor. Exactly one thread runs
/// the accept loop; connection workers are bounded and joined by shutdown().
class EvidenceLinkServer {
 public:
  EvidenceLinkServer(const EvidenceLinkServer&) = delete;
  EvidenceLinkServer& operator=(const EvidenceLinkServer&) = delete;
  ~EvidenceLinkServer();

  [[nodiscard]] static Result<std::unique_ptr<EvidenceLinkServer>> start(
      FairnessGovernor& governor, LinkServerOptions options);

  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] const std::string& bound_address() const noexcept;

  /// Runs the accept loop on the calling thread until stop is requested or a
  /// goodbye-with-shutdown frame is received.
  void serve();

  /// Asks the accept loop to stop. Safe to call from any thread, including a
  /// signal-handling-free context; never blocks on a lock held by a worker.
  void request_stop() noexcept;

  /// Stops accepting, drains workers, and releases sockets. Idempotent.
  [[nodiscard]] Status shutdown();

  [[nodiscard]] LinkStats stats() const noexcept;
  [[nodiscard]] bool stopped() const noexcept;

 private:
  struct Impl;
  explicit EvidenceLinkServer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

struct LinkClientOptions {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  FabricEpoch epoch{};
  PublisherId publisher{};
  /// Publisher boot identity. Left invalid, the client generates a fresh one for
  /// this process, which is what fences a recycled publisher identity.
  BootId publisher_boot{};
  std::uint64_t publisher_ordinal{1};
  /// Milliseconds a receive blocks before re-checking for closure.
  std::uint32_t receive_poll_ms{200};
  /// Bound on how long connect() keeps retrying a refused connection.
  std::uint32_t connect_attempts{100};
  std::uint32_t connect_retry_ms{50};
};

/// A publisher-side connection. Real socket, real frames, real acknowledgements.
class EvidenceLinkClient {
 public:
  EvidenceLinkClient(const EvidenceLinkClient&) = delete;
  EvidenceLinkClient& operator=(const EvidenceLinkClient&) = delete;
  ~EvidenceLinkClient();

  /// Connects and performs the handshake. On rejection the returned Status
  /// carries the server's reason (for example StaleEpoch).
  [[nodiscard]] static Result<std::unique_ptr<EvidenceLinkClient>> connect(
      const LinkClientOptions& options, HelloAck& ack_out);

  [[nodiscard]] Result<EvidenceAck> submit(const EvidenceSnapshot& snapshot);

  /// Sends a goodbye frame. When `request_shutdown` is true the server stops
  /// accepting after acknowledging.
  [[nodiscard]] Status goodbye(bool request_shutdown);

  void close() noexcept;

  [[nodiscard]] std::uint64_t session_id() const noexcept;
  [[nodiscard]] SequenceNumber next_sequence() const noexcept;

 private:
  struct Impl;
  explicit EvidenceLinkClient(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

/// A raw, unvalidated peer session. Exposed deliberately and only for the
/// hardening tests: it lets them drive the server with traffic no well-behaved
/// client would ever produce (a truncated frame, a bad checksum, a skipped
/// sequence, an oversized declared length) without duplicating socket code in
/// the test binary. It performs no framing checks of its own.
class RawLinkSession {
 public:
  RawLinkSession(const RawLinkSession&) = delete;
  RawLinkSession& operator=(const RawLinkSession&) = delete;
  ~RawLinkSession();

  [[nodiscard]] static Result<std::unique_ptr<RawLinkSession>> connect(const std::string& host,
                                                                      std::uint16_t port);

  /// Writes `bytes` verbatim.
  [[nodiscard]] Status send_raw(const ByteBuffer& bytes);

  /// Reads exactly one frame and returns its payload. Returns TransportError on
  /// the bounded receive timeout and LinkClosed at end of stream.
  [[nodiscard]] Result<ByteBuffer> receive();

  void close() noexcept;

 private:
  struct Impl;
  explicit RawLinkSession(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

/// Sends raw bytes and reads at most `response_limit` frame payloads.
[[nodiscard]] Status raw_send(const std::string& host, std::uint16_t port,
                              const ByteBuffer& bytes, ByteBuffer* response_out,
                              std::size_t response_limit);

/// Sends each frame verbatim on one connection, reading up to `response_limit`
/// frame payloads. Used by the adversarial transport tests.
[[nodiscard]] Status send_raw_frames(const std::string& host, std::uint16_t port,
                                     const std::vector<ByteBuffer>& frames,
                                     std::vector<ByteBuffer>& responses,
                                     std::size_t response_limit);

}  // namespace fairness_governor

#endif  // FAIRNESS_GOVERNOR_LINK_LINK_HPP
