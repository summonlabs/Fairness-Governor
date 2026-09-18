// Fairness Governor - framed evidence link over real loopback sockets.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/link/link.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "fairness_governor/core/checked.hpp"
#include "fairness_governor/core/entropy.hpp"
#include "fairness_governor/persist/codec.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace fairness_governor {
namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

/// Initialises Winsock exactly once and remembers whether it succeeded. The
/// result is checked rather than discarded: a process that could not start
/// Winsock must report a transport error, not fail later in a confusing way.
[[nodiscard]] bool ensure_winsock() {
  static std::once_flag once;
  static bool available = false;
  std::call_once(once, []() {
    WSADATA data {};
    available = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  });
  return available;
}

[[nodiscard]] Status socket_error(std::string_view what) {
  return Status::failure(StatusCode::TransportError,
                         std::string(what) + " failed with winsock error " +
                             to_decimal(static_cast<std::uint64_t>(WSAGetLastError())));
}

void close_socket(SocketHandle handle) {
  if (handle != kInvalidSocket) {
    closesocket(handle);
  }
}

[[nodiscard]] bool would_block() {
  const int code = WSAGetLastError();
  return code == WSAETIMEDOUT || code == WSAEWOULDBLOCK;
}

void set_timeouts(SocketHandle handle, std::uint32_t milliseconds, bool for_receive) {
  DWORD value = milliseconds;
  const int option = for_receive ? SO_RCVTIMEO : SO_SNDTIMEO;
  setsockopt(handle, SOL_SOCKET, option, reinterpret_cast<const char*>(&value), sizeof(value));
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

[[nodiscard]] bool ensure_winsock() { return true; }

[[nodiscard]] Status socket_error(std::string_view what) {
  return Status::failure(StatusCode::TransportError, std::string(what) + " failed");
}

void close_socket(SocketHandle handle) {
  if (handle != kInvalidSocket) {
    ::close(handle);
  }
}

[[nodiscard]] bool would_block() {
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

void set_timeouts(SocketHandle handle, std::uint32_t milliseconds, bool for_receive) {
  timeval value {};
  value.tv_sec = static_cast<long>(milliseconds / 1000);
  value.tv_usec = static_cast<long>((milliseconds % 1000) * 1000);
  const int option = for_receive ? SO_RCVTIMEO : SO_SNDTIMEO;
  setsockopt(handle, SOL_SOCKET, option, &value, sizeof(value));
}
#endif

enum class ReadOutcome {
  Complete,
  Timeout,
  Closed,
  Error,
};

/// Reads exactly `size` bytes, counting consecutive receive timeouts against
/// `idle_polls`. A stalled peer is dropped rather than holding a worker slot.
[[nodiscard]] ReadOutcome read_exact(SocketHandle handle, std::uint8_t* out, std::size_t size,
                                     const std::atomic<bool>& stop, std::uint32_t idle_polls) {
  std::size_t offset = 0;
  std::uint32_t idle = 0;
  while (offset < size) {
    if (stop.load(std::memory_order_acquire)) {
      return ReadOutcome::Closed;
    }
#ifdef _WIN32
    const int chunk = static_cast<int>(std::min<std::size_t>(size - offset, 1u << 16));
    const int received = ::recv(handle, reinterpret_cast<char*>(out + offset), chunk, 0);
#else
    const ssize_t received = ::recv(handle, out + offset, size - offset, 0);
#endif
    if (received == 0) {
      return ReadOutcome::Closed;
    }
    if (received < 0) {
      if (would_block()) {
        if (++idle > idle_polls) {
          return ReadOutcome::Timeout;
        }
        continue;
      }
      return ReadOutcome::Error;
    }
    idle = 0;
    offset += static_cast<std::size_t>(received);
  }
  return ReadOutcome::Complete;
}

[[nodiscard]] Status write_all(SocketHandle handle, const std::uint8_t* data, std::size_t size) {
  std::size_t offset = 0;
  while (offset < size) {
#ifdef _WIN32
    const int chunk = static_cast<int>(std::min<std::size_t>(size - offset, 1u << 16));
    const int sent = ::send(handle, reinterpret_cast<const char*>(data + offset), chunk, 0);
#else
    const ssize_t sent = ::send(handle, data + offset, size - offset, 0);
#endif
    if (sent <= 0) {
      if (would_block()) {
        continue;
      }
      return socket_error("send");
    }
    offset += static_cast<std::size_t>(sent);
  }
  return Status::success();
}

struct InboundFrame {
  FrameHeader header;
  ByteBuffer payload;
};

/// Reads one complete frame. A short read at a frame boundary is closure; a
/// short read inside a frame is a truncated frame and is reported as such.
[[nodiscard]] Status read_frame(SocketHandle handle, std::uint32_t max_payload,
                                const std::atomic<bool>& stop, std::uint32_t idle_polls,
                                InboundFrame& out, bool& closed) {
  std::uint8_t header_bytes[kFrameHeaderSize];
  std::size_t first = 0;
  std::uint32_t idle = 0;
  while (first < kFrameHeaderSize) {
    if (stop.load(std::memory_order_acquire)) {
      closed = true;
      return Status(StatusCode::LinkClosed, "link is stopping");
    }
#ifdef _WIN32
    const int received =
        ::recv(handle, reinterpret_cast<char*>(header_bytes + first),
               static_cast<int>(kFrameHeaderSize - first), 0);
#else
    const ssize_t received = ::recv(handle, header_bytes + first, kFrameHeaderSize - first, 0);
#endif
    if (received == 0) {
      if (first == 0) {
        closed = true;
        return Status(StatusCode::LinkClosed, "peer closed the link between frames");
      }
      closed = true;
      return Status(StatusCode::Truncated, "peer closed the link inside a frame header");
    }
    if (received < 0) {
      if (would_block()) {
        // A peer that opens a connection and then says nothing is dropped once
        // its idle budget is spent, so it cannot hold a worker slot forever.
        if (++idle > idle_polls) {
          closed = true;
          return Status(StatusCode::IdleTimeout, "peer went idle inside a frame header");
        }
        continue;
      }
      closed = true;
      return socket_error("recv");
    }
    idle = 0;
    first += static_cast<std::size_t>(received);
  }
  const Status header_status =
      decode_frame_header(header_bytes, kFrameHeaderSize, out.header, max_payload);
  if (!header_status.ok()) {
    return header_status;
  }
  out.payload.assign(out.header.payload_length, 0);
  if (!out.payload.empty()) {
    const ReadOutcome outcome =
        read_exact(handle, out.payload.data(), out.payload.size(), stop, idle_polls);
    if (outcome != ReadOutcome::Complete) {
      closed = outcome == ReadOutcome::Closed;
      return Status(outcome == ReadOutcome::Timeout ? StatusCode::IdleTimeout
                                                    : StatusCode::Truncated,
                    "frame payload was truncated or stalled");
    }
  }
  return verify_frame_payload(out.header, out.payload.data(), out.payload.size());
}

}  // namespace

// --- Server ------------------------------------------------------------------

struct EvidenceLinkServer::Impl {
  FairnessGovernor* governor{nullptr};
  LinkServerOptions options{};
  SocketHandle listener{kInvalidSocket};
  std::uint16_t port{0};
  std::string address;
  std::atomic<bool> stop{false};
  std::atomic<bool> stopped{false};
  /// True while the accept loop is executing on some thread.
  std::atomic<bool> serving{false};
  std::atomic<std::uint32_t> active_workers{0};

  mutable std::mutex stats_mutex;
  LinkStats stats{};

  struct Worker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::mutex workers_mutex;
  std::vector<Worker> workers;

  void record(const std::function<void(LinkStats&)>& update) {
    std::lock_guard<std::mutex> guard(stats_mutex);
    update(stats);
  }

  [[nodiscard]] LinkStats snapshot() const {
    std::lock_guard<std::mutex> guard(stats_mutex);
    return stats;
  }

  /// Builds the exact evaluation request for a snapshot the server is about to
  /// stage. The governor never guesses which evidence is current.
  [[nodiscard]] EvaluationRequest make_request_for(const EvidenceSnapshot& snapshot) const {
    // One atomic read: mixing separately read generations could bind a request
    // to a policy/epoch combination that never existed.
    const LiveAuthority live = governor->authority();
    EvaluationRequest request;
    request.policy_id = live.policy_id;
    request.policy_generation = live.policy_generation;
    request.epoch = live.epoch;
    request.evidence_id = snapshot.id;
    request.evidence_generation = snapshot.generation;
    request.window = snapshot.window;
    request.window_generation = snapshot.window_generation;
    request.now_ns = snapshot.captured_at_ns;
    request.request_id = snapshot.sequence;
    return request;
  }

  [[nodiscard]] Status send_frame(SocketHandle handle, FrameKind kind, const ByteBuffer& payload,
                                  std::uint64_t session, SequenceNumber sequence,
                                  std::uint16_t flags) {
    FrameHeader header;
    header.kind = kind;
    header.flags = flags;
    const LiveAuthority live = governor->authority();
    header.session_id = session;
    header.epoch = live.epoch;
    header.publisher = PublisherId::none();
    header.publisher_boot = live.governor.boot;
    header.publisher_ordinal = live.governor.ordinal;
    header.sequence = sequence;
    const ByteBuffer frame = encode_frame(header, payload);
    const Status status = write_all(handle, frame.data(), frame.size());
    if (status.ok()) {
      record([&](LinkStats& value) { value.bytes_sent += frame.size(); });
    }
    return status;
  }

  /// Best-effort error report. Every call site is already terminating the
  /// connection, so a failure to deliver the diagnostic changes nothing.
  void send_error(SocketHandle handle, std::uint64_t session, StatusCode code,
                  const std::string& detail) {
    ByteBuffer payload;
    ByteWriter writer(payload);
    writer.u16(static_cast<std::uint16_t>(code));
    writer.blob(detail.size() > kMaxFrameDetailBytes ? detail.substr(0, kMaxFrameDetailBytes)
                                                     : detail);
    (void)send_frame(handle, FrameKind::Error, payload, session, 0, kFrameFlagNone);
  }

  void serve_connection(SocketHandle handle) {
    struct WorkerScope {
      Impl* impl;
      ~WorkerScope() { impl->active_workers.fetch_sub(1, std::memory_order_acq_rel); }
    } scope{this};
    active_workers.fetch_add(1, std::memory_order_acq_rel);

    std::uint64_t session = 0;
    SequenceNumber expected = 1;
    InboundFrame frame;
    bool closed = false;

    // --- Handshake ---------------------------------------------------------
    Status status = read_frame(handle, options.max_payload, stop, options.idle_polls, frame, closed);
    if (!status.ok()) {
      record([&](LinkStats& value) {
        ++value.frames_rejected;
        // A peer that connects and leaves without sending anything is a probe,
        // and a peer that connects and then goes silent is an abandoned
        // connection. Neither is a rejected handshake; everything else is, and
        // is counted with its specific cause.
        if (status.code() == StatusCode::IdleTimeout) {
          ++value.connections_idle_dropped;
        } else if (status.code() != StatusCode::LinkClosed) {
          ++value.handshakes_rejected;
        }
        if (status.code() == StatusCode::ChecksumMismatch) {
          ++value.checksum_failures;
        } else if (status.code() == StatusCode::FrameTooLarge) {
          ++value.oversized_frames;
        }
      });
      return;
    }
    record([&](LinkStats& value) { ++value.frames_received; });
    if (frame.header.kind != FrameKind::Hello) {
      record([](LinkStats& value) { ++value.handshakes_rejected; });
      send_error(handle, 0, StatusCode::HandshakeRejected, "expected a HELLO frame");
      return;
    }
    if (!frame.header.publisher.valid() || !frame.header.publisher_boot.valid() ||
        frame.header.publisher_ordinal == 0) {
      record([](LinkStats& value) { ++value.handshakes_rejected; });
      send_error(handle, 0, StatusCode::HandshakeRejected,
                 "publisher identity or incarnation is missing");
      return;
    }
    if (frame.header.epoch != governor->epoch()) {
      record([](LinkStats& value) { ++value.handshakes_rejected; });
      const LiveAuthority live = governor->authority();
      HelloAck ack;
      ack.code = StatusCode::StaleEpoch;
      ack.epoch = live.epoch;
      ack.server_boot = live.governor.boot;
      ack.server_ordinal = live.governor.ordinal;
      ack.detail = "publisher epoch is not the live fabric epoch";
      (void)send_frame(handle, FrameKind::HelloAck, encode_hello_ack(ack), 0, 0, kFrameFlagNone);
      return;
    }
    if (governor->shutting_down()) {
      record([](LinkStats& value) { ++value.handshakes_rejected; });
      send_error(handle, 0, StatusCode::ShuttingDown, "governor is shutting down");
      return;
    }
    session = 0xF00D0000ULL ^ (frame.header.publisher.value() * 0x9E3779B97F4A7C15ULL) ^
              frame.header.sequence ^ (frame.header.publisher_boot.value() >> 17);
    if (session == 0) {
      session = 1;
    }
    {
      HelloAck ack;
      const LiveAuthority live = governor->authority();
      ack.code = StatusCode::Ok;
      ack.session_id = session;
      ack.epoch = live.epoch;
      ack.max_payload = options.max_payload;
      ack.server_boot = live.governor.boot;
      ack.server_ordinal = live.governor.ordinal;
      ack.policy_id = live.policy_id;
      ack.policy_generation = live.policy_generation;
      ack.accounting_generation = live.accounting_generation;
      ack.detail = "handshake accepted";
      status = send_frame(handle, FrameKind::HelloAck, encode_hello_ack(ack), session, 0,
                          kFrameFlagNone);
    }
    record([](LinkStats& value) { ++value.handshakes_completed; });
    if (!status.ok()) {
      return;
    }

    // --- Frame loop --------------------------------------------------------
    for (;;) {
      if (stop.load(std::memory_order_acquire)) {
        return;
      }
      frame = InboundFrame{};
      closed = false;
      status = read_frame(handle, options.max_payload, stop, options.idle_polls, frame, closed);
      if (!status.ok()) {
        if (closed) {
          return;
        }
        record([&](LinkStats& value) {
          ++value.frames_rejected;
          if (status.code() == StatusCode::ChecksumMismatch) {
            ++value.checksum_failures;
          } else if (status.code() == StatusCode::FrameTooLarge) {
            ++value.oversized_frames;
          } else if (status.code() == StatusCode::IdleTimeout) {
            ++value.connections_idle_dropped;
          }
        });
        send_error(handle, session, status.code(), std::string(status.detail()));
        return;
      }
      record([&](LinkStats& value) {
        ++value.frames_received;
        value.bytes_received += frame.payload.size() + kFrameHeaderSize;
      });
      if (frame.header.session_id != session) {
        record([](LinkStats& value) { ++value.frames_rejected; });
        send_error(handle, session, StatusCode::FrameInvalid, "frame session does not match");
        return;
      }
      if (frame.header.epoch != governor->epoch()) {
        record([](LinkStats& value) { ++value.frames_rejected; });
        send_error(handle, session, StatusCode::StaleEpoch, "frame epoch is not the live epoch");
        return;
      }
      switch (frame.header.kind) {
        case FrameKind::Goodbye: {
          if ((frame.header.flags & kFrameFlagRequestShutdown) != 0) {
            record([](LinkStats& value) { ++value.shutdown_requests; });
            (void)send_frame(handle, FrameKind::Goodbye, ByteBuffer{}, session, expected,
                             kFrameFlagNone);
            stop.store(true, std::memory_order_release);
          }
          return;
        }
        case FrameKind::Evidence: {
          if (frame.header.sequence < expected) {
            record([](LinkStats& value) {
              ++value.frames_rejected;
              ++value.sequence_violations;
            });
            send_error(handle, session, StatusCode::SequenceViolation,
                       "frame sequence went backwards");
            return;
          }
          if (frame.header.sequence > expected) {
            record([](LinkStats& value) {
              ++value.frames_rejected;
              ++value.sequence_violations;
            });
            send_error(handle, session, StatusCode::SequenceViolation, "frame sequence skipped");
            return;
          }
          ++expected;
          EvidenceSnapshot snapshot;
          const Status decode = codec::decode_evidence(frame.payload, snapshot);
          EvidenceAck ack;
          ack.snapshot = snapshot.id;
          ack.generation = snapshot.generation;
          ack.accounting_generation = governor->accounting_generation();
          if (!decode.ok()) {
            ack.code = decode.code();
            ack.detail = std::string(decode.detail());
            record([](LinkStats& value) { ++value.evidence_rejected; });
          } else {
            const EvaluationRequest request = make_request_for(snapshot);
            Result<IngestResult> ingested = governor->ingest_evidence(std::move(snapshot));
            if (ingested.ok()) {
              ack.code = ingested.value().disposition == IngestResult::Disposition::Staged
                             ? StatusCode::Ok
                             : StatusCode::Duplicate;
              ack.disposition = ingested.value().disposition == IngestResult::Disposition::Staged
                                    ? 0
                                    : 1;
              ack.detail = "evidence accepted";
              record([&](LinkStats& value) {
                if (ack.disposition == 0) {
                  ++value.evidence_accepted;
                } else {
                  ++value.evidence_duplicate;
                }
              });
              if (options.evaluate_on_ingest && ack.disposition == 0) {
                // Staging and committing are separate authoritative steps: the
                // decision is computed first and only then durably committed.
                Result<FairnessDecision> decision = governor->evaluate(request);
                if (decision.ok()) {
                  const Status committed = governor->commit(decision.value());
                  if (committed.ok()) {
                    record([](LinkStats& value) { ++value.windows_committed; });
                  } else {
                    record([](LinkStats& value) { ++value.evaluations_failed; });
                  }
                } else {
                  record([](LinkStats& value) { ++value.evaluations_failed; });
                }
              }
              ack.accounting_generation = governor->accounting_generation();
            } else {
              ack.code = ingested.code();
              ack.detail = std::string(ingested.status().detail());
              record([](LinkStats& value) { ++value.evidence_rejected; });
            }
          }
          const Status sent = send_frame(handle, FrameKind::EvidenceAck,
                                         encode_evidence_ack(ack), session, frame.header.sequence,
                                         kFrameFlagNone);
          if (!sent.ok()) {
            return;
          }
          break;
        }
        default: {
          record([](LinkStats& value) { ++value.frames_rejected; });
          send_error(handle, session, StatusCode::FrameInvalid, "unexpected frame kind");
          return;
        }
      }
    }
  }

  void run_worker(SocketHandle handle, std::shared_ptr<std::atomic<bool>> done) {
    serve_connection(handle);
    close_socket(handle);
    done->store(true, std::memory_order_release);
  }

  /// Retires finished workers. The registry lock is held only to move finished
  /// entries out; the joins happen outside it.
  void prune_workers() {
    std::vector<Worker> finished;
    {
      std::lock_guard<std::mutex> guard(workers_mutex);
      auto it = workers.begin();
      while (it != workers.end()) {
        if (it->done->load(std::memory_order_acquire)) {
          finished.push_back(std::move(*it));
          it = workers.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (Worker& worker : finished) {
      if (worker.thread.joinable()) {
        worker.thread.join();
      }
    }
  }
};

EvidenceLinkServer::EvidenceLinkServer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

EvidenceLinkServer::~EvidenceLinkServer() {
  if (impl_ != nullptr) {
    (void)shutdown();
  }
}

Result<std::unique_ptr<EvidenceLinkServer>> EvidenceLinkServer::start(
    FairnessGovernor& governor, LinkServerOptions options) {
  if (!ensure_winsock()) {
    return Status(StatusCode::TransportError, "the socket subsystem could not be initialised");
  }
  if (options.max_payload == 0 || options.max_payload > kMaxFramePayloadBytes) {
    return Status(StatusCode::OutOfRange, "max payload is outside the permitted range");
  }
  if (options.max_connections == 0 || options.max_connections > 64) {
    return Status(StatusCode::OutOfRange, "max connections is outside the permitted range");
  }
  options.receive_poll_ms = std::max<std::uint32_t>(options.receive_poll_ms, 20);

  auto impl = std::make_unique<Impl>();
  impl->governor = &governor;
  impl->options = options;

  SocketHandle listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == kInvalidSocket) {
    return socket_error("socket");
  }
  int reuse = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
             sizeof(reuse));

  sockaddr_in address {};
  address.sin_family = AF_INET;
  address.sin_port = htons(options.port);
  if (options.bind_address.empty() || options.bind_address == "127.0.0.1" ||
      options.bind_address == "localhost") {
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  } else if (::inet_pton(AF_INET, options.bind_address.c_str(), &address.sin_addr) != 1) {
    close_socket(listener);
    return Status(StatusCode::InvalidArgument, "bind address is not a valid IPv4 literal");
  }
  if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close_socket(listener);
    return socket_error("bind");
  }
  if (::listen(listener, static_cast<int>(options.accept_backlog)) != 0) {
    close_socket(listener);
    return socket_error("listen");
  }
  sockaddr_in bound {};
#ifdef _WIN32
  int bound_length = sizeof(bound);
#else
  socklen_t bound_length = sizeof(bound);
#endif
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
    close_socket(listener);
    return socket_error("getsockname");
  }
  impl->listener = listener;
  impl->port = ntohs(bound.sin_port);
  impl->address = options.bind_address.empty() ? "127.0.0.1" : options.bind_address;

  return std::unique_ptr<EvidenceLinkServer>(new EvidenceLinkServer(std::move(impl)));
}

