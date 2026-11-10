// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Host-side (POSIX) platform tests: clock, periodic timer, scheduling policy table,
// FIFO control surface and stderr logging smoke test.
#include "relmw/platform.hpp"
#include "minitest.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace relmw::platform;

static double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

static std::string read_file(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return std::string();
  std::string out;
  char buf[512];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

TEST(posix_now_ns_monotonic) {
  std::int64_t prev = now_ns();
  CHECK(prev > 0);
  for (int i = 0; i < 1000; ++i) {
    const std::int64_t cur = now_ns();
    CHECK(cur > 0);
    CHECK(cur >= prev);
    prev = cur;
  }
}

TEST(posix_periodic_timer_50_ticks) {
  const auto t0 = std::chrono::steady_clock::now();
  PeriodicTimer timer(2'000'000);  // 2 ms
  std::vector<std::int64_t> late;
  late.reserve(50);
  for (int i = 0; i < 50; ++i) {
    const std::int64_t l = timer.wait_tick();
    CHECK(l >= 0);
    late.push_back(l);
  }
  const double elapsed_ms = ms_since(t0);
  CHECK(elapsed_ms >= 95.0);
  CHECK(elapsed_ms <= 200.0);
  std::sort(late.begin(), late.end());
  const double median_ms = (double)late[late.size() / 2] / 1e6;
  CHECK(median_ms < 2.0);
}

TEST(posix_timer_interrupt) {
  PeriodicTimer timer(5'000'000);  // 5 ms
  std::thread th([&timer] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    timer.interrupt();
  });
  const auto t0 = std::chrono::steady_clock::now();
  std::int64_t r = 0;
  while (r != -1) r = timer.wait_tick();
  const double elapsed_ms = ms_since(t0);
  th.join();
  CHECK(r == -1);
  CHECK(elapsed_ms < 200.0);
}

TEST(posix_set_thread_sched) {
  std::string err;
  CHECK(!set_thread_sched("sporadic", 5, 0, 1000000LL, 1000000LL, &err));
  CHECK(err.find("SPORADIC") != std::string::npos);
  CHECK(set_thread_sched("other", 0));
}

TEST(posix_control_surface) {
  char dir_template[] = "/tmp/relmw_posix_XXXXXX";
  char* dir = mkdtemp(dir_template);
  CHECK(dir != nullptr);
  if (!dir) return;
  const std::string mount = dir;
  std::string status_val = "S1";
  std::string events_val = "E1";
  ControlHooks hooks;
  hooks.status = [&status_val] { return status_val; };
  hooks.events = [&events_val] { return events_val; };
  hooks.command = [](const std::string& line) { return "OK " + line; };

  {
    ControlSurface cs(mount, hooks);
    std::string err;
    CHECK(cs.start(&err));
    CHECK(read_file(mount + "/status") == "S1");
    CHECK(read_file(mount + "/events") == "E1");

    const std::string cmd = "fault bump\n";
    const int fd = open((mount + "/ctl").c_str(), O_WRONLY | O_NONBLOCK);
    CHECK(fd >= 0);
    if (fd >= 0) {
      const ssize_t w = write(fd, cmd.data(), cmd.size());
      CHECK(w == (ssize_t)cmd.size());
      close(fd);
    }
    bool got_reply = false;
    for (int i = 0; i < 100; ++i) {  // up to 1 s
      if (read_file(mount + "/reply").find("OK fault bump") != std::string::npos) {
        got_reply = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(got_reply);

    status_val = "S2";
    events_val = "E2";
    cs.publish();
    CHECK(read_file(mount + "/status") == "S2");
    CHECK(read_file(mount + "/events") == "E2");

    const auto t0 = std::chrono::steady_clock::now();
    cs.stop();
    CHECK(ms_since(t0) < 500.0);
    cs.stop();  // second stop() is harmless
  }

  unlink((mount + "/ctl").c_str());
  unlink((mount + "/reply").c_str());
  unlink((mount + "/status").c_str());
  unlink((mount + "/events").c_str());
  unlink((mount + "/status.tmp").c_str());
  unlink((mount + "/events.tmp").c_str());
  unlink((mount + "/reply.tmp").c_str());
  rmdir(mount.c_str());
}

TEST(posix_log_smoke) {
  log_init("t");
  log(Level::Info, "x=%d", 5);
}

TEST_MAIN()
