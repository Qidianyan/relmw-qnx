// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Unit & UDP zero-copy round-trip tests for the 128-byte TelemetryFrame bridge (telemetry_bridge.hpp).
#include "minitest.hpp"
#include "relmw/telemetry_bridge.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace relmw;

TEST(telemetry_frame_layout_and_crc32) {
  CHECK(sizeof(TelemetryFrame) == 128);
  TelemetryFrame f{};
  f.flags = kTelShieldEnabled | kTelShieldIntervened;
  f.seq = 42;
  f.episode = 1007;
  f.timestamp_ns = 123456789LL;
  f.release_latency_ns = 18500LL;
  f.pusher_xy[0] = 0.04f; f.pusher_xy[1] = -0.18f;
  f.desired_xy[0] = 0.05f; f.desired_xy[1] = -0.18f;
  f.safe_xy[0] = 0.01f; f.safe_xy[1] = -0.22f;
  f.obj_xy[0] = 0.12f; f.obj_xy[1] = -0.02f;
  f.obj_xy[2] = -0.12f; f.obj_xy[3] = -0.02f;
  f.goal_xy[0] = 0.25f; f.goal_xy[1] = 0.20f;
  f.goal_xy[2] = -0.25f; f.goal_xy[3] = 0.20f;
  f.min_clearance_m = 0.008f;
  f.compute_us = 3.2f;
  f.shield_interventions = 9;
  f.robot_reconnects = 1;
  f.shield_reason_mask = 4;
  f.arm_id = 2;
  f.strategy_id = strategy_to_id("relocalize(proactive)");
  f.failure_mode = static_cast<std::uint8_t>(FailureMode::OffAxisDrift);
  f.subtask_id = 0;
  f.attempt = 1;
  f.policy_kind = 1;
  f.tick = 150;

  finalize_telemetry_frame(f);
  CHECK(f.magic == kTelemetryMagic);
  CHECK(f.version == kTelemetryVersion);
  CHECK(verify_telemetry_frame(f));
  CHECK(std::string(strategy_from_id(f.strategy_id)) == "relocalize");

  // Single-bit corruption anywhere in payload must fail CRC-32 check
  TelemetryFrame corrupted = f;
  reinterpret_cast<std::uint8_t*>(&corrupted)[44] ^= 0x01u;
  CHECK(!verify_telemetry_frame(corrupted));
}

TEST(telemetry_ring_wraparound_and_binary_io) {
  TelemetryRing ring(4);
  for (int i = 0; i < 6; ++i) {
    TelemetryFrame f{};
    f.episode = 1000 + i;
    f.tick = static_cast<std::uint64_t>(i * 10);
    ring.emit(f);
  }
  CHECK(ring.size() == 4);
  CHECK(ring.total_emitted() == 6);
  const auto snap = ring.snapshot();
  CHECK(snap.size() == 4);
  CHECK(snap[0].episode == 1002 && snap[0].seq == 2);
  CHECK(snap[3].episode == 1005 && snap[3].seq == 5);

  char path[] = "/tmp/relmw_tel_XXXXXX";
  const int fd = mkstemp(path);
  CHECK(fd >= 0);
  if (fd >= 0) ::close(fd);
  CHECK(ring.save_binary(path));

  std::vector<TelemetryFrame> loaded;
  CHECK(TelemetryRing::load_binary(path, loaded));
  CHECK(loaded.size() == 4);
  for (std::size_t i = 0; i < 4; ++i) {
    CHECK(std::memcmp(&snap[i], &loaded[i], sizeof(TelemetryFrame)) == 0);
  }
  ::unlink(path);
}

TEST(telemetry_udp_loopback_zero_copy_datagram) {
  const int rx = ::socket(AF_INET, SOCK_DGRAM, 0);
  const int tx = ::socket(AF_INET, SOCK_DGRAM, 0);
  CHECK(rx >= 0 && tx >= 0);
  if (rx < 0 || tx < 0) return;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  CHECK(::bind(rx, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
  socklen_t alen = sizeof addr;
  CHECK(::getsockname(rx, reinterpret_cast<sockaddr*>(&addr), &alen) == 0);

  TelemetryFrame out{};
  out.episode = 2026;
  out.flags = kTelShieldEnabled | kTelRobotReconnected;
  out.pusher_xy[0] = 0.11f;
  out.pusher_xy[1] = 0.07f;
  out.tick = 77;
  finalize_telemetry_frame(out);

  const ssize_t sent = ::sendto(tx, &out, sizeof out, 0, reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
  CHECK(sent == static_cast<ssize_t>(sizeof(TelemetryFrame)));

  TelemetryFrame in{};
  const ssize_t got = ::recv(rx, &in, sizeof in, 0);
  CHECK(got == static_cast<ssize_t>(sizeof(TelemetryFrame)));
  CHECK(verify_telemetry_frame(in));
  CHECK(in.episode == 2026);
  CHECK(in.tick == 77);
  CHECK(std::memcmp(&out, &in, sizeof(TelemetryFrame)) == 0);

  ::close(tx);
  ::close(rx);
}

TEST_MAIN()