std::uint16_t EvidenceLinkServer::port() const noexcept { return impl_->port; }
const std::string& EvidenceLinkServer::bound_address() const noexcept { return impl_->address; }

void EvidenceLinkServer::serve() {
  impl_->serving.store(true, std::memory_order_release);
  while (!impl_->stop.load(std::memory_order_acquire)) {
    impl_->prune_workers();
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(impl_->listener, &readable);
    timeval timeout {};
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;
    const int ready = ::select(static_cast<int>(impl_->listener + 1), &readable, nullptr, nullptr,
#ifdef _WIN32
                               &timeout);
#else
                               &timeout);
#endif
    if (impl_->stop.load(std::memory_order_acquire)) {
      break;
    }
    if (ready < 0) {
      continue;
    }
    if (ready == 0) {
      continue;
    }
    sockaddr_in peer {};
#ifdef _WIN32
    int peer_length = sizeof(peer);
#else
    socklen_t peer_length = sizeof(peer);
#endif
    SocketHandle connection =
        ::accept(impl_->listener, reinterpret_cast<sockaddr*>(&peer), &peer_length);
    if (connection == kInvalidSocket) {
      if (impl_->stop.load(std::memory_order_acquire)) {
        break;
      }
      continue;
    }
    set_timeouts(connection, impl_->options.receive_poll_ms, true);
    set_timeouts(connection, 5000, false);
    if (impl_->active_workers.load(std::memory_order_acquire) >= impl_->options.max_connections) {
      impl_->record([](LinkStats& value) { ++value.connections_rejected; });
      impl_->send_error(connection, 0, StatusCode::LinkBusy, "connection budget is exhausted");
      close_socket(connection);
      continue;
    }
    impl_->record([](LinkStats& value) { ++value.connections_accepted; });
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard<std::mutex> guard(impl_->workers_mutex);
    if (impl_->stop.load(std::memory_order_acquire)) {
      // The stop request arrived while this connection was being set up: retire
      // it instead of registering a worker that shutdown would not see.
      close_socket(connection);
      break;
    }
    impl_->workers.push_back(Impl::Worker{
        std::thread([impl = impl_.get(), connection, done]() { impl->run_worker(connection, done); }),
        done});
  }
  impl_->serving.store(false, std::memory_order_release);
  impl_->stopped.store(true, std::memory_order_release);
}

