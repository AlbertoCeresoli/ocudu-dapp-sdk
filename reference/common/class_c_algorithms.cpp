// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/class_c_algorithms.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

using namespace ocudu::dapp::reference;

namespace {

constexpr uint64_t max_reference_samples = 64U * 1024U * 1024U;

struct complex32 {
  float real;
  float imag;
};

uint64_t element_size(const strided_complex_view& view) noexcept
{
  if (view.element_type == OCUDU_DAPP_ELEMENT_CF32_V1) {
    return sizeof(complex32);
  }
  return view.element_type == OCUDU_DAPP_ELEMENT_CBF16_V1 ? sizeof(uint16_t) * 2 : 0;
}

bool validate(const strided_complex_view& view, uint64_t& count) noexcept
{
  const uint64_t item_size = element_size(view);
  if (view.data == nullptr || item_size == 0 || view.rank == 0 || view.rank > OCUDU_DAPP_MAX_TENSOR_RANK_V1) {
    return false;
  }
  uint64_t extent = item_size;
  count           = 1;
  for (uint32_t axis = 0; axis != view.rank; ++axis) {
    const uint64_t dimension = view.dimensions[axis];
    const uint64_t stride    = view.byte_strides[axis];
    if (dimension == 0 || stride == 0 || dimension > max_reference_samples / count ||
        (dimension - 1) > (std::numeric_limits<uint64_t>::max() - extent) / stride) {
      return false;
    }
    count *= dimension;
    extent += (dimension - 1) * stride;
  }
  return count <= max_reference_samples && extent <= view.byte_length;
}

float bf16_to_float(uint16_t value) noexcept
{
  const uint32_t bits = static_cast<uint32_t>(value) << 16U;
  float          result{};
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

complex32 load(const strided_complex_view&                                view,
               const std::array<uint64_t, OCUDU_DAPP_MAX_TENSOR_RANK_V1>& index) noexcept
{
  uint64_t offset = 0;
  for (uint32_t axis = 0; axis != view.rank; ++axis) {
    offset += index[axis] * view.byte_strides[axis];
  }
  if (view.element_type == OCUDU_DAPP_ELEMENT_CF32_V1) {
    complex32 value{};
    std::memcpy(&value, view.data + offset, sizeof(value));
    return value;
  }
  uint16_t value[2]{};
  std::memcpy(value, view.data + offset, sizeof(value));
  return {bf16_to_float(value[0]), bf16_to_float(value[1])};
}

void advance(const strided_complex_view& view, std::array<uint64_t, OCUDU_DAPP_MAX_TENSOR_RANK_V1>& index) noexcept
{
  for (uint32_t axis = view.rank; axis != 0; --axis) {
    if (++index[axis - 1] != view.dimensions[axis - 1]) {
      return;
    }
    index[axis - 1] = 0;
  }
}

double power(complex32 value) noexcept
{
  return static_cast<double>(value.real) * value.real + static_cast<double>(value.imag) * value.imag;
}

bool run_prb_radiometer(const strided_complex_view& input, const expected_ul_use_view* expected_use,
                        double amplitude_scale, double power_threshold, uint32_t subcarriers_per_prb,
                        uint8_t* confidence, uint32_t confidence_capacity, uint32_t& nof_prbs,
                        spectrum_radiometer_result* summary) noexcept
{
  uint64_t count = 0;
  nof_prbs       = 0;
  if (!validate(input, count) || input.rank != 3 || !std::isfinite(amplitude_scale) || amplitude_scale <= 0.0 ||
      !std::isfinite(power_threshold) || power_threshold < 0.0 || subcarriers_per_prb == 0 || confidence == nullptr ||
      input.dimensions[2] % subcarriers_per_prb != 0 || input.dimensions[2] / subcarriers_per_prb == 0 ||
      input.dimensions[2] / subcarriers_per_prb > confidence_capacity) {
    return false;
  }

  nof_prbs = static_cast<uint32_t>(input.dimensions[2] / subcarriers_per_prb);
  if (expected_use != nullptr && (expected_use->data == nullptr || expected_use->plan_generation == 0 ||
                                  expected_use->symbols != input.dimensions[1] || expected_use->prbs != nof_prbs)) {
    nof_prbs = 0;
    return false;
  }
  const double                                        scale_squared    = amplitude_scale * amplitude_scale;
  double                                              total_sum        = 0.0;
  double                                              total_peak       = 0.0;
  uint64_t                                            total_samples    = 0;
  uint64_t                                            occupied_samples = 0;
  std::array<uint64_t, OCUDU_DAPP_MAX_TENSOR_RANK_V1> index{};
  for (uint32_t prb = 0; prb != nof_prbs; ++prb) {
    double   sum              = 0.0;
    uint64_t included_symbols = 0;
    for (uint64_t symbol = 0; symbol != input.dimensions[1]; ++symbol) {
      const bool include = expected_use == nullptr ||
                           expected_use->data[symbol * expected_use->prbs + prb] == OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1;
      if (!include && summary == nullptr) {
        continue;
      }
      included_symbols += include;
      index[1] = symbol;
      for (uint64_t port = 0; port != input.dimensions[0]; ++port) {
        index[0] = port;
        for (uint32_t subcarrier = 0; subcarrier != subcarriers_per_prb; ++subcarrier) {
          index[2]                  = static_cast<uint64_t>(prb) * subcarriers_per_prb + subcarrier;
          const double sample_power = power(load(input, index)) * scale_squared;
          if (!std::isfinite(sample_power)) {
            nof_prbs = 0;
            return false;
          }
          if (include) {
            sum += sample_power;
          }
          if (summary != nullptr) {
            total_sum += sample_power;
            total_peak = std::max(total_peak, sample_power);
            occupied_samples += sample_power > power_threshold;
            ++total_samples;
          }
        }
      }
    }
    const uint64_t samples = included_symbols * input.dimensions[0] * subcarriers_per_prb;
    confidence[prb]        = samples != 0 && sum / static_cast<double>(samples) > power_threshold ? UINT8_MAX : 0;
  }
  if (summary != nullptr) {
    if (total_samples != count) {
      nof_prbs = 0;
      return false;
    }
    *summary = {total_samples, total_sum / static_cast<double>(total_samples), total_peak,
                static_cast<double>(occupied_samples) / static_cast<double>(total_samples)};
    if (!std::isfinite(summary->mean_power)) {
      nof_prbs = 0;
      return false;
    }
  }
  return true;
}

} // namespace

bool ocudu::dapp::reference::make_host_complex_view(const ocudu_dapp_tensor_view_v1& input,
                                                    strided_complex_view&            output) noexcept
{
  constexpr uint32_t minimum = offsetof(ocudu_dapp_tensor_view_v1, reserved);
  const bool host_domain = input.domain == OCUDU_DAPP_MEM_HOST_V1 || input.domain == OCUDU_DAPP_MEM_HOST_PINNED_V1 ||
                           input.domain == OCUDU_DAPP_MEM_MANAGED_COHERENT_V1 ||
                           input.domain == OCUDU_DAPP_MEM_POSIX_SHARED_V1;
  if (input.struct_size < minimum || input.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 || !host_domain ||
      (input.element_type != OCUDU_DAPP_ELEMENT_CF32_V1 && input.element_type != OCUDU_DAPP_ELEMENT_CBF16_V1) ||
      (input.access & (OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1)) !=
          (OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1) ||
      input.native_handle == 0 || input.native_handle > std::numeric_limits<uintptr_t>::max() ||
      input.allocation_id == 0 || input.allocation_generation == 0 || input.lease_token == 0 || input.rank == 0 ||
      input.rank > OCUDU_DAPP_MAX_TENSOR_RANK_V1 || input.byte_length == 0 ||
      input.byte_offset > std::numeric_limits<uintptr_t>::max() - input.native_handle) {
    return false;
  }
  output      = {};
  output.data = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(input.native_handle + input.byte_offset));
  output.byte_length  = input.byte_length;
  output.element_type = input.element_type;
  output.rank         = input.rank;
  std::copy_n(input.dimensions, input.rank, output.dimensions);
  std::copy_n(input.byte_strides, input.rank, output.byte_strides);
  uint64_t ignored = 0;
  return validate(output, ignored);
}

