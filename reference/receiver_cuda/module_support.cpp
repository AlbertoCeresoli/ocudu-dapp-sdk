// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/module_support.h"
#include "ocudu/dapp/use_cases/v1/receiver.h"
#include "ocudu_dapp_sdk/receiver_cuda.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cuda_runtime.h>
#include <mutex>
#include <new>
#include <vector>

using namespace ocudu::dapp::reference;

namespace {

struct receiver_lane_state;

struct receiver_model_state {
  std::array<receiver_cuda_state, 2> slots{};
  std::atomic<uint32_t>              active_slot{0};
  std::atomic<uint32_t>              enqueue_readers{0};
  std::mutex                         lanes_mutex;
  std::vector<receiver_lane_state*>  lanes;
  int                                staged_slot{-1};
  int                                previous_slot{-1};

  ~receiver_model_state() noexcept
  {
    for (auto& slot : slots) {
      release_receiver_cuda_state(slot);
    }
  }

  bool reusable(uint32_t slot) noexcept;
  void reset_events(uint32_t slot) noexcept;
};

struct receiver_lane_state {
  receiver_lane_state(receiver_model_state& owner_, uint32_t worker_id_) : owner(owner_), worker_id(worker_id_)
  {
    if (initialize_receiver_cuda_worker_state(cuda) != OCUDU_DAPP_OK_V1) {
      throw std::bad_alloc();
    }
    for (auto& event : completion) {
      if (cudaEventCreateWithFlags(&event, cudaEventDisableTiming) != cudaSuccess) {
        static_cast<void>(cudaGetLastError());
        for (auto created : completion) {
          if (created != nullptr) {
            static_cast<void>(cudaEventDestroy(created));
          }
        }
        release_receiver_cuda_worker_state(cuda);
        throw std::bad_alloc();
      }
    }
    try {
      std::lock_guard<std::mutex> lock(owner.lanes_mutex);
      owner.lanes.push_back(this);
    } catch (...) {
      for (auto event : completion) {
        static_cast<void>(cudaEventDestroy(event));
      }
      release_receiver_cuda_worker_state(cuda);
      throw;
    }
  }

  ~receiver_lane_state() noexcept
  {
    {
      std::lock_guard<std::mutex> lock(owner.lanes_mutex);
      auto                        found = std::find(owner.lanes.begin(), owner.lanes.end(), this);
      if (found != owner.lanes.end()) {
        owner.lanes.erase(found);
      }
    }
    for (auto event : completion) {
      static_cast<void>(cudaEventDestroy(event));
    }
    release_receiver_cuda_worker_state(cuda);
  }