void EvidenceLinkServer::request_stop() noexcept { impl_->stop.store(true, std::memory_order_release); }

Status EvidenceLinkServer::shutdown() {
  impl_->stop.store(true, std::memory_order_release);
  // Wait for the accept loop to finish before touching the worker registry, so a
  // connection it is still registering can never be missed or registered after
  // the registry is drained. The wait is bounded by the accept poll interval.
  for (int spin = 0; impl_->serving.load(std::memory_order_acquire) && spin < 2000; ++spin) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // Drain the registry under the lock, then join OUTSIDE it: a worker must never
  // need a lock that the joining thread holds in order to finish.
  std::vector<Impl::Worker> pending;
  {
    std::lock_guard<std::mutex> guard(impl_->workers_mutex);
    pending.swap(impl_->workers);
  }
  for (Impl::Worker& worker : pending) {
    if (worker.thread.joinable()) {
      worker.thread.join();
    }
  }
  if (impl_->listener != kInvalidSocket) {
    close_socket(impl_->listener);
    impl_->listener = kInvalidSocket;
  }
  impl_->stopped.store(true, std::memory_order_release);
  return Status::success();
}

LinkStats EvidenceLinkServer::stats() const noexcept { return impl_->snapshot(); }
bool EvidenceLinkServer::stopped() const noexcept {
  return impl_->stopped.load(std::memory_order_acquire);
}