bool ocudu::dapp::reference::make_host_expected_ul_use_view(const ocudu_dapp_tensor_view_v1& input,
                                                            uint64_t                         plan_generation,
                                                            expected_ul_use_view&            output) noexcept
{
  constexpr uint32_t minimum = offsetof(ocudu_dapp_tensor_view_v1, reserved);
  const bool host_domain = input.domain == OCUDU_DAPP_MEM_HOST_V1 || input.domain == OCUDU_DAPP_MEM_HOST_PINNED_V1 ||
                           input.domain == OCUDU_DAPP_MEM_MANAGED_COHERENT_V1 ||
                           input.domain == OCUDU_DAPP_MEM_POSIX_SHARED_V1;
  if (input.struct_size < minimum || input.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 || !host_domain ||
      input.element_type != OCUDU_DAPP_ELEMENT_U8_V1 || input.rank != 2 ||
      (input.access & (OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1)) !=
          (OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1) ||
      input.native_handle == 0 || input.dimensions[0] == 0 || input.dimensions[0] > 14 || input.dimensions[1] == 0 ||
      input.dimensions[1] > OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1 || input.byte_strides[0] != input.dimensions[1] ||
      input.byte_strides[1] != sizeof(uint8_t) || input.byte_length != input.dimensions[0] * input.dimensions[1] ||
      input.layout_id != OCUDU_DAPP_LAYOUT_EXPECTED_UL_USE_SYMBOL_PRB_U8_V1 || input.layout_version != 1 ||
      input.allocation_id == 0 || input.lease_token == 0 || plan_generation == 0 ||
      input.allocation_generation != plan_generation ||
      input.byte_offset > std::numeric_limits<uintptr_t>::max() - input.native_handle) {
    return false;
  }
  const auto* data = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(input.native_handle + input.byte_offset));
  if (!std::all_of(data, data + input.byte_length,
                   [](uint8_t value) { return value <= OCUDU_DAPP_UL_USE_RESERVED_QUIET_V1; })) {
    return false;
  }
  output = {data, static_cast<uint32_t>(input.dimensions[0]), static_cast<uint32_t>(input.dimensions[1]),
            plan_generation};
  return true;
}

