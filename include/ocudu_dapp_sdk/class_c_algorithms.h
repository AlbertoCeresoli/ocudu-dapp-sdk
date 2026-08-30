// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include <cstddef>
#include <cstdint>

namespace ocudu::dapp::reference {

/// Non-owning strided view of a complex tensor (cf32 or cbf16). `data` aliases
/// caller-managed memory (host or accelerator, per `memory_domain`) and stays
/// valid only while the underlying buffer/lease does.
struct strided_complex_view {
  const uint8_t* data{};
  uint64_t       byte_length{};
  uint32_t       element_type{OCUDU_DAPP_ELEMENT_CF32_V1};
  uint32_t       memory_domain{OCUDU_DAPP_MEM_HOST_V1};
  uint32_t       backend_id{OCUDU_DAPP_BACKEND_HOST_V1};
  /// Accelerator device ordinal; UINT32_MAX for host memory.
  uint32_t device_ordinal{UINT32_MAX};
  uint32_t rank{};
  uint64_t dimensions[OCUDU_DAPP_MAX_TENSOR_RANK_V1]{};
  uint64_t byte_strides[OCUDU_DAPP_MAX_TENSOR_RANK_V1]{};
};

/// Minimal reference radiometer; this is intentionally not a learned detector.
struct spectrum_radiometer_result {
  /// Samples inspected.
  uint64_t samples{};
  /// Mean / peak per-sample power after applying the amplitude scale (linear).
  double mean_power{};
  double peak_power{};
  /// Fraction of samples whose power exceeded the threshold.
  double occupied_fraction{};
};

/// Read-only committed UL-use map. Only PlannedEmpty cells are sensing opportunities.
struct expected_ul_use_view {
  /// `symbols` x `prbs` row-major grid of OCUDU_DAPP_UL_USE_* categories
  /// (non-owning).
  const uint8_t* data{};
  uint32_t       symbols{};
  uint32_t       prbs{};
  /// Generation of the committed plan this map belongs to (non-zero).
  uint64_t plan_generation{};
};

constexpr uint32_t max_reference_cir_taps = OCUDU_DAPP_SRS_ISAC_MAX_CIR_TAPS_V1;

/// Numeric result of \ref estimate_srs_cir_placeholder: a bounded early-delay
/// CIR summary (`tap_count` leading taps in `tap_power`) plus channel power
/// statistics.
struct srs_cir_analysis {
  uint64_t samples{};
  uint64_t channel_rows{};
  uint32_t tap_count{};
  uint32_t peak_delay_bin{};
  double   mean_channel_power{};
  double   noise_corrected_power{};
  double   peak_cir_power{};
  double   tap_power[max_reference_cir_taps]{};
};

/// Wrap a host-resident ABI tensor as a \ref strided_complex_view (validates
/// element type, domain, rank and extents). No copy.
bool make_host_complex_view(const ocudu_dapp_tensor_view_v1& input, strided_complex_view& output) noexcept;

/// Wrap a host-resident U8 symbol x PRB UL-use tensor as an
/// \ref expected_ul_use_view for \p plan_generation. No copy.
bool make_host_expected_ul_use_view(const ocudu_dapp_tensor_view_v1& input, uint64_t plan_generation,
                                    expected_ul_use_view& output) noexcept;

/// Whole-grid radiometer: mean/peak power (scaled by `amplitude_scale`^2) and
/// the fraction of samples above \p power_threshold. The view's samples are
/// read on the CPU, so `data` must be CPU-addressable.
bool run_spectrum_radiometer(const strided_complex_view& input, double amplitude_scale, double power_threshold,
                             spectrum_radiometer_result& output) noexcept;

/// Emits one binary U8 confidence per contiguous PRB using a simple average-power radiometer.
/// Input is a rank-3 [port][symbol][subcarrier] grid; on success `nof_prbs` =
/// subcarriers / \p subcarriers_per_prb and `confidence[prb]` is 255 when the
/// PRB's average sample power exceeds \p power_threshold, else 0. The optional
/// \p summary aggregates the whole grid.
bool run_spectrum_prb_radiometer(const strided_complex_view& input, double amplitude_scale, double power_threshold,
                                 uint32_t subcarriers_per_prb, uint8_t* confidence, uint32_t confidence_capacity,
                                 uint32_t& nof_prbs, spectrum_radiometer_result* summary = nullptr) noexcept;

/// Plan-aware variants: per-PRB averages include only symbols the committed UL
/// plan marks PlannedEmpty for that PRB (\p expected_use dimensions must match
/// the grid); the optional summary still covers every sample.
bool run_spectrum_prb_radiometer(const strided_complex_view& input, const expected_ul_use_view& expected_use,
                                 double amplitude_scale, double power_threshold, uint32_t subcarriers_per_prb,
                                 uint8_t* confidence, uint32_t confidence_capacity, uint32_t& nof_prbs) noexcept;
bool run_spectrum_prb_radiometer(const strided_complex_view& input, const expected_ul_use_view& expected_use,
                                 double amplitude_scale, double power_threshold, uint32_t subcarriers_per_prb,
                                 uint8_t* confidence, uint32_t confidence_capacity, uint32_t& nof_prbs,
                                 spectrum_radiometer_result* summary) noexcept;

/// Forms a bounded early-delay CIR preview for transport and E2E validation.
bool estimate_srs_cir_placeholder(const strided_complex_view& input, double noise_variance,
                                  srs_cir_analysis& output) noexcept;

/// Converts the numeric placeholder result into the bounded public report body.
bool make_srs_cir_preview(const srs_cir_analysis& analysis, float noise_variance,
                          ocudu_dapp_srs_isac_cir_preview_v1& output) noexcept;

} // namespace ocudu::dapp::reference
