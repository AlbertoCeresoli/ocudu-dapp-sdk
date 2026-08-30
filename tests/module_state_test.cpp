// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/module_support.h"
#include <array>
#include <atomic>
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>

using namespace ocudu::dapp::reference;

namespace {

std::atomic<uint32_t> shared_destructions{};
std::atomic<uint32_t> worker_destructions{};

struct model_state {
  explicit model_state(const void* args) : weights(*static_cast<const std::array<float, 2>*>(args)) {}
  ~model_state() noexcept { shared_destructions.fetch_add(1, std::memory_order_relaxed); }

  const std::array<float, 2> weights;
  std::atomic<uint64_t>      accepted_invocations{};
  uint64_t                   model_generation{7};
};

struct lane_state {
  lane_state(const model_state& model, uint32_t worker_id_) : initial_weight(model.weights[0]), worker_id(worker_id_) {}
  ~lane_state() noexcept { worker_destructions.fetch_add(1, std::memory_order_relaxed); }

  float    initial_weight;
  uint32_t worker_id;
  uint64_t history{};
};

using state_context = stateful_module_context<model_state, lane_state>;

TEST(module_support, algorithm_guard_preserves_status_and_contains_exceptions)
{
  EXPECT_EQ(guard_algorithm_call([] { return OCUDU_DAPP_UNSUPPORTED_V1; }), OCUDU_DAPP_UNSUPPORTED_V1);
  EXPECT_EQ(guard_algorithm_call([]() -> ocudu_dapp_status_v1 { throw std::bad_alloc(); }), OCUDU_DAPP_RESOURCE_V1);
  EXPECT_EQ(guard_algorithm_call([]() -> ocudu_dapp_status_v1 { throw std::runtime_error("algorithm failure"); }),
            OCUDU_DAPP_EXCEPTION_V1);
  EXPECT_EQ(guard_algorithm_call([]() -> ocudu_dapp_status_v1 { throw 7; }), OCUDU_DAPP_EXCEPTION_V1);
}

void invoke_many(void* worker, uint64_t count)
{
  auto* model = state_context::shared_state(worker);
  auto* lane  = state_context::worker_state(worker);
  ASSERT_NE(model, nullptr);
  ASSERT_NE(lane, nullptr);
  for (uint64_t invocation = 0; invocation != count; ++invocation) {
    ASSERT_EQ(state_context::begin_invocation(worker), OCUDU_DAPP_OK_V1);
    lane->history += 1;
    model->accepted_invocations.fetch_add(1, std::memory_order_relaxed);
  }
}

} // namespace

TEST(module_state, keeps_instance_weights_and_independent_worker_history)
{
  shared_destructions.store(0, std::memory_order_relaxed);
  worker_destructions.store(0, std::memory_order_relaxed);

  ocudu_dapp_host_api_v1 host{};
  host.struct_size = sizeof(host);
  host.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  host.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  const std::array<float, 2> weights{1.25F, -0.5F};
  void*                      instance = nullptr;
  ASSERT_EQ(state_context::create(&host, &weights, &instance), OCUDU_DAPP_OK_V1);
  ASSERT_NE(instance, nullptr);
  ASSERT_EQ(start(instance), OCUDU_DAPP_OK_V1);

  const auto& workers = state_context::worker_api();
  void*       lane3   = nullptr;
  void*       lane9   = nullptr;
  ASSERT_EQ(workers.create_worker_context(instance, 3, &lane3), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(workers.create_worker_context(instance, 9, &lane9), OCUDU_DAPP_OK_V1);

  EXPECT_EQ(state_context::shared_state(lane3), state_context::shared_state(lane9));
  EXPECT_EQ(state_context::shared_state_from_instance(instance), state_context::shared_state(lane3));
  EXPECT_EQ(state_context::shared_state(lane3)->weights, weights);
  EXPECT_EQ(state_context::shared_state(lane3)->model_generation, 7U);
  EXPECT_EQ(state_context::worker_state(lane3)->initial_weight, weights[0]);
  EXPECT_EQ(state_context::worker_id(lane3), 3U);
  EXPECT_EQ(state_context::worker_id(lane9), 9U);

  std::thread first(invoke_many, lane3, 10'000);
  std::thread second(invoke_many, lane9, 7'000);
  first.join();
  second.join();

  EXPECT_EQ(state_context::worker_state(lane3)->history, 10'000U);
  EXPECT_EQ(state_context::worker_state(lane9)->history, 7'000U);
  EXPECT_EQ(state_context::shared_state(lane3)->accepted_invocations.load(std::memory_order_relaxed), 17'000U);

  workers.destroy_worker_context(instance, lane3);
  workers.destroy_worker_context(instance, lane9);
  EXPECT_EQ(worker_destructions.load(std::memory_order_relaxed), 2U);
  EXPECT_EQ(shared_destructions.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(stop(instance), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(quiesce(instance, 0), OCUDU_DAPP_OK_V1);
  destroy(instance);
  EXPECT_EQ(shared_destructions.load(std::memory_order_relaxed), 1U);
}

TEST(module_state, model_management_is_generation_safe_and_stateful)
{
  ocudu_dapp_host_api_v1 host{};
  host.struct_size     = sizeof(host);
  host.abi_major       = OCUDU_DAPP_ABI_MAJOR_V1;
  host.abi_minor       = OCUDU_DAPP_ABI_MINOR_V1;
  void* state_instance = nullptr;
  ASSERT_EQ(create(&host, nullptr, &state_instance), OCUDU_DAPP_OK_V1);

  const auto&                  management = management_api();
  const std::array<uint8_t, 4> artifact{1, 2, 3, 4};
  ocudu_dapp_model_request_v1  request{};
  request.struct_size         = sizeof(request);
  request.action              = OCUDU_DAPP_MODEL_STAGE_V1;
  request.expected_generation = 1;
  request.model_id            = "receiver";
  request.model_version       = "1";
  request.artifact.data       = artifact.data();
  request.artifact.size       = artifact.size();

  uint64_t generation = 0;
  request.dry_run     = 1;
  EXPECT_EQ(management.manage_model(state_instance, &request, &generation), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(generation, 1U);

  request.dry_run = 0;
  EXPECT_EQ(management.manage_model(state_instance, &request, &generation), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(generation, 2U);
  EXPECT_EQ(static_cast<instance*>(state_instance)->staged_model.artifact,
            std::vector<uint8_t>(artifact.begin(), artifact.end()));

  request.action              = OCUDU_DAPP_MODEL_VALIDATE_V1;
  request.expected_generation = 1;
  request.artifact            = {};
  EXPECT_EQ(management.manage_model(state_instance, &request, &generation), OCUDU_DAPP_CONFLICT_V1);
  EXPECT_EQ(generation, 2U);

  for (const auto action : {OCUDU_DAPP_MODEL_VALIDATE_V1, OCUDU_DAPP_MODEL_WARM_V1, OCUDU_DAPP_MODEL_ACTIVATE_V1}) {
    request.action              = action;
    request.expected_generation = generation;
    EXPECT_EQ(management.manage_model(state_instance, &request, &generation), OCUDU_DAPP_OK_V1);
  }
  EXPECT_EQ(generation, 5U);
  EXPECT_EQ(static_cast<instance*>(state_instance)->active_model.id, "receiver");
  EXPECT_TRUE(static_cast<instance*>(state_instance)->staged_model.id.empty());

  destroy(state_instance);
}
