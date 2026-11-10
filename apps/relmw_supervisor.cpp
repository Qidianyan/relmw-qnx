// relmw_supervisor: the reliability middleware as a real-time supervisor process.
//
//  * Control loop thread: SCHED_FIFO (or SCHED_SPORADIC on QNX) priority, paced by a periodic kernel timer
//    (QNX: timer_create -> pulse -> MsgReceivePulse). Every robot tick waits for the timer; lateness of every
//    release is recorded in a histogram (p50/p99/max, deadline misses).
//  * Control surface: QNX resource manager /dev/relmw/{status,events,ctl} (Linux host: files + FIFO in /tmp/relmw).
//  * Logging: slog2 on QNX (`slog2info -b relmw`), stderr on Linux.
//  * Watchdog: optional HAM self-attach with heartbeat (--ham), plus apps/relmw_ham_setup for restart-on-death.
//
//   relmw_supervisor [--episodes N] [--period-us 2000] [--arm mw|naive|none] [--nominal] [--prio 40]
//                    [--sched fifo|sporadic|other] [--mount DIR] [--library FILE] [--report FILE] [--ham]
#include "relmw/experiment.hpp"
#include "relmw/latency.hpp"
#include "relmw/platform.hpp"
#include "relmw/policy.hpp"
#include "relmw/verifier.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

using namespace relmw;
namespace pf = relmw::platform;

namespace {

struct Shared {
  std::mutex mu;
  // counters
  int episodes = 0, success = 0, recovered = 0, escalated = 0, failures_detected = 0;
  int cur_ep = 0; std::string cur_fault = "-", cur_subtask = "-", cur_strategy = "-"; int cur_attempt = 0;
  std::deque<std::string> events;     // ring buffer, newest last
  LatencyStats lat, compute;
  long ticks = 0;
  // commands
  std::string arm = "mw";
  int force_fault = -2;               // -2 = random field faults, -1 = nominal, >=0 forced
  bool paused = false, knock = false;
  std::atomic<bool> quit{false};
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

std::string status_text(std::int64_t period_ns, size_t lib_entries) {
  std::lock_guard<std::mutex> g(S.mu);
  std::ostringstream o;
  o << "relmw supervisor on " << platform_name() << "\n"
    << "arm=" << S.arm << " paused=" << (S.paused ? "yes" : "no") << "\n"
    << "episodes=" << S.episodes << " success=" << S.success << " recovered=" << S.recovered
    << " failures_detected=" << S.failures_detected << " escalated=" << S.escalated << "\n"
    << "current: ep=" << S.cur_ep << " fault=" << S.cur_fault << " subtask=" << S.cur_subtask
    << " attempt=" << S.cur_attempt << " strategy=" << S.cur_strategy << "\n"
    << "control loop: period_us=" << us(period_ns) << " ticks=" << S.ticks
    << " release_latency_us p50=" << us(S.lat.quantile(0.5)) << " p99=" << us(S.lat.quantile(0.99))
    << " max=" << us(S.lat.max()) << " deadline_misses=" << S.lat.deadline_misses() << "\n"
    << "compute_per_tick_us p50=" << us(S.compute.quantile(0.5)) << " p99=" << us(S.compute.quantile(0.99))
    << " max=" << us(S.compute.max()) << "\n"
    << "failure_library_entries=" << lib_entries << "\n";
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
  if (cmd == "knock") { S.knock = true; return "OK knock"; }
  if (cmd == "pause") { S.paused = true; return "OK pause"; }
  if (cmd == "resume") { S.paused = false; return "OK resume"; }
  if (cmd == "reset-stats") { S.lat.reset(); S.compute.reset(); return "OK reset-stats"; }
  if (cmd == "quit") { S.quit = true; return "OK quit"; }
  return "ERR commands: fault <x> | arm <mw|naive|none> | knock | pause | resume | reset-stats | quit";
}

// Robot adapter that paces every tick on the real-time timer and measures release latency + compute time.
class RealtimeRobot : public RobotAdapter {
 public:
  RealtimeRobot(pf::PeriodicTimer& t, bool ham) : timer_(t), ham_(ham) {}
  PushSim2D* sim = nullptr;
  Obs get_obs() override { return sim->get_obs(); }
  Vec2 robot_pose() override { return sim->robot_pose(); }
  void move_robot_to(Vec2 pose, int steps) override {
    for (int k = 0; k < steps; ++k) { step(pose); if ((sim->robot_pose() - pose).norm() < 0.01) break; }
  }
  Obs step(Vec2 target) override {
    if (last_wake_) S.compute.record(pf::now_ns() - last_wake_);   // work done since the previous release
    std::int64_t late = timer_.wait_tick();
    last_wake_ = pf::now_ns();
    if (late < 0) { S.quit = true; return sim->get_obs(); }
    {
      std::lock_guard<std::mutex> g(S.mu);
      S.lat.record(late);
      ++S.ticks;
      if (S.knock) { S.knock = false; sim->knock(0, Vec2{0.0, -0.10}); push_event("CTL knock: cube A displaced 10 cm"); }
    }
    if (ham_ && (++beats_ % 50 == 0)) pf::watchdog_heartbeat();
    return sim->step(target);
  }
 private:
  pf::PeriodicTimer& timer_;
  bool ham_;
  std::int64_t last_wake_ = 0;
  long beats_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  int episodes = 20, prio = 40;
  std::int64_t period_ns = 2000000;
  std::string sched = "fifo", report, library;
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
    else { std::fprintf(stderr, "see header of apps/relmw_supervisor.cpp for usage\n"); return 2; }
  }
  pf::log_init("relmw");
  pf::log(pf::Level::Info, "relmw_supervisor start: platform=%s period_us=%lld sched=%s prio=%d episodes=%d",
          platform_name(), (long long)(period_ns / 1000), sched.c_str(), prio, episodes);