// --- Client ------------------------------------------------------------------

struct EvidenceLinkClient::Impl {
  SocketHandle handle{kInvalidSocket};
  LinkClientOptions options{};
  std::uint64_t session{0};
  SequenceNumber sequence{0};
  std::atomic<bool> stop{false};
};

EvidenceLinkClient::EvidenceLinkClient(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

EvidenceLinkClient::~EvidenceLinkClient() { close(); }

Result<std::unique_ptr<EvidenceLinkClient>> EvidenceLinkClient::connect(
    const LinkClientOptions& options, HelloAck& ack_out) {
  if (!ensure_winsock()) {
    return Status(StatusCode::TransportError, "the socket subsystem could not be initialised");
  }
  if (options.port == 0) {
    return Status(StatusCode::InvalidArgument, "a destination port is required");
  }
  if (!options.epoch.valid() || !options.publisher.valid()) {
    return Status(StatusCode::InvalidArgument, "epoch and publisher identity are required");
  }
  auto impl = std::make_unique<Impl>();
  impl->options = options;
  if (!impl->options.publisher_boot.valid()) {
    impl->options.publisher_boot = make_boot_id();
  }
  if (impl->options.publisher_ordinal == 0) {
    impl->options.publisher_ordinal = 1;
  }

  sockaddr_in address {};
  address.sin_family = AF_INET;
  address.sin_port = htons(options.port);
  if (::inet_pton(AF_INET, options.host.c_str(), &address.sin_addr) != 1) {
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  }

  SocketHandle handle = kInvalidSocket;
  const std::uint32_t attempts = std::max<std::uint32_t>(options.connect_attempts, 1);
  for (std::uint32_t attempt = 0; attempt < attempts; ++attempt) {
    handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == kInvalidSocket) {
      return socket_error("socket");
    }
    if (::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
      break;
    }
    close_socket(handle);
    handle = kInvalidSocket;
    std::this_thread::sleep_for(std::chrono::milliseconds(options.connect_retry_ms));
  }
  if (handle == kInvalidSocket) {
    return Status(StatusCode::TransportError, "could not connect to the evidence link");
  }
  set_timeouts(handle, options.receive_poll_ms, true);
  set_timeouts(handle, 5000, false);
  impl->handle = handle;

  FrameHeader hello;
  hello.kind = FrameKind::Hello;
  hello.epoch = options.epoch;
  hello.publisher = options.publisher;
  // The publisher's incarnation travels in the frame header so the server can
  // fence a recycled publisher identity from a different boot.
  hello.publisher_boot = impl->options.publisher_boot;
  hello.publisher_ordinal = impl->options.publisher_ordinal;
  hello.sequence = 0;
  auto client = std::unique_ptr<EvidenceLinkClient>(new EvidenceLinkClient(std::move(impl)));
  const ByteBuffer hello_frame = encode_frame(hello, ByteBuffer{});
  const Status sent = write_all(handle, hello_frame.data(), hello_frame.size());
  if (!sent.ok()) {
    return sent;
  }

  InboundFrame response;
  bool closed = false;
  const Status status =
      read_frame(handle, kMaxFramePayloadBytes, client->impl_->stop, 8, response, closed);
  if (!status.ok()) {
    return status;
  }
  if (response.header.kind != FrameKind::HelloAck) {
    return Status(StatusCode::HandshakeRejected, "server did not answer the handshake");
  }
  HelloAck ack;
  const Status decoded = decode_hello_ack(response.payload, ack);
  if (!decoded.ok()) {
    return decoded;
  }
  ack_out = ack;
  if (!ack.accepted()) {
    close_socket(handle);
    client->impl_->handle = kInvalidSocket;
    return Status(ack.code, ack.detail.empty() ? "handshake rejected" : ack.detail);
  }
  client->impl_->session = ack.session_id;
  return client;
}