bool ocudu::dapp::reference::run_spectrum_radiometer(const strided_complex_view& input, double amplitude_scale,
                                                     double                      power_threshold,
                                                     spectrum_radiometer_result& output) noexcept
{
  uint64_t count = 0;
  if (!validate(input, count) || !std::isfinite(amplitude_scale) || amplitude_scale <= 0.0 ||
      !std::isfinite(power_threshold) || power_threshold < 0.0) {
    return false;
  }
  const double                                        scale_squared = amplitude_scale * amplitude_scale;
  double                                              sum           = 0.0;
  double                                              peak          = 0.0;
  uint64_t                                            occupied      = 0;
  std::array<uint64_t, OCUDU_DAPP_MAX_TENSOR_RANK_V1> index{};
  for (uint64_t sample = 0; sample != count; ++sample) {
    const double sample_power = power(load(input, index)) * scale_squared;
    if (!std::isfinite(sample_power)) {
      return false;
    }
    sum += sample_power;
    peak = std::max(peak, sample_power);
    occupied += sample_power > power_threshold;
    advance(input, index);
  }
  output = {count, sum / count, peak, static_cast<double>(occupied) / count};
  return std::isfinite(output.mean_power);
}

bool ocudu::dapp::reference::run_spectrum_prb_radiometer(const strided_complex_view& input, double amplitude_scale,
                                                         double power_threshold, uint32_t subcarriers_per_prb,
                                                         uint8_t* confidence, uint32_t confidence_capacity,
                                                         uint32_t&                   nof_prbs,
                                                         spectrum_radiometer_result* summary) noexcept
{
  return run_prb_radiometer(input, nullptr, amplitude_scale, power_threshold, subcarriers_per_prb, confidence,
                            confidence_capacity, nof_prbs, summary);
}

bool ocudu::dapp::reference::run_spectrum_prb_radiometer(const strided_complex_view& input,
                                                         const expected_ul_use_view& expected_use,
                                                         double amplitude_scale, double power_threshold,
                                                         uint32_t subcarriers_per_prb, uint8_t* confidence,
                                                         uint32_t confidence_capacity, uint32_t& nof_prbs) noexcept
{
  return run_prb_radiometer(input, &expected_use, amplitude_scale, power_threshold, subcarriers_per_prb, confidence,
                            confidence_capacity, nof_prbs, nullptr);
}

bool ocudu::dapp::reference::run_spectrum_prb_radiometer(const strided_complex_view& input,
                                                         const expected_ul_use_view& expected_use,
                                                         double amplitude_scale, double power_threshold,
                                                         uint32_t subcarriers_per_prb, uint8_t* confidence,
                                                         uint32_t confidence_capacity, uint32_t& nof_prbs,
                                                         spectrum_radiometer_result* summary) noexcept
{
  return run_prb_radiometer(input, &expected_use, amplitude_scale, power_threshold, subcarriers_per_prb, confidence,
                            confidence_capacity, nof_prbs, summary);
}