  receiver_model_state&            owner;
  uint32_t                         worker_id;
  uint64_t                         invocations{};
  receiver_cuda_worker_state       cuda;
  std::array<cudaEvent_t, 2>       completion{};
  std::array<std::atomic<bool>, 2> event_used{};
  std::array<std::atomic<bool>, 2> event_poisoned{};
};

bool receiver_model_state::reusable(uint32_t slot) noexcept
{
  if (slot >= slots.size() || slot == active_slot.load(std::memory_order_acquire) ||
      enqueue_readers.load(std::memory_order_acquire) != 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(lanes_mutex);
  for (auto* lane : lanes) {
    if (lane->event_poisoned[slot].load(std::memory_order_acquire)) {
      return false;
    }
    if (!lane->event_used[slot].load(std::memory_order_acquire)) {
      continue;
    }
    const auto status = cudaEventQuery(lane->completion[slot]);
    if (status != cudaSuccess) {
      if (status != cudaErrorNotReady) {
        static_cast<void>(cudaGetLastError());
        lane->event_poisoned[slot].store(true, std::memory_order_release);
      }
      return false;
    }
  }
  return true;
}

void receiver_model_state::reset_events(uint32_t slot) noexcept
{
  std::lock_guard<std::mutex> lock(lanes_mutex);
  for (auto* lane : lanes) {
    lane->event_used[slot].store(false, std::memory_order_release);
    lane->event_poisoned[slot].store(false, std::memory_order_release);
  }
}

using receiver_context = stateful_module_context<receiver_model_state, receiver_lane_state>;

bool decode_model(const ocudu_dapp_blob_view_v1& blob, receiver_cuda_model_artifact_v1& artifact) noexcept
{
  if (blob.data == nullptr || blob.size != sizeof(artifact)) {
    return false;
  }
  std::memcpy(&artifact, blob.data, sizeof(artifact));
  if (artifact.magic != receiver_cuda_model_magic_v1 || artifact.version != receiver_cuda_model_version_v1 ||
      artifact.level_count != receiver_cuda_model_level_count_v1) {
    return false;
  }
  for (uint32_t bits = 1; bits <= 4; ++bits) {
    const uint32_t offset = (bits - 1U) * 16U;
    const uint32_t count  = 1U << bits;
    for (uint32_t first = 0; first != count; ++first) {
      if (!std::isfinite(artifact.constellation_levels[offset + first])) {
        return false;
      }
      for (uint32_t second = first + 1; second != count; ++second) {
        if (std::abs(artifact.constellation_levels[offset + first] - artifact.constellation_levels[offset + second]) <
            1.0e-6F) {
          return false;
        }
      }
    }
  }
  return true;
}

ocudu_dapp_status_v1 manage_receiver_model(void* context, const ocudu_dapp_model_request_v1* request,
                                           uint64_t* generation) noexcept
{
  auto* base  = static_cast<instance*>(context);
  auto* model = receiver_context::shared_state_from_instance(context);
  if (base == nullptr || model == nullptr || request == nullptr || generation == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  *generation         = base->model_generation;
  const auto delegate = [&]() noexcept { return management_api().manage_model(context, request, generation); };

  if (request->action == OCUDU_DAPP_MODEL_STAGE_V1) {
    receiver_cuda_model_artifact_v1 artifact{};
    if (!decode_model(request->artifact, artifact)) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }
    const auto status = delegate();
    if (status == OCUDU_DAPP_OK_V1 && !request->dry_run) {
      model->staged_slot = -1;
    }
    return status;
  }

  if (request->action == OCUDU_DAPP_MODEL_VALIDATE_V1) {
    receiver_cuda_model_artifact_v1 artifact{};
    if (!decode_model(
            {base->staged_model.artifact.data(), static_cast<uint32_t>(base->staged_model.artifact.size()), 0},
            artifact)) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }
    return delegate();
  }

  if (request->action == OCUDU_DAPP_MODEL_WARM_V1) {
    receiver_cuda_model_artifact_v1 artifact{};
    if (!decode_model(
            {base->staged_model.artifact.data(), static_cast<uint32_t>(base->staged_model.artifact.size()), 0},
            artifact)) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }
    const uint32_t slot = 1U - model->active_slot.load(std::memory_order_acquire);
    if (model->previous_slot == static_cast<int>(slot) || !model->reusable(slot)) {
      return OCUDU_DAPP_CONFLICT_V1;
    }
    if (request->dry_run) {
      return delegate();
    }
    receiver_cuda_state prepared{};
    auto                status = initialize_receiver_cuda_state(prepared, artifact.constellation_levels);
    if (status != OCUDU_DAPP_OK_V1) {
      return status;
    }
    status = delegate();
    if (status != OCUDU_DAPP_OK_V1) {
      release_receiver_cuda_state(prepared);
      return status;
    }
    release_receiver_cuda_state(model->slots[slot]);
    model->slots[slot] = prepared;
    model->reset_events(slot);
    model->staged_slot = static_cast<int>(slot);
    return OCUDU_DAPP_OK_V1;
  }

  if (request->action == OCUDU_DAPP_MODEL_ACTIVATE_V1) {
    if (model->staged_slot < 0) {
      return OCUDU_DAPP_CONFLICT_V1;
    }
    if (request->dry_run) {
      return delegate();
    }
    const bool     had_named_active = !base->active_model.id.empty();
    const uint32_t next             = static_cast<uint32_t>(model->staged_slot);
    const auto     status           = delegate();
    if (status != OCUDU_DAPP_OK_V1) {
      return status;
    }
    const uint32_t old   = model->active_slot.exchange(next, std::memory_order_acq_rel);
    model->previous_slot = had_named_active ? static_cast<int>(old) : -1;
    model->staged_slot   = -1;
    return OCUDU_DAPP_OK_V1;
  }

  if (request->action == OCUDU_DAPP_MODEL_ROLLBACK_V1) {
    if (model->previous_slot < 0) {
      return OCUDU_DAPP_CONFLICT_V1;
    }
    if (request->dry_run) {
      return delegate();
    }
    const auto status = delegate();
    if (status != OCUDU_DAPP_OK_V1) {
      return status;
    }
    const uint32_t previous = static_cast<uint32_t>(model->previous_slot);
    const uint32_t old      = model->active_slot.exchange(previous, std::memory_order_acq_rel);
    model->previous_slot    = static_cast<int>(old);
    return OCUDU_DAPP_OK_V1;
  }

