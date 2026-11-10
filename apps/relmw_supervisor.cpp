// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// relmw_supervisor: the reliability middleware as a real-time supervisor process.
//
//  * Control loop thread: SCHED_FIFO (or SCHED_SPORADIC on QNX) priority, paced by a periodic kernel timer
//    (QNX: timer_create -> pulse -> MsgReceivePulse). Every robot tick waits for the timer; lateness of every
//    release is recorded in a histogram (p50/p99/max, deadline misses).
//  * Control surface: QNX resource manager /dev/relmw/{status,events,ctl} (Linux host: files + FIFO in /tmp/relmw).
//  * Logging: slog2 on QNX (`slog2info -b relmw`), stderr on Linux.
//  * Watchdog: optional HAM self-attach with heartbeat (--ham), plus apps/relmw_ham_setup for restart-on-death.
//  * Robot source: the in-process PushSim2D (default, bit-identical to the benchmark) or the driver process
//    relmw_robotd over the fixed-size robot link (--robot PATH): one request -> one reply, synchronous, like
//    QNX MsgSend/MsgReceive/MsgReply. If the link drops mid-episode the episode is re-run from the same seed
//    after reconnecting (process-level recovery; the task-level rollback/retry layer is untouched).
//
//   relmw_supervisor [--episodes N] [--period-us 2000] [--arm mw|naive|none] [--nominal] [--prio 40]
//                    [--sched fifo|sporadic|other] [--mount DIR] [--library FILE] [--report FILE] [--ham]
//                    [--heartbeat-ms N] [--robot PATH] [--cpu N | --cluster NAME] [--plan push_A,push_B]
#include "relmw/experiment.hpp"
#include "relmw/latency.hpp"
#include "relmw/platform.hpp"
#include "relmw/policy.hpp"
#include "relmw/robot_link.hpp"
#include "relmw/robot_link_os.hpp"
#include "relmw/verifier.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

using namespace relmw;
namespace pf = relmw::platform;

