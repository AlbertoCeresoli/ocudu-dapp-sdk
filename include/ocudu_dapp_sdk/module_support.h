// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Lifecycle / worker / metrics helpers for open references and commercial modules.
//
// Link OCUDUDapp::reference_support (in-tree: ocudu_reference_support). Do not
// copy this header's companion .cpp into vendor trees — that is dual maintenance.
//
// Typical package layout (see examples/vendor_min):
//   module.cpp          — pure ocudu_dapp_query_v1 trampoline
//   module_support.cpp  — stateful_module_context + invoke
//   management.cpp      — package_config schema (optional product keys)
//   algorithm.*         — product IP
//
// Config management: prefer package_config.h / make_config_management rather
// than reimplementing describe/get/set. Assign output->management on populate.

#pragma once

#include "ocudu/dapp/abi/v1/module.h"
#include "ocudu/dapp/use_cases/v1/common.h"
#include "ocudu/dapp/use_cases/v1/pusch_metadata.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ocudu::dapp::reference {

/// Per-instance host-facing state. Not copyable/assignable (atomics + mutex).
/// Product packages may derive from this type to hang private fields; package_config
/// always casts the management context to instance*.
struct instance {
  virtual ~instance() = default;

  ocudu_dapp_host_api_v1 host{};
  std::atomic<bool>      active{false};
  /// When false, begin_invocation returns BYPASS (host keeps conventional path).
  std::atomic<bool>     enabled{true};
  std::atomic<uint64_t> invocation_count{0};
  std::atomic<uint64_t> input_max_layers{0};
  std::mutex            management_mutex;
  uint64_t              config_generation = 1;
  struct model_slot {
    std::string          id;
    std::string          version;
    std::vector<uint8_t> artifact;
    uint32_t             phase{};
  };
  uint64_t   model_generation = 1;
  model_slot staged_model;
  model_slot active_model;
  model_slot previous_model;
};

// ---------------------------------------------------------------------------
// Lifecycle callbacks (assign to ocudu_dapp_module_v1, or reuse via
// populate_module below). The host drives them in order:
//   create -> configure -> prepare -> warmup -> start -> [invocations] ->
//   stop -> quiesce -> destroy
// All are noexcept; failures return an ABI status and never throw across the
// C boundary. `context` is the instance pointer produced by create().
// ---------------------------------------------------------------------------

/// Initializes an already-constructed (possibly derived) instance from the host
/// API table. Called by create(); call it directly when a package constructs
/// its own instance subclass.
ocudu_dapp_status_v1 initialize_instance(instance& state, const ocudu_dapp_host_api_v1* host) noexcept;
/// Allocates a plain reference::instance. Packages with private state use
/// stateful_module_context::create (or their own) instead.
ocudu_dapp_status_v1 create(const ocudu_dapp_host_api_v1* host, const void*, void** output) noexcept;
/// Accepts host configuration off the hot path. The reference implementation
/// bumps config_generation; packages using package_config.h get schema-checked
/// key handling instead of overriding this.
ocudu_dapp_status_v1 configure(void* context, const void*) noexcept;
/// Heavy one-time preparation: allocate device state, upload weights. Runs
/// before warmup; must leave the instance ready for start on success.
ocudu_dapp_status_v1 prepare(void* context, const void*) noexcept;
/// Optional dry-run of the invocation path so first-slot latency is not paid
/// on live traffic.
ocudu_dapp_status_v1 warmup(void* context, const void*) noexcept;
/// Marks the instance active; begin_invocation admits work only after this.
ocudu_dapp_status_v1 start(void* context) noexcept;
/// Marks the instance inactive; subsequent begin_invocation returns BYPASS.
ocudu_dapp_status_v1 stop(void* context) noexcept;
/// Host signal that no invocation is in flight (deadline in nanoseconds).
/// Retire deferred resources here — never during stop, which can race
/// in-flight lanes.
ocudu_dapp_status_v1 quiesce(void* context, uint64_t) noexcept;
/// Final teardown; the context pointer is invalid afterwards.
void destroy(void* context) noexcept;