Result<EvidenceAck> EvidenceLinkClient::submit(const EvidenceSnapshot& snapshot) {
  if (impl_->handle == kInvalidSocket) {
    return Status(StatusCode::LinkClosed, "the link is closed");
  }
  FrameHeader header;
  header.kind = FrameKind::Evidence;
  header.session_id = impl_->session;
  header.epoch = snapshot.epoch;
  header.publisher = impl_->options.publisher;
  header.publisher_boot = impl_->options.publisher_boot;
  header.publisher_ordinal = impl_->options.publisher_ordinal;
  header.sequence = ++impl_->sequence;
  const ByteBuffer frame = encode_frame(header, codec::encode_evidence(snapshot));
  const Status sent = write_all(impl_->handle, frame.data(), frame.size());
  if (!sent.ok()) {
    return sent;
  }
  InboundFrame response;
  bool closed = false;
  const Status status =
      read_frame(impl_->handle, kMaxFramePayloadBytes, impl_->stop, 8, response, closed);
  if (!status.ok()) {
    return status;
  }
  if (response.header.kind != FrameKind::EvidenceAck) {
    if (response.header.kind == FrameKind::Error) {
      ByteReader reader(response.payload.data(), response.payload.size());
      std::uint16_t code = 0;
      std::string detail;
      if (reader.u16(code) && reader.blob(detail, kMaxFrameDetailBytes)) {
        return Status(static_cast<StatusCode>(code), detail);
      }
    }
    return Status(StatusCode::FrameInvalid, "unexpected acknowledgement frame");
  }
  EvidenceAck ack;
  const Status decoded = decode_evidence_ack(response.payload, ack);
  if (!decoded.ok()) {
    return decoded;
  }
  return ack;
}

