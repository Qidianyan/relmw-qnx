// POSIX (Linux host) logging: one line per call on stderr, prefixed with the monotonic
// timestamp in seconds since the first log_init(). A single mutex guards both the name
// and the timestamp origin, so log_init()/log() are safe to call from any thread.
#define _POSIX_C_SOURCE 200809L
#include "relmw/platform.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace relmw::platform {
namespace {
std::mutex g_mu;
std::string g_name = "relmw";  // before log_init() the name is "relmw"
std::int64_t g_origin = 0;
bool g_have_origin = false;

const char* level_name(Level lvl) {
  switch (lvl) {
    case Level::Debug: return "DEBUG";
    case Level::Info:  return "INFO";
    case Level::Warn:  return "WARN";
    case Level::Error: return "ERROR";
  }
  return "?";
}
}  // namespace

void log_init(const char* buffer_name) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_name = buffer_name ? buffer_name : "relmw";
  if (!g_have_origin) {
    g_origin = now_ns();
    g_have_origin = true;
  }
}

void log(Level lvl, const char* fmt, ...) {
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  for (char* p = msg; *p; ++p) {
    if (*p == '\n') *p = ' ';  // exactly one line per call
  }
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_have_origin) {
    g_origin = now_ns();
    g_have_origin = true;
  }
  const double secs = (double)(now_ns() - g_origin) / 1e9;
  std::fprintf(stderr, "[relmw %s] %.6f %s %s\n", g_name.c_str(), secs, level_name(lvl), msg);
}

}  // namespace relmw::platform
