// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Checklist C6: robot driver link tests.
//   * wire-protocol layout (sizes / offsets, trivially copyable, standard layout)
//   * server-side validation: short message, wrong mgrid, wrong version, bad op, bad object index,
//     NaN target -> EINVAL/EPROTO, simulator state provably unchanged
//   * ops before Reset -> ENODEV; seq echo (RemoteRobot throws RobotLinkError on mismatch)
//   * EQUIVALENCE: for seeds 1000..1029, field faults on, all 3 arms, the middleware run over an
//     in-process PushSim2D and over a RemoteRobot (LoopbackTransport) produce identical
//     EpisodeReport, identical true_state bits and identical ticks.
#include "minitest.hpp"
#include "relmw/experiment.hpp"
#include "relmw/policy.hpp"
#include "relmw/robot_link.hpp"
#include "relmw/robot_proto.hpp"
#include "relmw/retry_policy.hpp"
#include "relmw/sim2d.hpp"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>

using namespace relmw;

namespace {

bool same_report(const EpisodeReport& a, const EpisodeReport& b) {
  if (a.success != b.success || a.abort_reason != b.abort_reason || a.failed_at != b.failed_at) return false;
  if (a.attempts.size() != b.attempts.size()) return false;
  for (size_t k = 0; k < a.attempts.size(); ++k) {
    const Attempt& x = a.attempts[k];
    const Attempt& y = b.attempts[k];
    if (x.subtask != y.subtask || x.attempt != y.attempt || x.ok != y.ok || x.mode != y.mode ||
        x.steps != y.steps || x.strategy != y.strategy || x.correlated != y.correlated ||
        x.predicted_correlated != y.predicted_correlated || x.gt_ok != y.gt_ok ||
        x.has_end_obj != y.has_end_obj || x.end_obj.x != y.end_obj.x || x.end_obj.y != y.end_obj.y)
      return false;
  }
  return true;
}

// Corrupts the seq echo (once `corrupt` is set) so RemoteRobot must raise RobotLinkError(EPROTO).
class BadSeqTransport : public RobotTransport {
 public:
  explicit BadSeqTransport(RobotServer& s) : srv_(s) {}
  int call(const RobotRequest& req, RobotReply& rep) override {
    srv_.handle(req, sizeof req, rep);
    if (corrupt) rep.seq = rep.seq + 1;
    return 0;
  }
  RobotServer& srv_;
  bool corrupt = false;
};

// Transport that works until `fail` is set, then reports EPIPE (like a driver process that died).
class FailTransport : public RobotTransport {
 public:
  explicit FailTransport(RobotServer& s) : srv_(s) {}
  int call(const RobotRequest& req, RobotReply& rep) override {
    if (fail) return EPIPE;
    srv_.handle(req, sizeof req, rep);
    return 0;
  }
  RobotServer& srv_;
  bool fail = false;
};

}  // namespace

TEST(robot_proto_layout) {
  CHECK(sizeof(ProtoHdr) == 8);
  CHECK(sizeof(RobotRequest) == 64);
  CHECK(sizeof(RobotReply) == 88);
  CHECK(offsetof(RobotRequest, hdr) == 0);
  CHECK(offsetof(RobotRequest, version) == 8);
  CHECK(offsetof(RobotRequest, seq) == 12);
  CHECK(offsetof(RobotRequest, i) == 16);
  CHECK(offsetof(RobotRequest, d) == 32);
  CHECK(offsetof(RobotReply, status) == 0);
  CHECK(offsetof(RobotReply, seq) == 4);
  CHECK(offsetof(RobotReply, obs) == 8);
  CHECK(offsetof(RobotReply, pose) == 56);
  CHECK(offsetof(RobotReply, ticks) == 72);
  CHECK(offsetof(RobotReply, fault) == 80);
  CHECK(offsetof(RobotReply, pad) == 84);
  const RobotRequest r = make_request(RobotOp::Step, 7);
  CHECK(r.hdr.type == 0);
  CHECK(r.hdr.combine_len == sizeof(ProtoHdr));
  CHECK(r.hdr.mgrid == kRobotMgrId);
  CHECK(r.hdr.subtype == static_cast<std::uint16_t>(RobotOp::Step));
  CHECK(r.version == kRobotProtoVersion);
  CHECK(r.seq == 7);
  // obs wire conversion is a bit-exact memcpy of the doubles
  Obs o;
  o.pusher = Vec2{0.1, -0.2};
  o.obj[0] = Vec2{0.3, 0.4};
  o.obj[1] = Vec2{-0.5, 0.6};
  double w[2 + 2 * kNumObjects] = {};
  obs_to_wire(o, w);
  Obs back = obs_from_wire(w);
  CHECK(std::memcmp(&o, &back, sizeof o) == 0);
}

