// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Unit tests for the plan control command core: parse_plan (CSV syntax rules) and apply_plan (reorder/filter,
// task left unchanged on error).
#include "minitest.hpp"
#include "relmw/experiment.hpp"
#include "relmw/sim2d.hpp"

#include <string>
#include <vector>

using namespace relmw;

static std::vector<std::string> names_of(const std::vector<Subtask>& t) {
  std::vector<std::string> v;
  for (auto& st : t) v.push_back(st.name);
  return v;
}

TEST(plan_parse_valid) {
  std::vector<std::string> out;
  std::string err;
  CHECK(parse_plan("push_B,push_A", out, &err));
  CHECK(out.size() == 2);
  CHECK(out[0] == "push_B" && out[1] == "push_A");
  CHECK(parse_plan("push_A", out, &err));
  CHECK(out.size() == 1);
  CHECK(parse_plan("push_A,push_B,push_A,push_B,push_A,push_B,push_A,push_B", out, &err));  // exactly 8
  CHECK(out.size() == 8);
}

TEST(plan_parse_rejects) {
  const char* bad[] = {"", ",", "push_A,", ",push_A", "push_A,,push_B", "push A", "push_A, push_B", "push-A",
                       "push_A,push_B,push_A,push_B,push_A,push_B,push_A,push_B,push_A"};
  for (const char* b : bad) {
    std::vector<std::string> out;
    std::string err;
    CHECK(!parse_plan(b, out, &err));
    CHECK(!err.empty());
    CHECK(out.empty());
  }
}

TEST(plan_apply_reorders) {
  PushSim2D sim(1000, SimConfig{});
  auto task = make_task(sim, 1000);
  std::vector<std::string> plan;
  std::string err;
  CHECK(parse_plan("push_B,push_A", plan, &err));
  CHECK(apply_plan(task, plan, &err));
  auto n = names_of(task);
  CHECK(n.size() == 2);
  CHECK(n[0] == "push_B" && n[1] == "push_A");
}

TEST(plan_apply_filters) {
  PushSim2D sim(1001, SimConfig{});
  auto task = make_task(sim, 1001);
  std::vector<std::string> plan;
  std::string err;
  CHECK(parse_plan("push_A", plan, &err));
  CHECK(apply_plan(task, plan, &err));
  auto n = names_of(task);
  CHECK(n.size() == 1);
  CHECK(n[0] == "push_A");
}

TEST(plan_apply_rejects_and_leaves_task_unchanged) {
  PushSim2D sim(1002, SimConfig{});
  auto task = make_task(sim, 1002);
  const auto orig = names_of(task);
  std::vector<std::string> plan;
  std::string err;
  CHECK(parse_plan("push_A,push_A", plan, &err));            // duplicates are rejected by apply_plan
  CHECK(!apply_plan(task, plan, &err));
  CHECK(!err.empty());
  CHECK(parse_plan("push_C", plan, &err));                    // unknown name
  CHECK(!apply_plan(task, plan, &err));
  CHECK(!err.empty());
  plan.clear();                                               // empty plan
  CHECK(!apply_plan(task, plan, &err));
  CHECK(!err.empty());
  CHECK(names_of(task) == orig);                              // task unchanged on every error
}

TEST_MAIN()
