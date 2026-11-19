// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Fixed-size 128-byte zero-copy binary telemetry frame & ring/file exporter for ROS 2 / UDP / Replay Dashboard.
//
// Wire layout is strictly fixed at 128 bytes (standard layout, trivially copyable, naturally aligned on 32/64-bit
// x86_64 and ARM64) with IEEE 802.3 CRC-32 integrity protection over the first 124 bytes.
// Emitting a frame into the pre-allocated ring buffer (or non-blocking UDP socket) performs ZERO heap allocations.
#pragma once
#include "relmw/types.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>
#include <vector>

namespace relmw {

constexpr std::uint32_t kTelemetryMagic = 0x524D5754u;   // "RMWT"
constexpr std::uint16_t kTelemetryVersion = 1u;

enum TelemetryFlags : std::uint16_t {
  kTelNone              = 0u,
  kTelShieldEnabled     = 1u << 0,
  kTelShieldIntervened  = 1u << 1,
  kTelCorrelatedFail    = 1u << 2,
  kTelRobotLinkLost     = 1u << 3,
  kTelRobotReconnected  = 1u << 4,
  kTelSubtaskSuccess    = 1u << 5,
  kTelNoGoIntrusion     = 1u << 6,
  kTelEpisodeDone       = 1u << 7,
};

struct alignas(8) TelemetryFrame {
  std::uint32_t magic;                //   0..3   : kTelemetryMagic (0x524D5754)
  std::uint16_t version;              //   4..5   : kTelemetryVersion (1)
  std::uint16_t flags;                //   6..7   : TelemetryFlags bitmask
  std::uint32_t seq;                  //   8..11  : monotonic frame sequence
  std::int32_t  episode;              //  12..15  : episode seed / id
  std::int64_t  timestamp_ns;         //  16..23  : monotonic timestamp (ns)
  std::int64_t  release_latency_ns;   //  24..31  : 1 kHz timer release jitter (ns)
  float         pusher_xy[2];         //  32..39  : true/estimated robot end-effector pose (m)
  float         desired_xy[2];        //  40..47  : raw policy target before Simplex shield (m)
  float         safe_xy[2];           //  48..55  : safe target after Control Barrier Projection (m)
  float         obj_xy[4];            //  56..71  : obj[0].xy, obj[1].xy (m)
  float         goal_xy[4];           //  72..87  : goal[0].xy, goal[1].xy (m)
  float         min_clearance_m;      //  88..91  : signed clearance to nearest No-Go boundary (m)
  float         compute_us;           //  92..95  : control tick compute duration (us)
  std::uint32_t shield_interventions; //  96..99  : cumulative SHIELD_INTERVENTION pulse count
  std::uint32_t robot_reconnects;     // 100..103 : cumulative relmw_robotd process recoveries
  std::uint16_t shield_reason_mask;   // 104..105 : ShieldReasonBits bitmask for this tick
  std::uint8_t  arm_id;               // 106      : 0=SinglePass, 1=NaiveRetry, 2=CorrAware
  std::uint8_t  strategy_id;          // 107      : 0=first_try, 1..7=kStrategyOrder index+1
  std::uint8_t  failure_mode;         // 108      : FailureMode enum
  std::uint8_t  subtask_id;           // 109      : object index (0=A, 1=B)
  std::uint8_t  attempt;              // 110      : retry attempt index
  std::uint8_t  policy_kind;          // 111      : 0=Scripted, 1=TinyMLP_FP32, 2=TinyMLP_Q12
  std::uint64_t tick;                 // 112..119 : simulation / control tick index
  std::uint32_t reserved;             // 120..123 : zero-filled future extension
  std::uint32_t crc32;                // 124..127 : IEEE 802.3 CRC-32 of bytes [0..123]
};

static_assert(std::is_trivially_copyable<TelemetryFrame>::value, "TelemetryFrame must be trivially copyable");
static_assert(std::is_standard_layout<TelemetryFrame>::value, "TelemetryFrame must be standard layout");
static_assert(sizeof(TelemetryFrame) == 128, "TelemetryFrame wire size must be exact 128 bytes");
static_assert(offsetof(TelemetryFrame, magic) == 0, "magic at 0");
static_assert(offsetof(TelemetryFrame, timestamp_ns) == 16, "timestamp_ns at 16");
static_assert(offsetof(TelemetryFrame, pusher_xy) == 32, "pusher_xy at 32");
static_assert(offsetof(TelemetryFrame, obj_xy) == 56, "obj_xy at 56");
static_assert(offsetof(TelemetryFrame, shield_interventions) == 96, "shield_interventions at 96");
static_assert(offsetof(TelemetryFrame, arm_id) == 106, "arm_id at 106");
static_assert(offsetof(TelemetryFrame, tick) == 112, "tick at 112");
static_assert(offsetof(TelemetryFrame, crc32) == 124, "crc32 at 124");

// Allocation-free IEEE 802.3 CRC-32 over arbitrary bytes.
std::uint32_t crc32_ieee(const void* data, std::size_t len) noexcept;

// Stamp magic, version, and crc32 onto `f`.
void finalize_telemetry_frame(TelemetryFrame& f) noexcept;

// Verify magic, version, and crc32 of `f`.
bool verify_telemetry_frame(const TelemetryFrame& f) noexcept;

// Map strategy name ("first_try", "replay", "relocalize", ...) to compact 1-byte id.
std::uint8_t strategy_to_id(const char* name) noexcept;
const char* strategy_from_id(std::uint8_t id) noexcept;

// Pre-allocated zero-allocation ring buffer + binary file serializer for 128-byte TelemetryFrames.
class TelemetryRing {
 public:
  explicit TelemetryRing(std::size_t capacity = 4096);

  // O(1), ZERO heap allocation: stamps seq + CRC-32 and writes into the pre-sized ring.
  void emit(TelemetryFrame& f) noexcept;

  void clear() noexcept { head_ = 0; size_ = 0; seq_ = 0; }
  std::size_t size() const noexcept { return size_; }
  std::size_t capacity() const noexcept { return buf_.size(); }
  std::uint32_t total_emitted() const noexcept { return seq_; }

  // Copy chronological frames out (off real-time path, e.g. at episode / benchmark end).
  std::vector<TelemetryFrame> snapshot() const;

  // Save/load binary stream of raw 128-byte TelemetryFrame records.
  bool save_binary(const char* path) const;
  static bool load_binary(const char* path, std::vector<TelemetryFrame>& out);

 private:
  std::vector<TelemetryFrame> buf_;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
  std::uint32_t seq_ = 0;
};

}  // namespace relmw