TEST(robot_server_rejects_bad_requests) {
  RobotServer srv;
  RobotReply rep{};
  for (int op = 3; op <= 9; ++op) {           // GetObs..Status: all need a Reset first
    RobotRequest r = make_request(static_cast<RobotOp>(op), 1);
    srv.handle(r, sizeof r, rep);
    CHECK(rep.status == ENODEV);
    CHECK(rep.seq == 1);                      // even error replies echo seq
  }
  RobotRequest h = make_request(RobotOp::Hello, 2);
  srv.handle(h, sizeof h, rep);
  CHECK(rep.status == 0);
  CHECK(rep.seq == 2);
  RobotRequest rs = make_request(RobotOp::Reset, 3);
  rs.i[0] = 1000; rs.i[1] = 0; rs.i[2] = 1; rs.i[3] = -1;
  srv.handle(rs, sizeof rs, rep);
  CHECK(rep.status == 0);
  CHECK(rep.fault >= 0 && rep.fault <= 3);
  CHECK(rep.ticks == 0);
  RobotRequest g = make_request(RobotOp::Truth, 4);
  srv.handle(g, sizeof g, rep);
  double ref[2 + 2 * kNumObjects];
  std::memcpy(ref, rep.obs, sizeof ref);
  auto unchanged = [&] {
    RobotRequest gg = make_request(RobotOp::Truth, 5);
    srv.handle(gg, sizeof gg, rep);
    CHECK(std::memcmp(rep.obs, ref, sizeof ref) == 0);   // simulator state untouched by the rejects
  };
  RobotRequest r = make_request(RobotOp::Step, 6);
  srv.handle(r, sizeof r - 1, rep);                      // short message
  CHECK(rep.status == EINVAL); unchanged();
  r.hdr.mgrid = static_cast<std::uint16_t>(kRobotMgrId + 1);
  srv.handle(r, sizeof r, rep);                          // wrong mgrid
  CHECK(rep.status == EINVAL); unchanged();
  r = make_request(RobotOp::Step, 7); r.version = 999;
  srv.handle(r, sizeof r, rep);                          // wrong version
  CHECK(rep.status == EPROTO); unchanged();
  r = make_request(static_cast<RobotOp>(99), 8);
  srv.handle(r, sizeof r, rep);                          // bad op
  CHECK(rep.status == EINVAL); unchanged();
  r = make_request(RobotOp::Knock, 9); r.i[0] = 5; r.d[0] = 0.01; r.d[1] = 0.0;
  srv.handle(r, sizeof r, rep);                          // bad object index
  CHECK(rep.status == EINVAL); unchanged();
  r = make_request(RobotOp::Step, 10); r.d[0] = std::nan(""); r.d[1] = 0.0;
  srv.handle(r, sizeof r, rep);                          // NaN target
  CHECK(rep.status == EINVAL); unchanged();
  r = make_request(RobotOp::Step, 11); r.d[0] = 0.1; r.d[1] = 0.0;
  srv.handle(r, sizeof r, rep);                          // legal request still works
  CHECK(rep.status == 0);
  CHECK(rep.ticks == 1);
  CHECK(srv.requests() == 23);   // 7 ENODEV + hello + reset + truth + 6 attacks + 6 truth probes + legal step
  CHECK(srv.errors() == 13);     // 7 ENODEV + 6 rejects (short/mgrid/version/op/obj/NaN)
}

