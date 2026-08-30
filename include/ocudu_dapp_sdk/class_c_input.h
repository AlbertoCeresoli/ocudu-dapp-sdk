// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu_dapp_sdk/class_c_algorithms.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace ocudu::dapp::reference {

/// Kind of one decoded Class-C publication: an uplink resource-grid snapshot
/// (`spectrum`), an SRS channel-response snapshot (`srs_isac`), or a
/// tensor-less advance uplink resource plan (`ul_resource_plan`).
enum class class_c_input_kind : uint8_t { spectrum, srs_isac, ul_resource_plan };

/// One validated Class-C publication. Views (`tensor`, `expected_ul_use`)
/// alias the caller's metadata/tensor buffers — they stay valid only while
/// those buffers do, and for tensors only until the producer lease expires
/// (\ref class_c_input_expired).
struct class_c_input {
  class_c_input_kind kind{};
  /// Payload view (spectrum: [rx_port][symbol][subcarrier]; srs_isac:
  /// [rx_port][tx_port][frequency_point]); empty for `ul_resource_plan`.
  strided_complex_view tensor;
  /// Spectrum only, when the publication carries occupancy annotations: the
  /// scheduler's expected UL use per symbol x PRB and its plan generation.
  expected_ul_use_view expected_ul_use;
  /// Producer publication sequence (equals `plan_generation` for plans).
  uint64_t source_sequence{};
  /// Producer monotonic capture time in nanoseconds.
  uint64_t observation_time_ns{};
  /// Monotonic expiry of the producer's tensor lease in nanoseconds (0 for plans).
  uint64_t lease_expiry_ns{};
  uint32_t cell_id{};
  /// SRS resource / configuration identifiers (`srs_isac` only).
  uint32_t                 srs_resource_id{};
  uint32_t                 srs_config_id{};
  ocudu_dapp_radio_time_v1 radio_time{};
  /// `ul_resource_plan` only: the first `nof_plan_spans` validated spans.
  std::array<ocudu_dapp_ul_resource_span_v1, OCUDU_DAPP_UL_RESOURCE_PLAN_MAX_SPANS_V1> plan_spans{};
  uint64_t                                                                             plan_generation{};
  uint32_t                                                                             nof_plan_spans{};
  /// Cell width in PRBs (`ul_resource_plan` only).
  uint16_t cell_nof_prbs{};
  /// Whether the plan covers the complete slot (`ul_resource_plan` only).
  bool plan_complete{};
  /// Spectrum amplitude scale from the producer (`spectrum` only, > 0).
  double amplitude_scale{1.0};
  /// Estimated noise variance from the producer (`srs_isac` only, >= 0).
  double noise_variance{};
};

/// Decode and validate one Class-C publication (FlatBuffers
/// `PublicationEnvelope` metadata + raw tensor bytes) for
/// \p expected_interface_id (spectrum or SRS-ISAC). Verifies the schema id and
/// version, radio time, per-kind metadata consistency, and that the tensor
/// descriptor (element type, rank, dimensions, strides) fits inside
/// \p tensor_size. On success fills \p output with views into the caller's
/// buffers (no copy of the samples); on any inconsistency returns false and
/// leaves \p output untouched.
bool decode_class_c_input(const uint8_t* metadata, std::size_t metadata_size, const uint8_t* tensor,
                          std::size_t tensor_size, uint32_t expected_interface_id, class_c_input& output) noexcept;

/// Returns true once the producer's bounded tensor lease is no longer valid.
/// Advance UL plans carry no tensor lease and therefore do not expire here.
bool class_c_input_expired(const class_c_input& input, uint64_t monotonic_time_ns) noexcept;

} // namespace ocudu::dapp::reference
