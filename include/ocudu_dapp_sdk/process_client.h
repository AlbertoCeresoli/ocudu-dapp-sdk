// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Worker-side transport client for supervised (Class-C) processes.
//
// A spawned worker uses this header to authenticate its supervisor, receive
// route offers over the inherited control descriptor, map the shared-memory
// publication rings, and consume zero-copy publication leases. It links no
// OCUDU runtime code. Typical worker flow (see reference/process/worker_main.cpp):
//
//   bind_parent_lifetime(getppid());
//   apply_restricted_process_policy();
//   auto routes = receive_route_set(control_fd, parent_pid, uid, interface, timeout);
//   while (routes.route_set->wait(poll)) { ... try_acquire() ... }

#pragma once

#include "ocudu/dapp/process/v1/control.h"
#include "ocudu/dapp/process/v1/cuda_export.h"
#include "ocudu/dapp/process/v1/publication_record.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace ocudu::dapp::sdk {

struct route_mapping;
struct route_set_mapping;
struct route_set_receive_result;

/// Optional package-management surface of the worker: the management vtable
/// and the instance passed to it. When set, management/config requests that
/// arrive on the process control channel are dispatched to this binding while
/// the route (set) services control traffic. Both pointers are borrowed and
/// must outlive the route set.
struct process_management_binding {
  const ocudu_dapp_management_v1* api{};
  void*                           instance{};
};

/// One interface the worker expects in a multi-interface bundle
/// (\ref receive_route_bundle). A `required` interface must be bound for the
/// receive to succeed; an optional one may be reported unavailable.
struct process_interface_expectation {
  uint32_t interface_id{};
  bool     required{};
};

/// Non-owning view of the route's admitted host-owned CUDA export pool: the
/// pool header plus `header->slot_count` slots carrying CUDA IPC memory and
/// ready-event handles. Pointers alias the route's shared-memory mapping and
/// stay valid only while the route does.
struct cuda_export_pool_view {
  const ocudu_dapp_cuda_export_header_v1* header{};
  const ocudu_dapp_cuda_export_slot_v1*   slots{};
};

/// RAII read lease on one publication slot in the producer's shared-memory
/// ring. `data()` aliases producer-owned shared memory and is valid only while
/// the lease is held. Destruction (or \ref reset) signals completion back to
/// the producer and advances the consumer read index — unless the route epoch
/// changed underneath, in which case the stale slot is silently abandoned.
/// At most one lease can be active per route at a time.
class publication_lease
{
public:
  publication_lease() noexcept = default;
  publication_lease(publication_lease&& other) noexcept;
  publication_lease& operator=(publication_lease&& other) noexcept;
  ~publication_lease();

  publication_lease(const publication_lease&)            = delete;
  publication_lease& operator=(const publication_lease&) = delete;

  const uint8_t*                                       data() const noexcept { return bytes; }
  std::size_t                                          size() const noexcept { return byte_count; }
  uint32_t                                             flags() const noexcept { return entry_flags; }
  uint64_t                                             sequence() const noexcept { return entry_sequence; }
  std::optional<ocudu_dapp_publication_record_view_v1> publication() const noexcept;
  explicit                                             operator bool() const noexcept { return owner != nullptr; }
  void                                                 reset() noexcept;

private:
  friend class process_route;
  publication_lease(std::shared_ptr<route_mapping> owner_, const uint8_t* bytes_, std::size_t size_, uint32_t flags_,
                    uint64_t sequence_, uint64_t epoch_) noexcept;

  std::shared_ptr<route_mapping> owner;
  const uint8_t*                 bytes{};
  std::size_t                    byte_count{};
  uint32_t                       entry_flags{};
  uint64_t                       entry_sequence{};
  uint64_t                       acquired_epoch{};
};

/// Consumer end of one shared-memory publication route. Designed for a
/// single consuming thread; methods are not synchronized against each other.
class process_route
{
public:
  // One process_route consumes one producer shard. A worker receiving multiple
  // shard offers must poll each route independently and merge by radio time.
  explicit process_route(std::shared_ptr<route_mapping> mapping_) : mapping(std::move(mapping_)) {}

  /// The producer's route offer (interface, shard, capacities) as admitted.
  const ocudu_dapp_route_offer_v1& offer() const noexcept;
  /// Generation stamped at route admission; results and control messages are
  /// validated against it.
  uint64_t route_generation() const noexcept;
  /// Block up to \p timeout for a publication notification. Returns true when
  /// the notification descriptor became readable.
  bool wait(std::chrono::milliseconds timeout) noexcept;
  /// Consume the pending notification counter; returns the number of
  /// notifications drained.
  uint64_t drain_notifications() noexcept;
  /// Snapshot of unread publications, including an active lease. This is a
  /// bounded hint for latest-wins consumers; the producer may advance it.
  uint32_t pending_publications() const noexcept;
  /// Non-blocking: lease the oldest unread publication. Returns nullopt when
  /// the ring is empty, a slot is inconsistent, or another lease from this
  /// route is still alive (one lease at a time).
  std::optional<publication_lease> try_acquire() noexcept;
  /// View of the admitted CUDA export pool, or nullopt for host-memory routes.
  std::optional<cuda_export_pool_view> cuda_export_pool() const noexcept;
  /// Validate and send one result record to the host (interface must match the
  /// route). Returns false on validation or transport failure.
  bool send_spectrum_result(ocudu_dapp_process_spectrum_result_v1 result) noexcept;
  bool send_quiet_intent_result(ocudu_dapp_process_quiet_intent_result_v1 result) noexcept;
  bool send_srs_isac_result(ocudu_dapp_process_srs_isac_result_v1 result) noexcept;

private:
  friend struct route_set_mapping;
  std::shared_ptr<route_mapping> mapping;
};

