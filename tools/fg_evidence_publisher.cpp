// Fairness Governor - evidence publisher (and adversarial transport driver).
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "fairness_governor/fairness_governor.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace fairness_governor;

[[nodiscard]] bool read_file(const std::string& path, std::string& out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return false;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  out = buffer.str();
  return true;
}

/// Sends arbitrary bytes on a raw socket, used for the malformed-traffic modes.
void raw_send(const std::string& host, std::uint16_t port, const std::uint8_t* data,
              std::size_t size) {
#ifdef _WIN32
  WSADATA wsa {};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    std::cerr << "WSAStartup failed\n";
    return;
  }
  SOCKET handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == INVALID_SOCKET) {
    return;
  }
  sockaddr_in address {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  inet_pton(AF_INET, host.c_str(), &address.sin_addr);
  if (::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    closesocket(handle);
    return;
  }
  std::size_t offset = 0;
  while (offset < size) {
    const int sent = ::send(handle, reinterpret_cast<const char*>(data + offset),
                            static_cast<int>(size - offset), 0);
    if (sent <= 0) {
      break;
    }
    offset += static_cast<std::size_t>(sent);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  closesocket(handle);
#else
  const int handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle < 0) {
    return;
  }
  sockaddr_in address {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  inet_pton(AF_INET, host.c_str(), &address.sin_addr);
  if (::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(handle);
    return;
  }
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t sent = ::send(handle, data + offset, size - offset, 0);
    if (sent <= 0) {
      break;
    }
    offset += static_cast<std::size_t>(sent);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  ::close(handle);
#endif
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  std::string scenario_path;
  std::uint32_t port = 0;
  std::uint64_t epoch_value = 0;
  std::uint64_t publisher_value = 0;
  std::string mode = "normal";
  bool announce_split = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--host" && i + 1 < argc) {
      host = argv[++i];
    } else if (argument == "--port" && i + 1 < argc) {
      port = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (argument == "--epoch" && i + 1 < argc) {
      epoch_value = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--publisher" && i + 1 < argc) {
      publisher_value = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--scenario" && i + 1 < argc) {
      scenario_path = argv[++i];
    } else if (argument == "--mode" && i + 1 < argc) {
      mode = argv[++i];
    } else if (argument == "--announce-split") {
      announce_split = true;
    } else {
      std::cerr << "unknown option: " << argument << "\n";
      return 2;
    }
  }
  if (port == 0 || epoch_value == 0 || publisher_value == 0) {
    std::cerr << "usage: fg_evidence_publisher --port <n> --epoch <n> --publisher <n>"
                 " [--scenario <file>] [--mode normal|split|short|badsum|shutdown|oversize]\n";
    return 2;
  }

  Scenario scenario;
  if (!scenario_path.empty()) {
    std::string text;
    if (!read_file(scenario_path, text)) {
      std::cerr << "cannot read scenario: " << scenario_path << "\n";
      return 2;
    }
    Result<Scenario> parsed = parse_scenario(text);
    if (!parsed.ok()) {
      std::cerr << "scenario rejected: " << parsed.status().to_string() << "\n";
      return 2;
    }
    scenario = parsed.take();
  }

  const FabricEpoch epoch = FabricEpoch::from_value(epoch_value);
  const PublisherId publisher = PublisherId::from_value(publisher_value);

  if (mode == "short" || mode == "badsum" || mode == "oversize") {
    // Craft malformed traffic directly so the server sees bytes no client would
    // ever produce.
    const ByteBuffer payload = codec::encode_evidence(
        scenario.evidence.empty() ? EvidenceSnapshot{} : scenario.evidence.front());
    FrameHeader header;
    header.kind = FrameKind::Evidence;
    header.epoch = epoch;
    header.publisher = publisher;
    header.sequence = 1;
    ByteBuffer frame = encode_frame(header, payload);
    if (mode == "badsum") {
      frame[kFrameHeaderSize] ^= 0xFFu;
      frame[kFrameHeaderSize + 1] ^= 0x5Au;
    } else if (mode == "short") {
      frame.resize(frame.size() / 2);
    } else {
      store_le<std::uint32_t>(frame.data() + 12, kMaxFramePayloadBytes);
    }
    raw_send(host, static_cast<std::uint16_t>(port), frame.data(), frame.size());
    std::cout << "FG-PUB-RAW mode=" << mode << " bytes=" << frame.size() << "\n";
    std::cout.flush();
    return 0;
  }

  LinkClientOptions options;
  options.host = host;
  options.port = static_cast<std::uint16_t>(port);
  options.epoch = epoch;
  options.publisher = publisher;
  HelloAck ack;
  Result<std::unique_ptr<EvidenceLinkClient>> connected =
      EvidenceLinkClient::connect(options, ack);
  if (!connected.ok()) {
    std::cout << "FG-PUB-HANDSHAKE status=" << to_string(connected.code()) << " detail=\""
              << connected.status().detail() << "\"\n";
    std::cout.flush();
    return 3;
  }
  std::cout << "FG-PUB-HANDSHAKE status=OK session=" << ack.session_id << "\n";
  std::cout.flush();
  std::unique_ptr<EvidenceLinkClient>& client = connected.value();

  if (mode == "shutdown") {
    const Status sent = client->goodbye(true);
    std::cout << "FG-PUB-GOODBYE status=" << to_string(sent.code()) << "\n";
    std::cout.flush();
    client->close();
    return sent.ok() ? 0 : 4;
  }

  if (mode == "split") {
    // Write a partial frame header and then block. The parent kills this
    // process mid-frame, which is exactly the truncation the server must reject.
    FrameHeader header;
    header.kind = FrameKind::Evidence;
    header.session_id = client->session_id();
    header.epoch = epoch;
    header.publisher = publisher;
    header.sequence = client->next_sequence();
    const ByteBuffer full = encode_frame(header, ByteBuffer{});
    raw_send(host, static_cast<std::uint16_t>(port), full.data(), kFrameHeaderSize / 2);
    if (announce_split) {
      std::cout << "FG-PUB-SPLIT bytes=" << (kFrameHeaderSize / 2) << "\n";
      std::cout.flush();
    }
    std::this_thread::sleep_for(std::chrono::hours(24));
    return 0;
  }

  std::size_t accepted = 0;
  std::size_t rejected = 0;
  for (const EvidenceSnapshot& snapshot : scenario.evidence) {
    Result<EvidenceAck> submitted = client->submit(snapshot);
    if (!submitted.ok()) {
      ++rejected;
      std::cout << "FG-PUB-ACK status=" << to_string(submitted.code()) << " snapshot="
                << snapshot.id.value() << " generation=" << snapshot.generation.value()
                << " detail=\"" << submitted.status().detail() << "\"\n";
      continue;
    }
    const EvidenceAck& result = submitted.value();
    if (result.accepted()) {
      ++accepted;
    } else {
      ++rejected;
    }
    std::cout << "FG-PUB-ACK status=" << to_string(result.code)
              << " snapshot=" << result.snapshot.value()
              << " generation=" << result.generation.value()
              << " disposition=" << static_cast<int>(result.disposition) << " detail=\""
              << result.detail << "\"\n";
    std::cout.flush();
  }
  std::cout << "FG-PUB-DONE accepted=" << accepted << " rejected=" << rejected << "\n";
  std::cout.flush();
  (void)client->goodbye(false);
  client->close();
  return 0;
}