/// Worker-context create/destroy table for modules without per-worker state
/// (every worker context aliases the instance). Typed per-worker state comes
/// from stateful_module_context::worker_api instead.
const ocudu_dapp_worker_api_v1& worker_api() noexcept;
/// Management table implementing config get/set and the bounded model
/// stage/validate/warm/activate/rollback slots declared in instance.
const ocudu_dapp_management_v1& management_api() noexcept;
/// Invocation admission gate: returns OK and counts the invocation when the
/// instance is started and enabled, BYPASS otherwise (host keeps the
/// conventional path). Call it first in every algorithm dispatch.
ocudu_dapp_status_v1 begin_invocation(instance& state) noexcept;
/// Overload recovering the instance from an opaque worker context created by
/// reference::worker_api (which aliases the instance pointer).
ocudu_dapp_status_v1 begin_invocation(void* worker_context) noexcept;
/// Forwards one bounded metric sample to the host metric sink.
/// \warning The numeric \p metric_id namespace is vendor-private and
/// UNREGISTERED: no catalog maps ids to names, and two vendors may
/// collide. Use the string-named describe_metrics/read_metrics catalog for
/// anything a management consumer must discover or interpret.
void add_metric(instance& state, uint32_t metric_id, uint64_t value) noexcept;
void add_metric(void* worker_context, uint32_t metric_id, uint64_t value) noexcept;
/// Records the highest layer count seen so metrics can report the admitted
/// input envelope.
void observe_input_layers(instance& state, uint32_t nof_layers) noexcept;
void observe_input_layers(void* worker_context, uint32_t nof_layers) noexcept;
/// Host API table for a worker context created by reference::worker_api;
/// nullptr for a foreign context.
const ocudu_dapp_host_api_v1* host_api(void* worker_context) noexcept;

/// Keep implementation exceptions behind the C ABI callback boundary.
///
/// Algorithm entry points intentionally need not be noexcept: a third-party
/// implementation may throw despite its authoring contract. ABI callbacks use
/// this guard and remain noexcept, allowing the host to disable or quarantine
/// the instance instead of terminating the DU process.
template <typename Callable>
ocudu_dapp_status_v1 guard_algorithm_call(Callable&& callable) noexcept
{
  try {
    return std::forward<Callable>(callable)();
  } catch (const std::bad_alloc&) {
    return OCUDU_DAPP_RESOURCE_V1;
  } catch (...) {
    return OCUDU_DAPP_EXCEPTION_V1;
  }
}

/// Builds the bounded PUSCH admission profile the host checks before module
/// callback dispatch (maximum layers plus completeness/feature flags).
ocudu_dapp_pusch_profile_v1 make_pusch_profile(uint32_t max_layers, uint32_t flags = 0) noexcept;

/// Assembles one interface descriptor: identity, class, placement, threading
/// contract, the per-interface vtable, declared capabilities, and an optional
/// PUSCH admission profile.
ocudu_dapp_interface_descriptor_v1 make_interface(uint32_t interface_id, uint32_t dapp_class, uint32_t placement_mask,
                                                  uint32_t threading, const void* vtable, uint64_t capabilities = 0,
                                                  const ocudu_dapp_pusch_profile_v1* profile = nullptr) noexcept;
/// Fills a module descriptor with the reference lifecycle callbacks, the
/// supplied name/interface, and ABI version stamps. The caller may override
/// individual callbacks afterwards (stateful_module_context::populate does
/// this for create).
ocudu_dapp_status_v1 populate_module(const ocudu_dapp_host_info_v1* host, ocudu_dapp_module_v1* output,
                                     const char* name, const ocudu_dapp_interface_descriptor_v1* interface) noexcept;
/// Identity-aware populate: vendor/version must match the package manifest exactly.
ocudu_dapp_status_v1 populate_module(const ocudu_dapp_host_info_v1* host, ocudu_dapp_module_v1* output,
                                     const char* name, const char* vendor, const char* version,
                                     const ocudu_dapp_interface_descriptor_v1* interface) noexcept;