bool ocudu::dapp::reference::estimate_srs_cir_placeholder(const strided_complex_view& input, double noise_variance,
                                                          srs_cir_analysis& output) noexcept
{
  uint64_t count = 0;
  if (!validate(input, count) || !std::isfinite(noise_variance) || noise_variance < 0.0) {
    return false;
  }
  const uint32_t frequency_axis   = input.rank - 1;
  const uint64_t frequency_points = input.dimensions[frequency_axis];
  const uint64_t rows             = count / frequency_points;
  const uint32_t taps             = static_cast<uint32_t>(std::min<uint64_t>(frequency_points, max_reference_cir_taps));
  double         sum_power        = 0.0;
  output                          = {};
  output.samples                  = count;
  output.channel_rows             = rows;
  output.tap_count                = taps;
  constexpr double                           two_pi = 6.283185307179586476925286766559;
  std::array<double, max_reference_cir_taps> step_real{};
  std::array<double, max_reference_cir_taps> step_imag{};
  for (uint32_t tap = 0; tap != taps; ++tap) {
    const double angle = two_pi * static_cast<double>(tap) / static_cast<double>(frequency_points);
    step_real[tap]     = std::cos(angle);
    step_imag[tap]     = std::sin(angle);
  }
  for (uint64_t row = 0; row != rows; ++row) {
    std::array<uint64_t, OCUDU_DAPP_MAX_TENSOR_RANK_V1> index{};
    uint64_t                                            remaining = row;
    for (uint32_t axis = frequency_axis; axis != 0; --axis) {
      index[axis - 1] = remaining % input.dimensions[axis - 1];
      remaining /= input.dimensions[axis - 1];
    }
    for (uint32_t tap = 0; tap != taps; ++tap) {
      double cir_real     = 0.0;
      double cir_imag     = 0.0;
      double twiddle_real = 1.0;
      double twiddle_imag = 0.0;
      for (uint64_t frequency = 0; frequency != frequency_points; ++frequency) {
        index[frequency_axis] = frequency;
        const complex32 value = load(input, index);
        cir_real += static_cast<double>(value.real) * twiddle_real - static_cast<double>(value.imag) * twiddle_imag;
        cir_imag += static_cast<double>(value.real) * twiddle_imag + static_cast<double>(value.imag) * twiddle_real;
        const double next_real = twiddle_real * step_real[tap] - twiddle_imag * step_imag[tap];
        twiddle_imag           = twiddle_real * step_imag[tap] + twiddle_imag * step_real[tap];
        twiddle_real           = next_real;
        if (tap == 0) {
          const double sample_power = power(value);
          if (!std::isfinite(sample_power)) {
            return false;
          }
          sum_power += sample_power;
        }
      }
      cir_real /= frequency_points;
      cir_imag /= frequency_points;
      output.tap_power[tap] += cir_real * cir_real + cir_imag * cir_imag;
    }
  }
  for (uint32_t tap = 0; tap != taps; ++tap) {
    output.tap_power[tap] /= rows;
    if (output.tap_power[tap] > output.peak_cir_power) {
      output.peak_cir_power = output.tap_power[tap];
      output.peak_delay_bin = tap;
    }
  }
  output.mean_channel_power    = sum_power / count;
  output.noise_corrected_power = std::max(0.0, output.mean_channel_power - noise_variance);
  return std::isfinite(output.mean_channel_power) && std::isfinite(output.peak_cir_power);
}

bool ocudu::dapp::reference::make_srs_cir_preview(const srs_cir_analysis& analysis, float noise_variance,
                                                  ocudu_dapp_srs_isac_cir_preview_v1& output) noexcept
{
  const auto valid_float = [](double value) {
    return std::isfinite(value) && value >= 0.0 && value <= std::numeric_limits<float>::max();
  };
  if (analysis.tap_count == 0 || analysis.tap_count > max_reference_cir_taps ||
      analysis.peak_delay_bin >= analysis.tap_count || !std::isfinite(noise_variance) || noise_variance < 0.0F ||
      !valid_float(analysis.mean_channel_power) || !valid_float(analysis.noise_corrected_power) ||
      !valid_float(analysis.peak_cir_power)) {
    return false;
  }
  output                       = {};
  output.struct_size           = sizeof(output);
  output.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  output.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  output.tap_count             = analysis.tap_count;
  output.peak_delay_bin        = analysis.peak_delay_bin;
  output.mean_channel_power    = static_cast<float>(analysis.mean_channel_power);
  output.noise_corrected_power = static_cast<float>(analysis.noise_corrected_power);
  output.noise_variance        = noise_variance;
  for (uint32_t tap = 0; tap != analysis.tap_count; ++tap) {
    if (!valid_float(analysis.tap_power[tap])) {
      return false;
    }
    output.tap_power[tap] = static_cast<float>(analysis.tap_power[tap]);
  }
  output.peak_cir_power = output.tap_power[output.peak_delay_bin];
  return true;
}