  CorrelationAwareRetry lib;
  if (!library.empty()) {
    if (lib.load(library)) pf::log(pf::Level::Info, "failure library loaded: %zu entries", lib.lib.size());
    else pf::log(pf::Level::Warn, "failure library %s not found; starting empty", library.c_str());
  }
  NaiveRetry naive;

  pf::ControlSurface ctl(mount, {[&] { return status_text(period_ns, lib.lib.size()); }, events_text, handle_command});
  std::string err;
  if (!ctl.start(&err)) pf::log(pf::Level::Warn, "control surface unavailable: %s", err.c_str());

  if (ham) {
    if (pf::watchdog_attach("relmw_supervisor", 100000000LL, 3, 10, &err)) pf::log(pf::Level::Info, "HAM heartbeat attached (100 ms)");
    else pf::log(pf::Level::Warn, "HAM attach failed: %s", err.c_str());
  }
  std::int64_t budget = period_ns / 2;
  if (!pf::set_thread_sched(sched, prio, 10, budget, period_ns, &err))
    pf::log(pf::Level::Warn, "set_thread_sched(%s,%d) failed: %s (continuing at default priority)", sched.c_str(), prio, err.c_str());

  pf::PeriodicTimer timer(period_ns);
  S.lat.set_deadline(period_ns / 2);    // a release later than half a period counts as a deadline miss
  RealtimeRobot robot(timer, ham);
  ExperimentConfig ecfg;

  for (int n = 0; (episodes <= 0 || n < episodes) && !S.quit; ++n) {
    while (true) { { std::lock_guard<std::mutex> g(S.mu); if (!S.paused || S.quit) break; } timer.wait_tick(); ctl.publish(); }
    int ep = 1000 + n;
    SimConfig sc;
    std::string arm;
    {
      std::lock_guard<std::mutex> g(S.mu);
      sc.field_faults = S.force_fault != -1;
      if (S.force_fault >= 0) sc.force_fault = S.force_fault;
      arm = S.arm;
    }
    PushSim2D sim(ep, sc);
    robot.sim = &sim;
    ScriptedPushPolicy pol(ecfg.policy_bias_std);
    pol.new_episode(ep);
    MiddlewareConfig mc;
    mc.max_retries = arm == "none" ? 0 : 3;
    mc.seed = ep;
    RetryPolicy* rp = arm == "mw" ? (RetryPolicy*)&lib : (RetryPolicy*)&naive;
    {
      std::lock_guard<std::mutex> g(S.mu);
      S.cur_ep = ep; S.cur_fault = to_string(sim.fault());
      push_event("EPISODE " + std::to_string(ep) + " start arm=" + arm + " fault=" + S.cur_fault);
    }
    ReliabilityMiddleware mw(pol, robot, rp, mc, [&](const Event& e) {
      char line[256];
      std::snprintf(line, sizeof line, "ep=%d tick=%ld %s %s attempt=%d strategy=%s mode=%s%s", ep, sim.ticks(), to_string(e.type),
                    e.subtask.c_str(), e.attempt, e.strategy.c_str(), to_string(e.mode), e.correlated ? " (correlated)" : "");
      pf::log(e.type == EventType::Escalate ? pf::Level::Warn : pf::Level::Info, "%s", line);
      std::lock_guard<std::mutex> g(S.mu);
      if (!e.subtask.empty()) S.cur_subtask = e.subtask;
      S.cur_attempt = e.attempt;
      if (!e.strategy.empty()) S.cur_strategy = e.strategy;
      push_event(line);
    });
    auto task = make_task(sim, ep);
    EpisodeReport rep = mw.run(task);
    bool gt = true;
    for (int i = 0; i < kNumObjects; ++i) gt = gt && oracle_verdict(sim.true_state(), i).ok;
    {
      std::lock_guard<std::mutex> g(S.mu);
      ++S.episodes; S.success += gt; S.recovered += (gt && rep.n_failures() > 0);
      S.failures_detected += rep.n_failures() > 0; S.escalated += rep.abort_reason == "escalate_to_operator";
      push_event("EPISODE " + std::to_string(ep) + (gt ? " SUCCESS" : " FAILED (" + rep.abort_reason + ")"));
    }
    pf::log(pf::Level::Info, "episode %d fault=%s arm=%s -> %s (failures=%d, ticks=%ld)", ep, to_string(sim.fault()), arm.c_str(),
            gt ? "SUCCESS" : "FAILED", rep.n_failures(), sim.ticks());
    ctl.publish();
  }
  std::string st = status_text(period_ns, lib.lib.size());
  std::printf("%s", st.c_str());
  if (!report.empty()) {
    std::lock_guard<std::mutex> g(S.mu);
    std::ofstream f(report);
    f << "{\"platform\":\"" << platform_name() << "\",\"arm\":\"" << S.arm << "\",\"period_ns\":" << period_ns
      << ",\"sched\":\"" << sched << "\",\"priority\":" << prio << ",\"episodes\":" << S.episodes << ",\"success\":" << S.success
      << ",\"recovered\":" << S.recovered << ",\"escalated\":" << S.escalated << ",\"ticks\":" << S.ticks
      << ",\"release_latency\":" << S.lat.to_json() << ",\"compute_per_tick\":" << S.compute.to_json() << "}\n";
  }
  ctl.stop();
  pf::log(pf::Level::Info, "relmw_supervisor exit");
  return 0;
}