namespace {

struct Shared {
  std::mutex mu;
  // counters
  int episodes = 0, success = 0, recovered = 0, escalated = 0, failures_detected = 0;
  int robot_link_errors = 0;
  int cur_ep = 0; std::string cur_fault = "-", cur_subtask = "-", cur_strategy = "-"; int cur_attempt = 0;
  std::deque<std::string> events;     // ring buffer, newest last
  LatencyStats lat, compute;
  long ticks = 0;
  // commands
  std::string arm = "mw";
  std::string robot_desc = "in-process";    // "in-process" or the --robot PATH
  int force_fault = -2;               // -2 = random field faults, -1 = nominal, >=0 forced
  bool paused = false, knock = false;
  std::string cluster = "-";          // CPU cluster of the CONTROL thread; "-" = default affinity
  std::string affinity = "?";         // thread_affinity_text() sampled ON the control thread (status is served by
                                      // the control-surface thread, whose own affinity is not the one we report)
  std::string plan;                   // planned subtask order (CSV); empty = built-in order (default)
  std::vector<Subtask> task_proto;    // pristine built-in task; its names validate a plan (dry apply_plan)
  std::atomic<bool> quit{false};
  std::atomic<size_t> lib_entries{0};   // read by the resmgr thread; the map itself is only touched by the control thread
};

Shared S;

const char* platform_name() {
#ifdef __QNX__
  return "QNX";
#else
  return "POSIX host";
#endif
}

void push_event(const std::string& line) {
  S.events.push_back(line);
  while (S.events.size() > 64) S.events.pop_front();
}

std::string us(std::int64_t ns) { char b[32]; std::snprintf(b, sizeof b, "%.1f", ns / 1000.0); return b; }

// Watchdog heartbeat by TIME, not by tick count: beat at least every heartbeat_period/2 so that even a paused
// supervisor (main()'s paused-wait loop) keeps the watchdog alive. heartbeat_period defaults to the named
// constant kHeartbeatPeriodMs (100 ms) and is overridden by --heartbeat-ms.
constexpr std::int64_t kHeartbeatPeriodMs = 100;
std::int64_t heartbeat_period_ns = kHeartbeatPeriodMs * 1000000LL;
std::int64_t last_hb_ns = 0;

void heartbeat_if_due() {
  const std::int64_t now = pf::now_ns();
  if (now - last_hb_ns >= heartbeat_period_ns / 2) {
    last_hb_ns = now;
    pf::watchdog_heartbeat();
  }
}

std::string status_text(std::int64_t period_ns, size_t lib_entries, const std::string& library_file) {
  std::lock_guard<std::mutex> g(S.mu);
  std::ostringstream o;
  o << "relmw supervisor on " << platform_name() << "\n"
    << "arm=" << S.arm << " paused=" << (S.paused ? "yes" : "no") << "\n"
    << "robot=" << S.robot_desc << "\n"
    << "plan=" << (S.plan.empty() ? "default" : S.plan) << "\n"
    << "episodes=" << S.episodes << " success=" << S.success << " recovered=" << S.recovered
    << " failures_detected=" << S.failures_detected << " escalated=" << S.escalated << "\n"
    << "robot_link_errors=" << S.robot_link_errors << "\n"
    << "current: ep=" << S.cur_ep << " fault=" << S.cur_fault << " subtask=" << S.cur_subtask
    << " attempt=" << S.cur_attempt << " strategy=" << S.cur_strategy << "\n"
    << "control loop: period_us=" << us(period_ns) << " ticks=" << S.ticks
    << " release_latency_us p50=" << us(S.lat.quantile(0.5)) << " p99=" << us(S.lat.quantile(0.99))
    << " max=" << us(S.lat.max()) << " deadline_misses=" << S.lat.deadline_misses() << "\n"
    << "compute_per_tick_us p50=" << us(S.compute.quantile(0.5)) << " p99=" << us(S.compute.quantile(0.99))
    << " max=" << us(S.compute.max()) << "\n"
    << "control_affinity=" << S.cluster << " " << S.affinity << "\n"
    << "failure_library_entries=" << lib_entries << "\n"
    << "failure_library_file=" << (library_file.empty() ? "-" : library_file) << "\n";
  return o.str();
}

std::string events_text() {
  std::lock_guard<std::mutex> g(S.mu);
  std::string s;
  for (auto& e : S.events) s += e + "\n";
  return s;
}

std::string handle_command(const std::string& line) {
  std::istringstream is(line);
  std::string cmd, arg;
  is >> cmd >> arg;
  std::lock_guard<std::mutex> g(S.mu);
  if (cmd == "fault") {
    if (arg == "none") S.force_fault = 0;
    else if (arg == "bump") S.force_fault = 1;
    else if (arg == "percep_bias") S.force_fault = 2;
    else if (arg == "slippery") S.force_fault = 3;
    else if (arg == "random") S.force_fault = -2;
    else if (arg == "nominal") S.force_fault = -1;
    else return "ERR fault none|bump|percep_bias|slippery|random|nominal";
    push_event("CTL fault " + arg + " (from next episode)");
    return "OK fault " + arg;
  }
  if (cmd == "arm") {
    if (arg != "mw" && arg != "naive" && arg != "none") return "ERR arm mw|naive|none";
    S.arm = arg; push_event("CTL arm " + arg + " (from next episode)");
    return "OK arm " + arg;
  }
  if (cmd == "plan") {
    if (arg == "default") { S.plan.clear(); push_event("CTL plan default (from next episode)"); return "OK plan default"; }
    std::vector<std::string> items;
    std::string e;
    if (!parse_plan(arg, items, &e)) return "ERR plan: " + e;
    std::vector<Subtask> dry = S.task_proto;         // dry run against make_task's names
    if (!apply_plan(dry, items, &e)) return "ERR plan: " + e;
    S.plan = arg;
    push_event("CTL plan " + arg + " (from next episode)");
    return "OK plan " + arg;
  }
  if (cmd == "knock") { S.knock = true; return "OK knock"; }
  if (cmd == "pause") { S.paused = true; return "OK pause"; }
  if (cmd == "resume") { S.paused = false; return "OK resume"; }
  if (cmd == "reset-stats") { S.lat.reset(); S.compute.reset(); return "OK reset-stats"; }
  if (cmd == "quit") { S.quit = true; return "OK quit"; }
  return "ERR commands: fault <x> | arm <mw|naive|none> | plan <csv> | knock | pause | resume | reset-stats | quit";
}

// Real-time robot adapter: wraps any RobotAdapter (the in-process PushSim2D or a RemoteRobot over the
// driver process), paces every tick on the real-time timer, measures release latency + compute time, and
// services the knock control command. The knock and ticks callbacks adapt to whichever robot is bound.
class RealtimeRobot : public RobotAdapter {
 public:
  explicit RealtimeRobot(pf::PeriodicTimer& t) : timer_(t) {}
  void bind(RobotAdapter& dev, std::function<void()> knock_cb, std::function<long()> ticks_cb) {
    dev_ = &dev;
    knock_cb_ = std::move(knock_cb);
    ticks_cb_ = std::move(ticks_cb);
  }
  Obs get_obs() override { return dev_->get_obs(); }
  Vec2 robot_pose() override { return dev_->robot_pose(); }
  void move_robot_to(Vec2 pose, int steps) override {
    for (int k = 0; k < steps; ++k) { step(pose); if ((dev_->robot_pose() - pose).norm() < 0.01) break; }
  }
  Obs step(Vec2 target) override {
    if (last_wake_) S.compute.record(pf::now_ns() - last_wake_);   // work done since the previous release
    std::int64_t late = timer_.wait_tick();
    last_wake_ = pf::now_ns();
    if (late < 0) { S.quit = true; return dev_->get_obs(); }
    {
      std::lock_guard<std::mutex> g(S.mu);
      S.lat.record(late);
      ++S.ticks;
      if (S.knock) { S.knock = false; if (knock_cb_) knock_cb_(); }
      if (ticks_cb_) last_ticks_ = ticks_cb_();      // event log line prints the robot's tick count
    }
    heartbeat_if_due();
    return dev_->step(target);
  }
  long robot_ticks() const { return last_ticks_; }
 private:
  pf::PeriodicTimer& timer_;
  RobotAdapter* dev_ = nullptr;
  std::function<void()> knock_cb_;
  std::function<long()> ticks_cb_;
  std::int64_t last_wake_ = 0;
  long last_ticks_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  int episodes = 20, prio = 40, heartbeat_ms = kHeartbeatPeriodMs;
  std::int64_t period_ns = 2000000;
  std::string sched = "fifo", report, library, robot_path, cluster, plan_flag;
  bool ham = false;
#ifdef __QNX__
  std::string mount = "/dev/relmw";
#else
  std::string mount = "/tmp/relmw";
#endif
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); } return argv[++i]; };
    if (a == "--episodes") episodes = std::atoi(next().c_str());
    else if (a == "--period-us") period_ns = std::atoll(next().c_str()) * 1000;
    else if (a == "--arm") S.arm = next();
    else if (a == "--nominal") S.force_fault = -1;
    else if (a == "--prio") prio = std::atoi(next().c_str());
    else if (a == "--sched") sched = next();
    else if (a == "--mount") mount = next();
    else if (a == "--library") library = next();
    else if (a == "--report") report = next();
    else if (a == "--ham") ham = true;
    else if (a == "--heartbeat-ms") heartbeat_ms = std::atoi(next().c_str());
    else if (a == "--robot") robot_path = next();
    else if (a == "--cpu") cluster = "_cpu-" + next();   // validated strictly by set_thread_cluster
    else if (a == "--cluster") cluster = next();
    else if (a == "--plan") plan_flag = next();
    else { std::fprintf(stderr, "see header of apps/relmw_supervisor.cpp for usage\n"); return 2; }
  }
  const bool robot_mode = !robot_path.empty();
  heartbeat_period_ns = heartbeat_ms * 1000000LL;
  pf::log_init("relmw");
  pf::log(pf::Level::Info, "relmw_supervisor start: platform=%s period_us=%lld sched=%s prio=%d episodes=%d robot=%s",
          platform_name(), (long long)(period_ns / 1000), sched.c_str(), prio, episodes,
          robot_mode ? robot_path.c_str() : "in-process");

  CorrelationAwareRetry lib;
  if (!library.empty()) {
    if (lib.load(library)) pf::log(pf::Level::Info, "failure library loaded: %zu entries", lib.lib.size());
    else pf::log(pf::Level::Warn, "failure library %s not found; starting empty", library.c_str());
  }
  S.lib_entries = lib.lib.size();
  NaiveRetry naive;

  // The pristine built-in task; a plan is validated against make_task's names (dry apply_plan on a copy).
  {
    PushSim2D proto(0, SimConfig{});
    S.task_proto = make_task(proto, 0);
  }

  pf::ControlSurface ctl(mount, {[&] { return status_text(period_ns, S.lib_entries.load(), library); }, events_text, handle_command});
  std::string err;
  if (!ctl.start(&err)) pf::log(pf::Level::Warn, "control surface unavailable: %s", err.c_str());

  if (ham) {
    if (pf::watchdog_attach("relmw_supervisor", heartbeat_period_ns, 3, 10, &err))
      pf::log(pf::Level::Info, "HAM heartbeat attached (%lld ms)", (long long)(heartbeat_period_ns / 1000000));
    else pf::log(pf::Level::Warn, "HAM attach failed: %s", err.c_str());
  }
  std::int64_t budget = period_ns / 2;
  if (!pf::set_thread_sched(sched, prio, 10, budget, period_ns, &err))
    pf::log(pf::Level::Warn, "set_thread_sched(%s,%d) failed: %s (continuing at default priority)", sched.c_str(), prio, err.c_str());

  // Pin the CONTROL thread only (the control-surface service thread was created earlier and stays on the default
  // affinity). Warn and continue on failure.
  if (!cluster.empty()) {
    std::string ce;
    if (pf::set_thread_cluster(cluster, &ce)) S.cluster = cluster;
    else pf::log(pf::Level::Warn, "set_thread_cluster(%s) failed: %s (continuing on default affinity)",
                 cluster.c_str(), ce.c_str());
  }
  { std::lock_guard<std::mutex> g(S.mu); S.affinity = pf::thread_affinity_text(); }
  if (!plan_flag.empty()) {
    std::vector<std::string> items;
    std::string pe;
    std::vector<Subtask> dry = S.task_proto;
    if (!parse_plan(plan_flag, items, &pe) || !apply_plan(dry, items, &pe))
      pf::log(pf::Level::Warn, "--plan %s rejected: %s (using the built-in order)", plan_flag.c_str(), pe.c_str());
    else S.plan = plan_flag;
  }

  pf::PeriodicTimer timer(period_ns);
  S.lat.set_deadline(period_ns / 2);    // a release later than half a period counts as a deadline miss
  RealtimeRobot robot(timer);

  // Robot link state: the driver process (remote mode) or the in-process simulator (default).
  std::unique_ptr<RobotTransport> transport;
  std::unique_ptr<RemoteRobot> remote;
  ExperimentConfig ecfg;

  auto connect_remote = [&]() -> bool {
    std::string e;
    std::unique_ptr<RobotTransport> t = robot_connect(robot_path, &e);
    if (!t) { pf::log(pf::Level::Error, "robot_connect(%s) failed: %s", robot_path.c_str(), e.c_str()); return false; }
    try {
      remote = std::make_unique<RemoteRobot>(*t);   // hello() throws on version mismatch
    } catch (const RobotLinkError& ex) {
      pf::log(pf::Level::Error, "robot handshake(%s) failed: %s", robot_path.c_str(), ex.what());
      return false;
    }
    transport = std::move(t);
    return true;
  };

  // Reconnect after a link loss: retry every 200 ms, heartbeat still beating, honour quit.
  auto wait_robot = [&]() -> bool {
    for (;;) {
      if (connect_remote()) return true;
      for (int k = 0; k < 20; ++k) {
        if (S.quit.load()) return false;
        heartbeat_if_due();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
  };

  if (robot_mode) {
    if (!wait_robot()) { pf::log(pf::Level::Error, "no robot driver at %s; exit", robot_path.c_str()); return 1; }
    { std::lock_guard<std::mutex> g(S.mu); S.robot_desc = robot_path; }
  }

  for (int n = 0; (episodes <= 0 || n < episodes) && !S.quit; ++n) {
    while (true) { { std::lock_guard<std::mutex> g(S.mu); if (!S.paused || S.quit) break; } timer.wait_tick(); ctl.publish(); heartbeat_if_due(); }
    int ep = 1000 + n;
    SimConfig sc;
    std::string arm;
    {
      std::lock_guard<std::mutex> g(S.mu);
      sc.field_faults = S.force_fault != -1;
      if (S.force_fault >= 0) sc.force_fault = S.force_fault;
      arm = S.arm;
    }
    EpisodeReport rep;
    bool gt = false;
    Fault fault = Fault::None;
    long sim_ticks = 0;
    bool done = false;
    // The failure library learns DURING an episode. If the driver dies mid-episode, the re-run of the same seed must
    // start from the library as it was before the lost attempt, otherwise remote and in-process runs diverge.
    const auto lib_before = lib.lib;
    while (!done && !S.quit) {
      // Bind this episode's robot source: the in-process simulator, or a fresh Reset on the driver.
      std::unique_ptr<PushSim2D> sim;                 // local mode only
      RobotAdapter* dev = nullptr;
      TruthSensor* truth = nullptr;
      try {
        if (robot_mode) {
          fault = remote->reset(static_cast<std::uint64_t>(ep), sc);
          dev = remote.get();
          truth = remote.get();
        } else {
          sim = std::make_unique<PushSim2D>(ep, sc);
          fault = sim->fault();
          dev = sim.get();
          truth = sim.get();
        }
        robot.bind(*dev,
                   [&, robot_mode] {
                     if (robot_mode) remote->knock(0, Vec2{0.0, -0.10});
                     else sim->knock(0, Vec2{0.0, -0.10});
                     push_event("CTL knock: cube A displaced 10 cm");
                   },
                   [&, robot_mode]() -> long {
                     return robot_mode ? remote->last_ticks() : sim->ticks();
                   });
        ScriptedPushPolicy pol(ecfg.policy_bias_std);
        pol.new_episode(ep);
        MiddlewareConfig mc;
        mc.max_retries = arm == "none" ? 0 : 3;
        mc.seed = ep;
        RetryPolicy* rp = arm == "mw" ? (RetryPolicy*)&lib : (RetryPolicy*)&naive;
        {
          std::lock_guard<std::mutex> g(S.mu);
          S.cur_ep = ep; S.cur_fault = to_string(fault);
          push_event("EPISODE " + std::to_string(ep) + " start arm=" + arm + " fault=" + S.cur_fault);
        }
        // Reviewer fix: the middleware must drive the RealtimeRobot wrapper (timer pacing, latency histogram, knock,
        // heartbeat), never the raw device; T8 had passed *dev here, which silently bypassed the real-time loop.
        ReliabilityMiddleware mw(pol, robot, rp, mc, [&](const Event& e) {
          char line[256];
          std::snprintf(line, sizeof line, "ep=%d tick=%ld %s %s attempt=%d strategy=%s mode=%s%s", ep, robot.robot_ticks(), to_string(e.type),
                        e.subtask.c_str(), e.attempt, e.strategy.c_str(), to_string(e.mode), e.correlated ? " (correlated)" : "");
          pf::log(e.type == EventType::Escalate ? pf::Level::Warn : pf::Level::Info, "%s", line);
          std::lock_guard<std::mutex> g(S.mu);
          if (!e.subtask.empty()) S.cur_subtask = e.subtask;
          S.cur_attempt = e.attempt;
          if (!e.strategy.empty()) S.cur_strategy = e.strategy;
          push_event(line);
        });
        auto task = make_task(*truth, ep);
        {
          std::lock_guard<std::mutex> g(S.mu);
          if (!S.plan.empty()) {   // default plan: apply_plan is not called, behavior stays identical
            std::vector<std::string> items;
            std::string pe;
            if (!parse_plan(S.plan, items, &pe) || !apply_plan(task, items, &pe))
              pf::log(pf::Level::Warn, "plan \"%s\" no longer valid: %s (using the built-in order)",
                      S.plan.c_str(), pe.c_str());
          }
        }
        rep = mw.run(task);
        gt = true;
        // Ground truth scores only the planned objects: the middleware's FINAL CHECK re-verifies exactly the
        // subtasks in the (possibly filtered) task vector, so the end-of-episode score must match the plan.
        for (int i = 0; i < kNumObjects; ++i) {
          bool wanted = false;
          for (auto& st : task) if (st.name == std::string("push_") + object_name(i)) wanted = true;
          if (wanted) gt = gt && oracle_verdict(truth->truth(), i).ok;
        }
        sim_ticks = robot.robot_ticks();
        done = true;
      } catch (const RobotLinkError& e) {
        // Process-level recovery: the driver process died with the robot state. Do not count the episode,
        // reconnect, and re-run the SAME episode seed from the start (task-level recovery is the
        // middleware's own rollback/retry, which stays untouched).
        pf::log(pf::Level::Error, "robot link lost: %s", e.what());
        {
          std::lock_guard<std::mutex> g(S.mu);
          push_event("ROBOT LINK LOST");
          ++S.robot_link_errors;
        }
        lib.lib = lib_before;       // undo what the lost attempt taught the library
        remote.reset();
        transport.reset();
        if (!wait_robot()) break;   // quit during reconnect
      }
    }
    if (S.quit) break;
    {
      std::lock_guard<std::mutex> g(S.mu);
      ++S.episodes; S.success += gt; S.recovered += (gt && rep.n_failures() > 0);
      S.failures_detected += rep.n_failures() > 0; S.escalated += rep.abort_reason == "escalate_to_operator";
      push_event("EPISODE " + std::to_string(ep) + (gt ? " SUCCESS" : " FAILED (" + rep.abort_reason + ")"));
    }
    pf::log(pf::Level::Info, "episode %d fault=%s arm=%s -> %s (failures=%d, ticks=%ld)", ep, to_string(fault), arm.c_str(),
            gt ? "SUCCESS" : "FAILED", rep.n_failures(), sim_ticks);
    if (!library.empty() && arm == "mw") {  // persist the failure library atomically: tmp write + rename
      const std::string tmp = library + ".tmp";
      if (!lib.save(tmp)) pf::log(pf::Level::Warn, "failed to save failure library to %s", tmp.c_str());
      else if (std::rename(tmp.c_str(), library.c_str()) != 0)
        pf::log(pf::Level::Warn, "failed to install failure library %s: %s", library.c_str(), std::strerror(errno));
    }
    S.lib_entries = lib.lib.size();
    ctl.publish();
  }
  std::string st = status_text(period_ns, S.lib_entries.load(), library);
  std::printf("%s", st.c_str());
  if (!report.empty()) {
    std::lock_guard<std::mutex> g(S.mu);
    std::ofstream f(report);
    f << "{\"platform\":\"" << platform_name() << "\",\"arm\":\"" << S.arm << "\",\"period_ns\":" << period_ns
      << ",\"sched\":\"" << sched << "\",\"priority\":" << prio << ",\"cluster\":\"" << S.cluster
      << "\",\"episodes\":" << S.episodes << ",\"success\":" << S.success
      << ",\"recovered\":" << S.recovered << ",\"escalated\":" << S.escalated << ",\"ticks\":" << S.ticks
      << ",\"release_latency\":" << S.lat.to_json() << ",\"compute_per_tick\":" << S.compute.to_json() << "}\n";
  }
  ctl.stop();
  pf::log(pf::Level::Info, "relmw_supervisor exit");
  return 0;
}
