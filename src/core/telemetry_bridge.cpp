// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
#include "relmw/telemetry_bridge.hpp"
#include "relmw/strategies.hpp"
#include <cstring>

namespace relmw {

std::uint32_t crc32_ieee(const void* data, std::size_t len) noexcept {
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < len; ++i) {
    crc ^= static_cast<std::uint32_t>(p[i]);
    for (int b = 0; b < 8; ++b) {
      const std::uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

void finalize_telemetry_frame(TelemetryFrame& f) noexcept {
  f.magic = kTelemetryMagic;
  f.version = kTelemetryVersion;
  f.reserved = 0u;
  f.crc32 = 0u;
  f.crc32 = crc32_ieee(&f, offsetof(TelemetryFrame, crc32));
}

bool verify_telemetry_frame(const TelemetryFrame& f) noexcept {
  if (f.magic != kTelemetryMagic || f.version != kTelemetryVersion) return false;
  const std::uint32_t expected = crc32_ieee(&f, offsetof(TelemetryFrame, crc32));
  return f.crc32 == expected;
}

std::uint8_t strategy_to_id(const char* name) noexcept {
  if (!name || !*name) return 0;
  if (std::strncmp(name, "first_try", 9) == 0) return 0;
  for (int i = 0; i < 7; ++i) {
    const std::size_t len = std::strlen(kStrategyOrder[i]);
    if (std::strncmp(name, kStrategyOrder[i], len) == 0 &&
        (name[len] == '\0' || name[len] == '(' || name[len] == '@')) {
      return static_cast<std::uint8_t>(i + 1);
    }
  }
  return 255;
}

const char* strategy_from_id(std::uint8_t id) noexcept {
  if (id == 0) return "first_try";
  if (id >= 1 && id <= 7) return kStrategyOrder[id - 1];
  return "custom";
}

TelemetryRing::TelemetryRing(std::size_t capacity) : buf_(capacity ? capacity : 1) {}

void TelemetryRing::emit(TelemetryFrame& f) noexcept {
  f.seq = seq_++;
  finalize_telemetry_frame(f);
  const std::size_t cap = buf_.size();
  buf_[head_] = f;
  head_ = (head_ + 1) % cap;
  if (size_ < cap) ++size_;
}

std::vector<TelemetryFrame> TelemetryRing::snapshot() const {
  std::vector<TelemetryFrame> out;
  out.reserve(size_);
  const std::size_t cap = buf_.size();
  const std::size_t start = (size_ == cap) ? head_ : 0;
  for (std::size_t i = 0; i < size_; ++i) {
    out.push_back(buf_[(start + i) % cap]);
  }
  return out;
}

bool TelemetryRing::save_binary(const char* path) const {
  if (!path) return false;
  std::FILE* fp = std::fopen(path, "wb");
  if (!fp) return false;
  const std::size_t cap = buf_.size();
  const std::size_t start = (size_ == cap) ? head_ : 0;
  for (std::size_t i = 0; i < size_; ++i) {
    const TelemetryFrame& f = buf_[(start + i) % cap];
    if (std::fwrite(&f, sizeof(TelemetryFrame), 1, fp) != 1) {
      std::fclose(fp);
      return false;
    }
  }
  return std::fclose(fp) == 0;
}

bool TelemetryRing::load_binary(const char* path, std::vector<TelemetryFrame>& out) {
  out.clear();
  if (!path) return false;
  std::FILE* fp = std::fopen(path, "rb");
  if (!fp) return false;
  TelemetryFrame f{};
  while (std::fread(&f, sizeof(TelemetryFrame), 1, fp) == 1) {
    if (!verify_telemetry_frame(f)) {
      std::fclose(fp);
      return false;
    }
    out.push_back(f);
  }
  const bool ok = std::feof(fp) != 0 && std::ferror(fp) == 0;
  std::fclose(fp);
  return ok;
}

}  // namespace relmw