/// Typed instance-owned and per-worker state for C++ dApp implementations.
///
/// SharedState lives for the module instance and is visible to every worker.
/// WorkerState lives for one host worker context. Mutable shared state remains
/// the module author's synchronization responsibility; lane-local state needs
/// no synchronization when the host honors the per-worker threading contract.
///
/// The class supplies the C ABI glue: \ref create builds the instance and its
/// SharedState (constructible from `(host_api, create_args)`, `(create_args)`,
/// `(host_api)` or default), \ref worker_api exposes worker-context
/// create/destroy callbacks that build one WorkerState per worker, and
/// \ref populate wires \ref create into a populated `ocudu_dapp_module_v1`.
/// Inside ABI callbacks, \ref shared_state / \ref worker_state / \ref
/// worker_id / \ref host_api recover the typed state from the opaque worker
/// context (\ref shared_state_from_instance from the instance pointer), and
/// \ref begin_invocation / \ref add_metric forward to the instance's
/// bypass/metrics bookkeeping. All hooks are noexcept; exceptions from state
/// constructors are mapped to ABI status codes.
template <typename SharedState, typename WorkerState>
class stateful_module_context
{
  static_assert(std::is_nothrow_destructible<SharedState>::value, "dApp shared-state destruction must be noexcept");
  static_assert(std::is_nothrow_destructible<WorkerState>::value, "dApp worker-state destruction must be noexcept");

  struct owned_instance final : instance {
    std::unique_ptr<SharedState> shared;
  };

  struct worker_context {
    owned_instance*              owner;
    uint32_t                     id;
    std::unique_ptr<WorkerState> local;
  };

  template <typename T>
  struct dependent_false : std::false_type {
  };

  static std::unique_ptr<SharedState> make_shared_state(const ocudu_dapp_host_api_v1& host, const void* args)
  {
    if constexpr (std::is_constructible<SharedState, const ocudu_dapp_host_api_v1&, const void*>::value) {
      return std::make_unique<SharedState>(host, args);
    } else if constexpr (std::is_constructible<SharedState, const void*>::value) {
      return std::make_unique<SharedState>(args);
    } else if constexpr (std::is_constructible<SharedState, const ocudu_dapp_host_api_v1&>::value) {
      return std::make_unique<SharedState>(host);
    } else if constexpr (std::is_default_constructible<SharedState>::value) {
      return std::make_unique<SharedState>();
    } else {
      static_assert(dependent_false<SharedState>::value, "unsupported dApp shared-state constructor");
    }
  }

  static std::unique_ptr<WorkerState> make_worker_state(SharedState& shared, uint32_t worker_id)
  {
    if constexpr (std::is_constructible<WorkerState, SharedState&, uint32_t>::value) {
      return std::make_unique<WorkerState>(shared, worker_id);
    } else if constexpr (std::is_constructible<WorkerState, const SharedState&, uint32_t>::value) {
      return std::make_unique<WorkerState>(static_cast<const SharedState&>(shared), worker_id);
    } else if constexpr (std::is_constructible<WorkerState, uint32_t>::value) {
      return std::make_unique<WorkerState>(worker_id);
    } else if constexpr (std::is_constructible<WorkerState, SharedState&>::value) {
      return std::make_unique<WorkerState>(shared);
    } else if constexpr (std::is_constructible<WorkerState, const SharedState&>::value) {
      return std::make_unique<WorkerState>(static_cast<const SharedState&>(shared));
    } else if constexpr (std::is_default_constructible<WorkerState>::value) {
      return std::make_unique<WorkerState>();
    } else {
      static_assert(dependent_false<WorkerState>::value, "unsupported dApp worker-state constructor");
    }
  }

  static ocudu_dapp_status_v1 create_worker(void* context, uint32_t worker_id, void** output) noexcept
  {
    if (context == nullptr || output == nullptr) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }
    *output     = nullptr;
    auto* owner = static_cast<owned_instance*>(static_cast<instance*>(context));
    try {
      auto worker   = std::make_unique<worker_context>();
      worker->owner = owner;
      worker->id    = worker_id;
      worker->local = make_worker_state(*owner->shared, worker_id);
      *output       = worker.release();
      return OCUDU_DAPP_OK_V1;
    } catch (const std::bad_alloc&) {
      return OCUDU_DAPP_RESOURCE_V1;
    } catch (...) {
      return OCUDU_DAPP_EXCEPTION_V1;
    }
  }

  static void destroy_worker(void*, void* worker) noexcept { delete static_cast<worker_context*>(worker); }