TEST(robot_link_seq_echo) {
  RobotServer srv;
  LoopbackTransport t(srv);
  RemoteRobot robot(t);
  const Fault f = robot.reset(1000, SimConfig{});
  CHECK(f == Fault::None || f == Fault::Bump || f == Fault::PercepBias || f == Fault::Slippery);
  robot.step(Vec2{0.1, 0.0});
  CHECK(robot.ticks() >= 1);
  CHECK(robot.last_ticks() == robot.ticks());
  BadSeqTransport bad(srv);
  RemoteRobot robot2(bad);          // hello must pass: corruption starts only afterwards
  bad.corrupt = true;
  bool threw = false;
  try { robot2.step(Vec2{0.1, 0.0}); } catch (const RobotLinkError& e) { threw = e.code == EPROTO; }
  CHECK(threw);
  FailTransport fail(srv);
  RemoteRobot robot3(fail);          // hello succeeds (server is fine)...
  fail.fail = true;                  // ...then the driver process dies
  bool threw2 = false;
  try { robot3.step(Vec2{0.1, 0.0}); } catch (const RobotLinkError& e) { threw2 = e.code == EPIPE; }
  CHECK(threw2);
  bool threw3 = false;
  try { robot3.reset(1, SimConfig{}); } catch (const RobotLinkError&) { threw3 = true; }
  CHECK(threw3);
}

TEST(robot_link_equivalence) {
  ExperimentConfig ecfg;
  for (std::uint64_t ep = 1000; ep <= 1029; ++ep) {
    for (int arm = 0; arm < 3; ++arm) {
      SimConfig sc;
      sc.field_faults = true;
      // ---- in-process PushSim2D ----
      PushSim2D sim(ep, sc);
      ScriptedPushPolicy pol(ecfg.policy_bias_std);
      pol.new_episode(ep);
      MiddlewareConfig mc;
      mc.max_retries = arm == 0 ? 0 : 3;
      mc.episode_step_budget = 2000;
      mc.seed = ep;
      NaiveRetry naive;
      CorrelationAwareRetry corr;
      RetryPolicy* rp = arm == 0 ? nullptr : arm == 1 ? static_cast<RetryPolicy*>(&naive) : static_cast<RetryPolicy*>(&corr);
      ReliabilityMiddleware mw(pol, sim, rp, mc, nullptr);
      auto task = make_task(sim, ep);
      EpisodeReport rep_local = mw.run(task);
      Obs truth_local = sim.true_state();
      const long ticks_local = sim.ticks();
      // ---- RemoteRobot over LoopbackTransport, same policy + seeds ----
      RobotServer srv;
      LoopbackTransport tr(srv);
      RemoteRobot rrobot(tr);
      rrobot.reset(ep, sc);
      ScriptedPushPolicy pol2(ecfg.policy_bias_std);
      pol2.new_episode(ep);
      NaiveRetry naive2;
      CorrelationAwareRetry corr2;
      RetryPolicy* rp2 = arm == 0 ? nullptr : arm == 1 ? static_cast<RetryPolicy*>(&naive2) : static_cast<RetryPolicy*>(&corr2);
      ReliabilityMiddleware mw2(pol2, rrobot, rp2, mc, nullptr);
      auto task2 = make_task(static_cast<TruthSensor&>(rrobot), ep);
      EpisodeReport rep_remote = mw2.run(task2);
      Obs truth_remote = rrobot.truth();
      const long ticks_remote = rrobot.ticks();
      CHECK(rep_local.success == rep_remote.success);
      CHECK(rep_local.abort_reason == rep_remote.abort_reason);
      CHECK(rep_local.failed_at == rep_remote.failed_at);
      CHECK(same_report(rep_local, rep_remote));
      CHECK(std::memcmp(&truth_local, &truth_remote, sizeof(Obs)) == 0);
      CHECK(ticks_local == ticks_remote);
    }
  }
}

TEST_MAIN()
