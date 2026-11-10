#include "relmw/middleware.hpp"

namespace relmw {

static Event mk(EventType t, const std::string& st, int k = 0, const std::string& strat = "",
               FailureMode m = FailureMode::None, bool corr = false) {
  Event e; e.type = t; e.subtask = st; e.attempt = k; e.strategy = strat; e.mode = m; e.correlated = corr;
  return e;
}

const char* to_string(EventType t) {
  switch (t) {
    case EventType::SubtaskStart: return "SUBTASK_START";
    case EventType::VerifyFail: return "VERIFY_FAIL";
    case EventType::Rollback: return "ROLLBACK";
    case EventType::Retry: return "RETRY";
    case EventType::Escalate: return "ESCALATE";
    case EventType::SubtaskOk: return "SUBTASK_OK";
    case EventType::EpisodeDone: return "EPISODE_DONE";
  }
  return "?";
}

int EpisodeReport::n_failures() const {
  int n = 0;
  for (auto& a : attempts) n += a.ok ? 0 : 1;
  return n;
}

ReliabilityMiddleware::ReliabilityMiddleware(Policy& policy, RobotAdapter& robot, RetryPolicy* retry,
                                             const MiddlewareConfig& cfg, EventSink sink)
    : policy_(policy), robot_(robot), retry_(retry ? retry : &naive_), cfg_(cfg),
      rng_(hash_mix({0xA1u, cfg.seed})), strat_rng_(hash_mix({0xB2u, cfg.seed})), sink_(std::move(sink)) {}

void ReliabilityMiddleware::emit(Event e) {
  e.steps_used = used_;
  if (sink_) sink_(e);
}

ReliabilityMiddleware::SubResult ReliabilityMiddleware::run_subtask(Subtask& st, const Perturb& p, Strategy* strategy) {
  Obs obs = robot_.get_obs();
  Verdict v;
  for (int t = 0; t < st.max_steps; ++t) {
    Obs pobs = strategy ? strategy->obs_fn(obs, strat_rng_) : obs;
    Vec2 act = policy_.act(pobs, st.name, st.meta, p);
    if (strategy) act = strategy->act_fn(act, obs);
    obs = robot_.step(act);
    if ((t + 1) % st.check_every == 0) {
      v = st.verifier(obs);
      int extra = 0;
      while (v.ok && extra < st.hold - 1) {           // settle check: hold still, look again
        for (int k = 0; k < st.check_every; ++k) obs = robot_.step(robot_.robot_pose());
        v = st.verifier(obs);
        ++extra;
      }
      if (v.ok) return {true, FailureMode::None, t + 1 + extra * st.check_every, v, obs};
    }
  }
  v = st.verifier(obs);
  return {v.ok, v.mode, st.max_steps, v, obs};
}

EpisodeReport ReliabilityMiddleware::run(std::vector<Subtask>& subtasks) {
  EpisodeReport rep;
  used_ = 0;
  if (cfg_.max_retries > 0) retry_->new_episode();
  for (auto& st : subtasks)
    if (!run_with_retries(st, rep, "")) { emit(mk(EventType::EpisodeDone, st.name)); return rep; }
  if (cfg_.final_check && cfg_.max_retries > 0) {
    for (auto& st : subtasks) {
      if (!st.verifier(robot_.get_obs()).ok) {
        Attempt a; a.subtask = st.name; a.attempt = -1; a.mode = FailureMode::Regressed; a.strategy = "final_check";
        rep.attempts.push_back(a);
        if (!run_with_retries(st, rep, "redo")) { emit(mk(EventType::EpisodeDone, st.name)); return rep; }
      }
    }
  }
  rep.success = true;
  emit(mk(EventType::EpisodeDone, ""));
  return rep;
}

bool ReliabilityMiddleware::run_with_retries(Subtask& st, EpisodeReport& rep, const std::string& tag) {
  Vec2 start_pose = robot_.robot_pose();               // subtask checkpoint
  std::vector<Attempt> history;
  for (int k = 0; k <= cfg_.max_retries; ++k) {
    if (used_ >= cfg_.episode_step_budget) { rep.failed_at = st.name; rep.abort_reason = "time_budget"; return false; }
    Verdict v0 = st.verifier(robot_.get_obs());
    std::optional<Vec2> start_obj;
    if (v0.has_obj) start_obj = v0.obj_xy;
    std::unique_ptr<Strategy> strategy;
    bool pred_corr = false;
    Perturb perturb;
    perturb.retry = k;
    if (k == 0) {
      if (cfg_.max_retries > 0) {
        strategy = retry_->proactive(st.name);
        if (strategy) strategy->prepare(robot_, start_pose, start_obj, strat_rng_, st.meta);
      }
      emit(mk(EventType::SubtaskStart, st.name, 0, strategy ? strategy->name() + "(proactive)" : "first_try"));
    } else {
      Choice c = retry_->choose(st.name, history, k);
      if (!c.strategy) {
        rep.failed_at = st.name; rep.abort_reason = "escalate_to_operator";
        emit(mk(EventType::Escalate, st.name, k));
        return false;
      }
      strategy = std::move(c.strategy);
      pred_corr = c.predicted_correlated;
      perturb.has_seed = true;
      perturb.seed = rng_.next_u64() & ((1ULL << 30) - 1);
      strategy->prepare(robot_, start_pose, start_obj, strat_rng_, st.meta);
      emit(mk(EventType::Retry, st.name, k, strategy->name()));
    }
    SubResult r = run_subtask(st, perturb, strategy.get());
    used_ += r.steps;
    Attempt a;
    a.subtask = st.name; a.attempt = k; a.ok = r.ok; a.mode = r.mode; a.steps = r.steps;
    a.strategy = (strategy ? strategy->name() : std::string("first_try")) + (k == 0 && strategy ? "(proactive)" : "") +
                 (tag.empty() ? "" : "@" + tag);
    a.predicted_correlated = pred_corr;
    a.has_end_obj = r.info.has_obj; a.end_obj = r.info.obj_xy;
    if (st.oracle) a.gt_ok = st.oracle(r.obs).ok ? 1 : 0;
    if (!r.ok) a.correlated = is_repeat(a, history);
    if (k > 0) {   // feed the failure library: did strategy X fix failure mode Y in subtask Z?
      FailureMode prev = FailureMode::None;
      for (auto it = history.rbegin(); it != history.rend(); ++it) if (!it->ok) { prev = it->mode; break; }
      retry_->record(st.name, prev, strategy->name(), r.ok);
    }
    history.push_back(a);
    rep.attempts.push_back(a);
    if (r.ok) { emit(mk(EventType::SubtaskOk, st.name, k, a.strategy)); return true; }
    emit(mk(EventType::VerifyFail, st.name, k, a.strategy, r.mode, a.correlated));
    if (r.mode == FailureMode::Unrecoverable) { rep.failed_at = st.name; rep.abort_reason = "unrecoverable"; return false; }
    if (k < cfg_.max_retries) {
      robot_.move_robot_to(start_pose);                // rollback: ROBOT only, world is NOT reset
      emit(mk(EventType::Rollback, st.name, k));
    }
  }
  rep.failed_at = st.name; rep.abort_reason = "retry_budget";
  return false;
}

}  // namespace relmw
