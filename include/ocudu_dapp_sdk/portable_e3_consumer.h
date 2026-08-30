// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/e3/local_client.h"
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ocudu::dapp::reference {

/// Configuration for one portable E3 data subscription. Every field is
/// validated by \ref portable_e3_consumer::create; the noted bounds are the
/// accepted ranges.
struct portable_e3_consumer_config {
  /// Path of the DU's local E3 management UNIX socket.
  std::string socket_path;
  /// Caller-chosen non-zero subscription identifier.
  management::identifier subscription_id;
  /// OCUDU_DAPP_INTERFACE_SPECTRUM_V1 or OCUDU_DAPP_INTERFACE_SRS_ISAC_V1.
  uint32_t interface_id{};
  /// Optional cell filter (0-65535); all cells when unset.
  std::optional<uint32_t> cell_id;
  /// Keep every Nth publication (1-65535); 1 = every object the agent offers.
  uint32_t sample_every_n{1};
  /// Agent-side queue depth in chunks (1-1024).
  uint32_t queue_depth_chunks{64};
  /// Maximum wire chunk size in bytes (1024-65536).
  uint32_t maximum_chunk_bytes{64 * 1024};
  /// Subscription lease requested from the agent, in microseconds (up to 60 s).
  uint64_t lease_us{1'000'000};
  /// Upper bound on a reassembled tensor in bytes (up to 64 MiB); larger
  /// publications are rejected as invalid.
  uint64_t maximum_tensor_bytes{4 * 1024 * 1024};
  /// Per-request timeout for subscribe/poll/unsubscribe calls (up to 1 min).
  std::chrono::milliseconds timeout{1000};
};

/// One complete Class-C publication reassembled from its chunks.
struct portable_e3_object {
  uint32_t interface_id{};
  uint32_t cell_id{};
  /// Producer-side sequence number of this publication.
  uint64_t publication_sequence{};
  /// Route epoch; changes when the producer route is recreated.
  uint64_t route_epoch{};
  /// Cumulative count of publications this subscription missed (bounded queue).
  uint64_t dropped_objects{};
  /// Interface-specific metadata blob (from the first chunk), APER-opaque bytes.
  std::vector<uint8_t> metadata;
  /// Reassembled tensor payload bytes.
  std::vector<uint8_t> tensor;
};

/// Outcome of one \ref portable_e3_consumer::poll call.
enum class portable_e3_poll_status : uint8_t { no_data, object, error };

/// Poll result: `object` is engaged only when `status == object`; `detail`
/// carries a human-readable reason when `status == error`.
struct portable_e3_poll_result {
  portable_e3_poll_status           status{portable_e3_poll_status::error};
  std::optional<portable_e3_object> object;
  std::string                       detail;
};

/// Independently running, bounded APER fallback consumer for low-rate Class-C data.
///
/// Fully synchronous: every method performs blocking request/response calls on
/// the caller's thread (no background thread). Not thread-safe — drive one
/// instance from a single thread.
class portable_e3_consumer
{
public:
  /// Validate \p config and build a consumer. On failure the pointer is null
  /// and the string carries the reason. Creation does not touch the socket.
  static std::pair<std::unique_ptr<portable_e3_consumer>, std::string>
  create(portable_e3_consumer_config config) noexcept;

  ~portable_e3_consumer();
  portable_e3_consumer(const portable_e3_consumer&)            = delete;
  portable_e3_consumer& operator=(const portable_e3_consumer&) = delete;

  /// Create the data subscription on the agent (and reset any partial
  /// assembly). Returns false with \p detail on failure.
  bool subscribe(std::string& detail) noexcept;
  /// Fetch and reassemble pending publication chunks (one bounded request of
  /// at most 8 chunks per call). Returns `object` when a publication
  /// completes, `no_data` when the queue is drained, `error` (assembly state
  /// discarded) on a malformed or noncontiguous chunk sequence.
  portable_e3_poll_result poll() noexcept;
  /// Remove the subscription on the agent and discard local assembly state.
  bool unsubscribe(std::string& detail) noexcept;

private:
  struct implementation;
  explicit portable_e3_consumer(std::unique_ptr<implementation> state_);
  std::unique_ptr<implementation> state;
};

} // namespace ocudu::dapp::reference