Status EvidenceLinkClient::goodbye(bool request_shutdown) {
  if (impl_->handle == kInvalidSocket) {
    return Status::success();
  }
  FrameHeader header;
  header.kind = FrameKind::Goodbye;
  header.session_id = impl_->session;
  header.epoch = impl_->options.epoch;
  header.publisher = impl_->options.publisher;
  header.publisher_boot = impl_->options.publisher_boot;
  header.publisher_ordinal = impl_->options.publisher_ordinal;
  header.flags = request_shutdown ? kFrameFlagRequestShutdown : kFrameFlagNone;
  const ByteBuffer frame = encode_frame(header, ByteBuffer{});
  return write_all(impl_->handle, frame.data(), frame.size());
}

void EvidenceLinkClient::close() noexcept {
  if (impl_ != nullptr && impl_->handle != kInvalidSocket) {
    impl_->stop.store(true, std::memory_order_release);
#ifdef _WIN32
    shutdown(impl_->handle, SD_BOTH);
#else
    ::shutdown(impl_->handle, SHUT_RDWR);
#endif
    close_socket(impl_->handle);
    impl_->handle = kInvalidSocket;
  }
}

std::uint64_t EvidenceLinkClient::session_id() const noexcept { return impl_->session; }
SequenceNumber EvidenceLinkClient::next_sequence() const noexcept { return impl_->sequence + 1; }