  if (request->action == OCUDU_DAPP_MODEL_RETIRE_V1) {
    if (base->active_model.id == request->model_id) {
      return OCUDU_DAPP_CONFLICT_V1;
    }
    int retired_slot = -1;
    if (base->staged_model.id == request->model_id) {
      retired_slot = model->staged_slot;
    } else if (base->previous_model.id == request->model_id) {
      retired_slot = model->previous_slot;
    }
    if (retired_slot >= 0 && !model->reusable(static_cast<uint32_t>(retired_slot))) {
      return OCUDU_DAPP_CONFLICT_V1;
    }
    if (request->dry_run) {
      return delegate();
    }
    const auto status = delegate();
    if (status != OCUDU_DAPP_OK_V1) {
      return status;
    }
    if (retired_slot >= 0) {
      release_receiver_cuda_state(model->slots[retired_slot]);
      model->reset_events(static_cast<uint32_t>(retired_slot));
    }
    if (model->staged_slot == retired_slot) {
      model->staged_slot = -1;
    }
    if (model->previous_slot == retired_slot) {
      model->previous_slot = -1;
    }
    return OCUDU_DAPP_OK_V1;
  }
  return delegate();
}

const ocudu_dapp_management_v1& receiver_management_api() noexcept
{
  static const auto api = [] {
    auto result         = management_api();
    result.manage_model = manage_receiver_model;
    return result;
  }();
  return api;
}

ocudu_dapp_status_v1 prepare_receiver(void* context, const void* args) noexcept
{
  auto* model = receiver_context::shared_state_from_instance(context);
  if (model == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const uint32_t active = model->active_slot.load(std::memory_order_acquire);
  if (model->slots[active].constellation_levels == nullptr) {
    const auto status = initialize_receiver_cuda_state(model->slots[active]);
    if (status != OCUDU_DAPP_OK_V1) {
      return status;
    }
  }
  return prepare(context, args);
}

ocudu_dapp_status_v1 invoke(void* context, const ocudu_dapp_receiver_input_v1* input,
                            ocudu_dapp_receiver_output_v1* output) noexcept
{
  if (input == nullptr || output == nullptr)
    return OCUDU_DAPP_INVALID_INPUT_V1;
  const auto status = receiver_context::begin_invocation(context);
  auto*      model  = receiver_context::shared_state(context);
  auto*      lane   = receiver_context::worker_state(context);
  if (status != OCUDU_DAPP_OK_V1 || model == nullptr || lane == nullptr) {
    return status != OCUDU_DAPP_OK_V1 ? status : OCUDU_DAPP_INVALID_INPUT_V1;
  }
  model->enqueue_readers.fetch_add(1, std::memory_order_acq_rel);
  const uint32_t slot = model->active_slot.load(std::memory_order_acquire);
  ++lane->invocations;
  auto invoked = guard_algorithm_call([&] { return receive_cuda(*input, *output, model->slots[slot], lane->cuda); });
  if (invoked == OCUDU_DAPP_OK_V1) {
    receiver_context::observe_input_layers(
        context, static_cast<const ocudu_dapp_pusch_metadata_v1*>(input->pusch.metadata.data)->nof_layers);
  }
  // A failed or throwing implementation may already have enqueued work. Fence
  // every admitted call so model retirement cannot recycle its device state
  // while such work still references it.
  if (cudaEventRecord(lane->completion[slot], reinterpret_cast<cudaStream_t>(static_cast<uintptr_t>(
                                                  input->invocation.execution_stream))) == cudaSuccess) {
    lane->event_used[slot].store(true, std::memory_order_release);
  } else {
    static_cast<void>(cudaGetLastError());
    lane->event_poisoned[slot].store(true, std::memory_order_release);
    if (invoked == OCUDU_DAPP_OK_V1) {
      invoked = OCUDU_DAPP_INTERNAL_ERROR_V1;
    }
  }
  model->enqueue_readers.fetch_sub(1, std::memory_order_release);
  return invoked;
}
const ocudu_dapp_receiver_interface_v1 api     = {receiver_context::worker_api(), invoke, {}};
const auto                             profile = make_pusch_profile(2, OCUDU_DAPP_PUSCH_PROFILE_DC_OVERLAP_V1);
const auto                             descriptor =
    make_interface(OCUDU_DAPP_INTERFACE_RECEIVER_V1, OCUDU_DAPP_CLASS_A_INLINE_V1, OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1,
                   OCUDU_DAPP_THREADING_PER_WORKER_V1, &api, OCUDU_DAPP_PUSCH_CAP_RESIDENT_ACCELERATOR_V1, &profile);
} // namespace

ocudu_dapp_status_v1 populate_receiver_cuda_module(const ocudu_dapp_host_info_v1* host,
                                                   ocudu_dapp_module_v1*          output) noexcept
{
  const auto status = receiver_context::populate(host, output, "org.ocudu.reference.receiver.cuda", &descriptor);
  if (status == OCUDU_DAPP_OK_V1) {
    output->prepare    = prepare_receiver;
    output->management = &receiver_management_api();
  }
  return status;
}
