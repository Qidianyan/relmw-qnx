// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// slog2 logging (QNX 8.0 discontinued syslogd; slogger2 is the system logger). View with: slog2info -b relmw
#ifdef __QNX__
#include "relmw/platform.hpp"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/slog2.h>

namespace relmw::platform {

static slog2_buffer_t g_buf = nullptr;

void log_init(const char* buffer_name) {
  slog2_buffer_set_config_t cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.buffer_set_name = buffer_name;
  cfg.num_buffers = 1;
  cfg.verbosity_level = SLOG2_DEBUG1;
  cfg.buffer_config[0].buffer_name = buffer_name;
  cfg.buffer_config[0].num_pages = 8;
  if (slog2_register(&cfg, &g_buf, 0) == -1) {
    g_buf = nullptr;
    fprintf(stderr, "slog2_register failed; logging to stderr\n");
  }
}

void log(Level lvl, const char* fmt, ...) {
  uint8_t sev = lvl == Level::Debug ? SLOG2_DEBUG1 : lvl == Level::Info ? SLOG2_INFO : lvl == Level::Warn ? SLOG2_WARNING : SLOG2_ERROR;
  va_list ap;
  va_start(ap, fmt);
  if (g_buf) {
    vslog2f(g_buf, 0, sev, fmt, ap);
  } else {
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
  }
  va_end(ap);
}

}  // namespace relmw::platform
#endif  // __QNX__