// --- Raw helper used by adversarial tests -------------------------------------

struct RawLinkSession::Impl {
  SocketHandle handle{kInvalidSocket};
  std::atomic<bool> stop{false};
};

RawLinkSession::RawLinkSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
RawLinkSession::~RawLinkSession() { close(); }

Result<std::unique_ptr<RawLinkSession>> RawLinkSession::connect(const std::string& host,
                                                                std::uint16_t port) {
  if (!ensure_winsock()) {
    return Status(StatusCode::TransportError, "the socket subsystem could not be initialised");
  }
  sockaddr_in address {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  }
  SocketHandle handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == kInvalidSocket) {
    return socket_error("socket");
  }
  if (::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close_socket(handle);
    return Status(StatusCode::TransportError, "raw connect failed");
  }
  set_timeouts(handle, 2000, true);
  set_timeouts(handle, 5000, false);
  auto impl = std::make_unique<Impl>();
  impl->handle = handle;
  return std::unique_ptr<RawLinkSession>(new RawLinkSession(std::move(impl)));
}

Status RawLinkSession::send_raw(const ByteBuffer& bytes) {
  if (impl_->handle == kInvalidSocket) {
    return Status(StatusCode::LinkClosed, "the raw session is closed");
  }
  return write_all(impl_->handle, bytes.data(), bytes.size());
}