/// Owns the routes and interface bindings received in one handshake, plus the
/// process control channel. Single-threaded like \ref process_route.
class process_route_set
{
public:
  ~process_route_set();

  process_route_set(const process_route_set&)            = delete;
  process_route_set& operator=(const process_route_set&) = delete;

  std::size_t size() const noexcept;
  /// Route \p index in [0, size()); throws std::out_of_range beyond that.
  process_route&                                 route(std::size_t index);
  std::size_t                                    interface_count() const noexcept;
  const ocudu_dapp_process_interface_binding_v1& interface_binding(std::size_t index) const;
  /// Block up to \p timeout for data on any route while also servicing the
  /// control channel (heartbeats, management requests). Returns true when at
  /// least one route has a publication pending; false on timeout or when the
  /// control channel failed (check \ref control_healthy).
  bool wait(std::chrono::milliseconds timeout) noexcept;
  /// False once control-channel servicing failed; the worker should exit.
  bool control_healthy() const noexcept;

private:
  friend route_set_receive_result receive_route_set(int, pid_t, uid_t, uint32_t, std::chrono::milliseconds,
                                                    process_management_binding);
  friend route_set_receive_result receive_route_bundle(int, pid_t, uid_t, const process_interface_expectation*,
                                                       std::size_t, std::chrono::milliseconds,
                                                       process_management_binding);
  process_route_set(std::vector<std::unique_ptr<process_route>>          routes,
                    std::vector<ocudu_dapp_process_interface_binding_v1> interfaces,
                    process_management_binding                           management);
  std::unique_ptr<route_set_mapping> mapping;
};

/// Outcome of \ref receive_route: the bound route on success (bool
/// conversion), otherwise a null route and a human-readable failure reason.
struct route_receive_result {
  std::unique_ptr<process_route> route;
  std::string                    detail;
  explicit                       operator bool() const noexcept { return route != nullptr; }
};

/// Bind this worker to its supervising process. Returns false if the parent
/// already changed or the kernel cannot guarantee SIGKILL on parent death.
bool bind_parent_lifetime(pid_t expected_parent_pid) noexcept;

/// Installs the reference Linux worker policy after startup and before route
/// admission. The policy sets no_new_privs and a TSYNC seccomp deny-list for
/// process injection, new executable images, kernel/module administration and
/// other privileged interfaces. It deliberately leaves ordinary file and
/// device syscalls available for CUDA runtime initialization.
bool apply_restricted_process_policy() noexcept;

/// Single-route variant of \ref receive_route_set: authenticates the
/// supervising peer and accepts exactly one route offer on \p control_fd.
/// Prefer receive_route_set for shard-aware workers.
route_receive_result receive_route(int control_fd, pid_t expected_pid, uid_t expected_uid);

/// Outcome of \ref receive_route_set / \ref receive_route_bundle: the route
/// set on success (bool conversion), otherwise null plus a failure reason.
struct route_set_receive_result {
  std::unique_ptr<process_route_set> route_set;
  std::string                        detail;
  explicit                           operator bool() const noexcept { return route_set != nullptr; }
};

/// Run the supervised-worker handshake on \p control_fd: authenticate the
/// supervising peer (\p expected_pid / \p expected_uid), accept the shard
/// offers for \p expected_interface_id, and map their shared-memory rings.
/// Blocks up to \p timeout. \p management, when non-empty, answers
/// management/config requests arriving on the control channel afterwards.
route_set_receive_result receive_route_set(int control_fd, pid_t expected_pid, uid_t expected_uid,
                                           uint32_t expected_interface_id, std::chrono::milliseconds timeout,
                                           process_management_binding management = {});

/// Receives one manifest-ordered multi-interface bundle. Required expectations
/// must be bound; optional expectations may be reported unavailable. The
/// original receive_route_set handshake remains unchanged for one interface.
route_set_receive_result receive_route_bundle(int control_fd, pid_t expected_pid, uid_t expected_uid,
                                              const process_interface_expectation* expectations,
                                              std::size_t expectation_count, std::chrono::milliseconds timeout,
                                              process_management_binding management = {});

} // namespace ocudu::dapp::sdk