public:
  static ocudu_dapp_status_v1 create(const ocudu_dapp_host_api_v1* host, const void* args, void** output) noexcept
  {
    if (output == nullptr) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }
    *output = nullptr;
    try {
      auto owner  = std::make_unique<owned_instance>();
      auto status = initialize_instance(*owner, host);
      if (status != OCUDU_DAPP_OK_V1) {
        return status;
      }
      owner->shared = make_shared_state(*host, args);
      *output       = static_cast<instance*>(owner.release());
      return OCUDU_DAPP_OK_V1;
    } catch (const std::bad_alloc&) {
      return OCUDU_DAPP_RESOURCE_V1;
    } catch (...) {
      return OCUDU_DAPP_EXCEPTION_V1;
    }
  }

  static const ocudu_dapp_worker_api_v1& worker_api() noexcept
  {
    static const ocudu_dapp_worker_api_v1 api = [] {
      ocudu_dapp_worker_api_v1 result{};
      result.struct_size            = sizeof(result);
      result.abi_major              = OCUDU_DAPP_ABI_MAJOR_V1;
      result.abi_minor              = OCUDU_DAPP_ABI_MINOR_V1;
      result.create_worker_context  = create_worker;
      result.destroy_worker_context = destroy_worker;
      return result;
    }();
    return api;
  }

  static ocudu_dapp_status_v1 populate(const ocudu_dapp_host_info_v1* host, ocudu_dapp_module_v1* output,
                                       const char* name, const ocudu_dapp_interface_descriptor_v1* interface) noexcept
  {
    const auto status = reference::populate_module(host, output, name, interface);
    if (status == OCUDU_DAPP_OK_V1) {
      output->create = create;
    }
    return status;
  }

  /// Identity-aware populate for commercial packages (vendor/version match manifest).
  static ocudu_dapp_status_v1 populate(const ocudu_dapp_host_info_v1* host, ocudu_dapp_module_v1* output,
                                       const char* name, const char* vendor, const char* version,
                                       const ocudu_dapp_interface_descriptor_v1* interface) noexcept
  {
    const auto status = reference::populate_module(host, output, name, vendor, version, interface);
    if (status == OCUDU_DAPP_OK_V1) {
      output->create = create;
    }
    return status;
  }

  static SharedState* shared_state_from_instance(void* context) noexcept
  {
    if (context == nullptr) {
      return nullptr;
    }
    return static_cast<owned_instance*>(static_cast<instance*>(context))->shared.get();
  }

  static SharedState* shared_state(void* context) noexcept
  {
    auto* worker = static_cast<worker_context*>(context);
    return worker != nullptr ? worker->owner->shared.get() : nullptr;
  }

  static WorkerState* worker_state(void* context) noexcept
  {
    auto* worker = static_cast<worker_context*>(context);
    return worker != nullptr ? worker->local.get() : nullptr;
  }

  static uint32_t worker_id(void* context) noexcept
  {
    auto* worker = static_cast<worker_context*>(context);
    return worker != nullptr ? worker->id : 0;
  }

  static const ocudu_dapp_host_api_v1* host_api(void* context) noexcept
  {
    auto* worker = static_cast<worker_context*>(context);
    return worker != nullptr && worker->owner != nullptr ? &worker->owner->host : nullptr;
  }

  static ocudu_dapp_status_v1 begin_invocation(void* context) noexcept
  {
    auto* worker = static_cast<worker_context*>(context);
    return worker != nullptr ? reference::begin_invocation(*worker->owner) : OCUDU_DAPP_INVALID_INPUT_V1;
  }

  static void add_metric(void* context, uint32_t metric_id, uint64_t value) noexcept
  {
    auto* worker = static_cast<worker_context*>(context);
    if (worker != nullptr) {
      reference::add_metric(*worker->owner, metric_id, value);
    }
  }

  static void observe_input_layers(void* context, uint32_t nof_layers) noexcept
  {
    auto* worker = static_cast<worker_context*>(context);
    if (worker != nullptr) {
      reference::observe_input_layers(*worker->owner, nof_layers);
    }
  }
};

} // namespace ocudu::dapp::reference
