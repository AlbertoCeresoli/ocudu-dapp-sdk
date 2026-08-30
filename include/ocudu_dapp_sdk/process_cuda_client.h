// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu_dapp_sdk/process_client.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace ocudu::dapp::sdk {

class cuda_process_route;

/// Outcome of \ref cuda_process_route::create: the imported route on success
/// (bool conversion), otherwise null plus a human-readable failure reason.
struct cuda_process_route_create_result {
  std::unique_ptr<cuda_process_route> route;
  std::string                         detail;
  explicit                            operator bool() const noexcept { return route != nullptr; }
};

/// One publication's tensor as device memory ready for consumption.
struct cuda_publication_view {
  /// Device pointer to the imported tensor bytes (despite the host-pointer
  /// type); dereference it only in device code / CUDA APIs.
  const uint8_t* data{};
  /// Tensor size in bytes.
  std::size_t size{};
  /// The route-owned `cudaStream_t` (as an integer). \ref
  /// cuda_process_route::prepare has already enqueued a wait for the
  /// producer's ready event on this stream, so work launched on it is
  /// correctly ordered after the producer's writes. The route never
  /// synchronises it between publications — the worker must synchronise (or
  /// order against) this stream before releasing the publication_lease and
  /// before reading results on the host. The destructor of the route
  /// synchronises it one final time.
  uintptr_t execution_stream{};
  /// CUDA device ordinal the pool lives on.
  uint32_t device_ordinal{};
};

/// Imports one fixed host-owned CUDA export pool. The worker must complete all
/// device use before releasing the corresponding publication_lease.
///
/// `create` opens every slot's CUDA IPC memory and ready-event handle on the
/// pool's device and creates one non-blocking stream owned by the route; the
/// destructor synchronises that stream and closes all imported handles.
class cuda_process_route
{
public:
  /// Import the route's admitted CUDA export pool; fails (with detail) when
  /// the route has no pool or an IPC handle cannot be opened.
  static cuda_process_route_create_result create(const process_route& route) noexcept;

  virtual ~cuda_process_route() = default;
  /// Map one external-tensor publication to its device slot: selects the
  /// device, enqueues a wait for the slot's ready event on the route stream,
  /// and returns the view. Returns nullopt for publications that do not carry
  /// an external tensor, reference an invalid slot, or when a CUDA call fails.
  virtual std::optional<cuda_publication_view>
  prepare(const ocudu_dapp_publication_record_view_v1& publication) noexcept = 0;
};

} // namespace ocudu::dapp::sdk