Result<ByteBuffer> RawLinkSession::receive() {
  if (impl_->handle == kInvalidSocket) {
    return Status(StatusCode::LinkClosed, "the raw session is closed");
  }
  std::uint8_t header_bytes[kFrameHeaderSize];
  std::size_t first = 0;
  while (first < kFrameHeaderSize) {
#ifdef _WIN32
    const int received = ::recv(impl_->handle, reinterpret_cast<char*>(header_bytes + first),
                                static_cast<int>(kFrameHeaderSize - first), 0);
#else
    const ssize_t received = ::recv(impl_->handle, header_bytes + first, kFrameHeaderSize - first, 0);
#endif
    if (received == 0) {
      return Status(StatusCode::LinkClosed, "the peer closed the link");
    }
    if (received < 0) {
      if (would_block()) {
        if (first == 0) {
          return Status(StatusCode::TransportError, "no frame arrived within the poll window");
        }
        continue;
      }
      return socket_error("recv");
    }
    first += static_cast<std::size_t>(received);
  }
  InboundFrame frame;
  const Status decoded =
      decode_frame_header(header_bytes, kFrameHeaderSize, frame.header, kMaxFramePayloadBytes);
  if (!decoded.ok()) {
    return decoded;
  }
  frame.payload.assign(frame.header.payload_length, 0);
  const ReadOutcome outcome =
      read_exact(impl_->handle, frame.payload.data(), frame.payload.size(), impl_->stop, 4);
  if (outcome != ReadOutcome::Complete) {
    return Status(StatusCode::Truncated, "the response payload was truncated");
  }
  const Status verified =
      verify_frame_payload(frame.header, frame.payload.data(), frame.payload.size());
  if (!verified.ok()) {
    return verified;
  }
  return frame.payload;
}

void RawLinkSession::close() noexcept {
  if (impl_ != nullptr && impl_->handle != kInvalidSocket) {
    impl_->stop.store(true, std::memory_order_release);
#ifdef _WIN32
    shutdown(impl_->handle, SD_BOTH);
#else
    ::shutdown(impl_->handle, SHUT_RDWR);
#endif
    close_socket(impl_->handle);
    impl_->handle = kInvalidSocket;
  }
}

Status send_raw_frames(const std::string& host, std::uint16_t port,
                       const std::vector<ByteBuffer>& frames, std::vector<ByteBuffer>& responses,
                       std::size_t response_limit) {
  Result<std::unique_ptr<RawLinkSession>> session = RawLinkSession::connect(host, port);
  if (!session.ok()) {
    return session.status();
  }
  std::unique_ptr<RawLinkSession>& link = session.value();
  for (const ByteBuffer& frame : frames) {
    const Status sent = link->send_raw(frame);
    if (!sent.ok()) {
      link->close();
      return sent;
    }
  }
  for (std::size_t index = 0; index < response_limit; ++index) {
    Result<ByteBuffer> payload = link->receive();
    if (!payload.ok()) {
      link->close();
      // A closed or idle peer after the expected traffic is not an error for
      // this helper: the caller decides what the responses mean.
      return Status::success();
    }
    responses.push_back(payload.take());
  }
  link->close();
  return Status::success();
}

Status raw_send(const std::string& host, std::uint16_t port, const ByteBuffer& bytes,
                ByteBuffer* response_out, std::size_t response_limit) {
  std::vector<ByteBuffer> frames;
  frames.push_back(bytes);
  std::vector<ByteBuffer> responses;
  const Status status = send_raw_frames(host, port, frames, responses, response_limit);
  if (!status.ok()) {
    return status;
  }
  if (response_out != nullptr && !responses.empty()) {
    *response_out = responses.front();
  }
  return Status::success();
}

}  // namespace fairness_governor
