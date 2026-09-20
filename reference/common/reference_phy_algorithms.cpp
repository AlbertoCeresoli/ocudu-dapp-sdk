// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Host-path Class-A reference CE/EQ/RX — algorithmically aligned with the CUDA
// Class-A contract (OCUDU conventional average CE + baseline MMSE with tx_scaling=1):
//   1) per-pilot LS
//   2) average_pairs for 2-layer CDM OCC
//   3) estimate common phase rotation across DM-RS symbols and align pilots
//   4) TD average aligned pilots, raised-cosine FD smoothing, then interpolation
//   5) per-RE one-layer MRC (MMSE-equivalent with LLR scaling), or two-layer MMSE
//
// Performance notes (hot path, PUSCH deadline):
//   - H is stored per (port, layer, SC) only — after TD average it is
//     symbol-independent, so we avoid a 4-D plane of size ·nof_symbols.
//   - Pilot SC mask + sorted pilot SC list are built once per estimate.
//   - Soft demap uses precomputed PAM levels (no per-label axis_level rebuild).
//   - thread_local workspaces avoid heap churn on every invoke.

#include "ocudu_dapp_sdk/reference_phy_algorithms.h"
#include "ocudu_dapp_sdk/receiver_symbol_storage.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

using namespace ocudu::dapp::reference;

namespace {

constexpr uint32_t max_rx_ports             = 64;
constexpr uint32_t max_layers               = 8;
constexpr uint64_t max_grid_re              = 64U * 1024U * 1024U;
constexpr float    minimum_soft_demap_noise = 1.0e-4F;
constexpr float    deep_fade_noise          = 1.0e6F;

struct cf32 {
  float real;
  float imag;
};

struct channel_state {
  const ocudu_dapp_pusch_metadata_v1* pusch{};
  const ocudu_dapp_dmrs_metadata_v1*  dmrs{};
  /// Compact H at phase_reference_symbol: [port][layer][subcarrier].
  std::vector<cf32>*  plane{};
  std::vector<float>* port_noise{};
  /// Common phase rotation in radians per OFDM-symbol, per [port][layer].
  std::vector<float>* phase_slope{};
  /// Pilot SC mask: 1 if any pilot uses this SC.
  std::vector<uint8_t>* pilot_sc{};
  /// Sorted unique pilot subcarriers (for FD interpolation).
  std::vector<uint16_t>* pilot_scs_sorted{};
  uint32_t               nsym{};
  uint32_t               nsc{};
  uint32_t               phase_reference_symbol{};
};

// Reusable per-thread workspace so the hot path does not malloc every PUSCH.
struct workspace {
  std::vector<cf32>     plane;
  std::vector<float>    port_noise;
  std::vector<float>    phase_slope;
  std::vector<uint8_t>  pilot_sc;
  std::vector<uint16_t> pilot_scs_sorted;
  std::vector<cf32>     joint_corr;
  std::vector<float>    joint_energy;
  std::vector<cf32>     h_flat;
  std::vector<cf32>     pilot_h;
  /// Accumulators for O(nof_pilots) TD average over pilot SCs.
  std::vector<cf32>  sc_sum;
  std::vector<float> sc_count;
  /// Bounded frequency-smoothing scratch, reused for every port and layer.
  std::vector<cf32> fd_extended;
  std::vector<cf32> fd_filtered;
};

workspace& tls_workspace() noexcept
{
  thread_local workspace ws;
  return ws;
}

uint32_t element_size(uint32_t type) noexcept
{
  switch (type) {
    case OCUDU_DAPP_ELEMENT_I8_V1:
    case OCUDU_DAPP_ELEMENT_U8_V1:
      return 1;
    case OCUDU_DAPP_ELEMENT_I16_V1:
    case OCUDU_DAPP_ELEMENT_U16_V1:
      return 2;
    case OCUDU_DAPP_ELEMENT_I32_V1:
    case OCUDU_DAPP_ELEMENT_U32_V1:
    case OCUDU_DAPP_ELEMENT_F32_V1:
      return 4;
    case OCUDU_DAPP_ELEMENT_CF32_V1:
      return 8;
    default:
      return 0;
  }
}

bool host_tensor(const ocudu_dapp_tensor_view_v1& view, uint32_t type, uint32_t rank, uint32_t access) noexcept
{
  constexpr uint32_t minimum   = offsetof(ocudu_dapp_tensor_view_v1, reserved);
  const uint32_t     item_size = element_size(type);
  if (view.struct_size < minimum || view.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 || view.element_type != type ||
      view.rank != rank || (view.access & access) != access || (view.access & OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1) == 0 ||
      item_size == 0 || view.native_handle == 0 || view.native_handle > std::numeric_limits<uintptr_t>::max() ||
      view.byte_offset > std::numeric_limits<uintptr_t>::max() - view.native_handle || view.byte_length == 0 ||
      (view.domain != OCUDU_DAPP_MEM_HOST_V1 && view.domain != OCUDU_DAPP_MEM_HOST_PINNED_V1 &&
       view.domain != OCUDU_DAPP_MEM_MANAGED_COHERENT_V1)) {
    return false;
  }
  const uintptr_t base      = static_cast<uintptr_t>(view.native_handle + view.byte_offset);
  const uint32_t  alignment = std::min<uint32_t>(item_size, alignof(cf32));
  if (base % alignment != 0) {
    return false;
  }
  uint64_t extent = 0;
  for (uint32_t dimension = 0; dimension != rank; ++dimension) {
    if (view.dimensions[dimension] == 0 || view.byte_strides[dimension] == 0 ||
        view.byte_strides[dimension] % alignment != 0) {
      return false;
    }
    const uint64_t count = view.dimensions[dimension] - 1;
    if (count != 0 && view.byte_strides[dimension] > (std::numeric_limits<uint64_t>::max() - extent) / count) {
      return false;
    }
    extent += count * view.byte_strides[dimension];
  }
  return extent <= view.byte_length && item_size <= view.byte_length - extent;
}

template <typename T>
const T* at(const ocudu_dapp_tensor_view_v1& view, uint64_t first, uint64_t second = 0, uint64_t third = 0,
            uint64_t fourth = 0) noexcept
{
  const uintptr_t base   = static_cast<uintptr_t>(view.native_handle) + view.byte_offset;
  const uint64_t  offset = first * view.byte_strides[0] + second * view.byte_strides[1] + third * view.byte_strides[2] +
                          fourth * view.byte_strides[3];
  return reinterpret_cast<const T*>(base + offset);
}

template <typename T>
T* writable_at(const ocudu_dapp_tensor_view_v1& view, uint64_t first, uint64_t second = 0, uint64_t third = 0,
               uint64_t fourth = 0) noexcept
{
  return const_cast<T*>(at<T>(view, first, second, third, fourth));
}

/// True when a rank-3 CF32 grid is dense [port][symbol][sc] with natural strides.
bool contiguous_cf32_grid3(const ocudu_dapp_tensor_view_v1& view) noexcept
{
  if (view.rank != 3 || view.element_type != OCUDU_DAPP_ELEMENT_CF32_V1) {
    return false;
  }
  const uint64_t n0 = view.dimensions[0];
  const uint64_t n1 = view.dimensions[1];
  const uint64_t n2 = view.dimensions[2];
  return view.byte_strides[2] == sizeof(cf32) && view.byte_strides[1] == sizeof(cf32) * n2 &&
         view.byte_strides[0] == sizeof(cf32) * n1 * n2 && n0 > 0 && n1 > 0 && n2 > 0;
}

const cf32* grid_base(const ocudu_dapp_tensor_view_v1& view) noexcept
{
  return reinterpret_cast<const cf32*>(static_cast<uintptr_t>(view.native_handle) + view.byte_offset);
}

cf32 add(cf32 lhs, cf32 rhs) noexcept
{
  return {lhs.real + rhs.real, lhs.imag + rhs.imag};
}

cf32 multiply_conjugate(cf32 lhs, cf32 rhs) noexcept
{
  return {lhs.real * rhs.real + lhs.imag * rhs.imag, lhs.imag * rhs.real - lhs.real * rhs.imag};
}

cf32 multiply(cf32 lhs, cf32 rhs) noexcept
{
  return {lhs.real * rhs.real - lhs.imag * rhs.imag, lhs.real * rhs.imag + lhs.imag * rhs.real};
}

cf32 scale(cf32 value, float factor) noexcept
{
  return {value.real * factor, value.imag * factor};
}

cf32 rotate(cf32 value, float phase) noexcept
{
  const float c = std::cos(phase);
  const float s = std::sin(phase);
  return {value.real * c - value.imag * s, value.real * s + value.imag * c};
}

float squared_norm(cf32 value) noexcept
{
  return value.real * value.real + value.imag * value.imag;
}

// Same raised-cosine prototype used by OCUDU's default PUSCH port channel
// estimator (roll-off 0.2, span 3, 10 samples/symbol).
constexpr std::array<float, 31> fd_rc_filter = {
    -0.0641253F, -0.0660711F, -0.0611526F, -0.0485918F, -0.0281126F, 0.0000000F,  0.0348830F, 0.0751249F,
    0.1188406F,  0.1637874F,  0.2075139F,  0.2475302F,  0.2814857F,  0.3073415F,  0.3235207F, 0.3290274F,
    0.3235207F,  0.3073415F,  0.2814857F,  0.2475302F,  0.2075139F,  0.1637874F,  0.1188406F, 0.0751249F,
    0.0348830F,  0.0000000F,  -0.0281126F, -0.0485918F, -0.0611526F, -0.0660711F, -0.0641253F};
constexpr uint32_t max_virtual_pilots = 12;
constexpr float    pi_f               = 3.14159265358979323846F;

struct fd_filter {
  std::array<float, fd_rc_filter.size()> coefficients{};
  uint32_t                               count{};
};

fd_filter make_fd_filter(uint32_t nof_rbs, uint32_t stride) noexcept
{
  nof_rbs                   = std::min(nof_rbs, 3U);
  stride                    = std::max(stride, 1U);
  uint32_t       half_count = (nof_rbs * 10U + 1U) / 2U / stride;
  const uint32_t first      = static_cast<uint32_t>(fd_rc_filter.size() / 2U) - half_count * stride;
  fd_filter      result{};
  result.count = 2U * half_count + 1U;
  float sum    = 0.0F;
  for (uint32_t i = 0; i != result.count; ++i) {
    result.coefficients[i] = fd_rc_filter[first + i * stride];
    sum += result.coefficients[i];
  }
  if (std::abs(sum) > 1.0e-12F) {
    for (uint32_t i = 0; i != result.count; ++i) {
      result.coefficients[i] /= sum;
    }
  }
  return result;
}

void extrapolate_virtual_pilots(cf32* output, const cf32* input, uint32_t count, bool at_start) noexcept
{
  std::array<float, max_virtual_pilots> magnitudes{};
  std::array<float, max_virtual_pilots> phases{};
  if (count == 0 || count > max_virtual_pilots) {
    return;
  }
  for (uint32_t i = 0; i != count; ++i) {
    magnitudes[i] = std::sqrt(squared_norm(input[i]));
    phases[i]     = std::atan2(input[i].imag, input[i].real);
  }
  float phase_wraps = 0.0F;
  for (uint32_t i = 0; i != count; ++i) {
    const float raw = phases[i];
    phases[i] += 2.0F * phase_wraps * pi_f;
    if (i + 1U != count) {
      const float jump = phases[i + 1U] - raw;
      if (std::abs(jump) > pi_f) {
        phase_wraps -= std::copysign(1.0F, jump);
      }
    }
  }

  const float mean_x    = static_cast<float>(count - 1U) * 0.5F;
  const float norm_x_sq = static_cast<float>((count - 1U) * count * (2U * count - 1U)) / 6.0F;
  float       mean_abs  = 0.0F;
  float       mean_arg  = 0.0F;
  float       dot_abs   = 0.0F;
  float       dot_arg   = 0.0F;
  for (uint32_t i = 0; i != count; ++i) {
    mean_abs += magnitudes[i];
    mean_arg += phases[i];
    dot_abs += magnitudes[i] * static_cast<float>(i);
    dot_arg += phases[i] * static_cast<float>(i);
  }
  mean_abs /= static_cast<float>(count);
  mean_arg /= static_cast<float>(count);
  const float   denominator   = norm_x_sq - static_cast<float>(count) * mean_x * mean_x;
  const float   slope_abs     = denominator > 0.0F ? (dot_abs - mean_x * mean_abs * count) / denominator : 0.0F;
  const float   slope_arg     = denominator > 0.0F ? (dot_arg - mean_x * mean_arg * count) / denominator : 0.0F;
  const float   intercept_abs = mean_abs - slope_abs * mean_x;
  const float   intercept_arg = mean_arg - slope_arg * mean_x;
  const int32_t offset        = at_start ? -static_cast<int32_t>(count) : static_cast<int32_t>(count);
  for (uint32_t i = 0; i != count; ++i) {
    const float x         = static_cast<float>(static_cast<int32_t>(i) + offset);
    const float amplitude = slope_abs * x + intercept_abs;
    const float phase     = slope_arg * x + intercept_arg + (amplitude > 0.0F ? 0.0F : pi_f);
    const float magnitude = std::abs(amplitude);
    output[i]             = {magnitude * std::cos(phase), magnitude * std::sin(phase)};
  }
}

bool smooth_frequency_pilots(workspace& ws, const channel_state& state, uint32_t port, uint32_t layer) noexcept
{
  const uint32_t nof_pilots = static_cast<uint32_t>(ws.pilot_scs_sorted.size());
  if (nof_pilots < 2U) {
    return true;
  }
  const uint32_t pilots_per_rb = state.dmrs->dmrs_type == 1 ? 6U : 4U;
  // The ABI permits sparse synthetic coordinate sets. The conventional FIR is
  // defined for complete per-RB PUSCH patterns; preserve raw LS interpolation
  // when that geometry is not present rather than attenuating missing pilots.
  if (nof_pilots < pilots_per_rb || nof_pilots % pilots_per_rb != 0U) {
    return true;
  }
  const uint32_t  nof_rbs     = nof_pilots / pilots_per_rb;
  const uint32_t  stride      = state.dmrs->dmrs_type == 1 ? 2U : 1U;
  const fd_filter filter      = make_fd_filter(nof_rbs, stride);
  const uint32_t  half_filter = filter.count / 2U;
  const uint32_t  nof_virtual =
      nof_rbs == 1U ? std::min(max_virtual_pilots, nof_pilots) : std::min(max_virtual_pilots, half_filter);
  if (nof_virtual == 0U) {
    return true;
  }
  try {
    ws.fd_extended.assign(static_cast<size_t>(nof_pilots) + 2U * nof_virtual, cf32{});
    ws.fd_filtered.assign(nof_pilots, cf32{});
  } catch (...) {
    return false;
  }
  for (uint32_t i = 0; i != nof_pilots; ++i) {
    const size_t index =
        (static_cast<size_t>(port) * state.pusch->nof_layers + layer) * state.nsc + ws.pilot_scs_sorted[i];
    ws.fd_extended[nof_virtual + i] = ws.plane[index];
  }
  extrapolate_virtual_pilots(ws.fd_extended.data(), ws.fd_extended.data() + nof_virtual, nof_virtual, true);
  extrapolate_virtual_pilots(ws.fd_extended.data() + nof_virtual + nof_pilots, ws.fd_extended.data() + nof_pilots,
                             nof_virtual, false);

  const int32_t extended_size = static_cast<int32_t>(ws.fd_extended.size());
  for (uint32_t i = 0; i != nof_pilots; ++i) {
    cf32 filtered{};
    for (uint32_t tap = 0; tap != filter.count; ++tap) {
      const int32_t source =
          static_cast<int32_t>(nof_virtual + i) + static_cast<int32_t>(tap) - static_cast<int32_t>(half_filter);
      if (source >= 0 && source < extended_size) {
        filtered = add(filtered, scale(ws.fd_extended[static_cast<uint32_t>(source)], filter.coefficients[tap]));
      }
    }
    ws.fd_filtered[i] = filtered;
  }
  for (uint32_t i = 0; i != nof_pilots; ++i) {
    const size_t index =
        (static_cast<size_t>(port) * state.pusch->nof_layers + layer) * state.nsc + ws.pilot_scs_sorted[i];
    ws.plane[index] = ws.fd_filtered[i];
  }
  return true;
}

template <typename T>
const T* metadata(const ocudu_dapp_typed_metadata_v1& wrapper, uint32_t schema) noexcept
{
  if (wrapper.struct_size < sizeof(wrapper) || wrapper.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
      wrapper.metadata_schema_id != schema || wrapper.metadata_schema_version != 1 ||
      wrapper.metadata.data == nullptr || wrapper.metadata.size < offsetof(T, reserved)) {
    return nullptr;
  }
  const auto* value = static_cast<const T*>(wrapper.metadata.data);
  return value->struct_size >= offsetof(T, reserved) && value->abi_major == OCUDU_DAPP_ABI_MAJOR_V1 ? value : nullptr;
}

bool valid_dimensions(const ocudu_dapp_tensor_view_v1& tensor, std::initializer_list<uint64_t> expected) noexcept
{
  if (tensor.rank != expected.size()) {
    return false;
  }
  uint32_t index = 0;
  for (uint64_t dimension : expected) {
    if (tensor.dimensions[index++] != dimension) {
      return false;
    }
  }
  return true;
}

/// Compact plane index: [port][layer][sc] (no symbol dimension).
size_t plane_index(const channel_state& state, uint32_t port, uint32_t layer, uint32_t subcarrier) noexcept
{
  return (static_cast<size_t>(port) * state.pusch->nof_layers + layer) * state.nsc + subcarrier;
}

cf32 channel_at(const channel_state& state, uint32_t port, uint32_t layer, uint32_t symbol,
                uint32_t subcarrier) noexcept
{
  const cf32  base  = (*state.plane)[plane_index(state, port, layer, subcarrier)];
  const float slope = (*state.phase_slope)[static_cast<size_t>(port) * state.pusch->nof_layers + layer];
  return rotate(base, slope * static_cast<float>(static_cast<int32_t>(symbol) -
                                                 static_cast<int32_t>(state.phase_reference_symbol)));
}

ocudu_dapp_status_v1 validate_input(const ocudu_dapp_tensor_view_v1&    rx_grid,
                                    const ocudu_dapp_typed_metadata_v1& pusch_wrapper,
                                    const ocudu_dapp_typed_metadata_v1& dmrs_wrapper, channel_state& state) noexcept
{
  constexpr uint32_t known_pusch_flags = OCUDU_DAPP_PUSCH_TRANSFORM_PRECODING_V1 |
                                         OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1 |
                                         OCUDU_DAPP_PUSCH_DC_OVERLAP_V1;
  constexpr uint32_t required_dmrs_flags =
      OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
  state.pusch = metadata<ocudu_dapp_pusch_metadata_v1>(pusch_wrapper, OCUDU_DAPP_METADATA_PUSCH_V1);
  state.dmrs  = metadata<ocudu_dapp_dmrs_metadata_v1>(dmrs_wrapper, OCUDU_DAPP_METADATA_DMRS_V1);
  if (state.pusch == nullptr || state.dmrs == nullptr ||
      !host_tensor(rx_grid, OCUDU_DAPP_ELEMENT_CF32_V1, 3, OCUDU_DAPP_ACCESS_READ_V1) ||
      !host_tensor(state.dmrs->reference_symbols, OCUDU_DAPP_ELEMENT_CF32_V1, 2, OCUDU_DAPP_ACCESS_READ_V1) ||
      !host_tensor(state.dmrs->coordinates, OCUDU_DAPP_ELEMENT_U16_V1, 2, OCUDU_DAPP_ACCESS_READ_V1)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  if (state.pusch->nof_rx_ports == 0 || state.pusch->nof_rx_ports > max_rx_ports || state.pusch->nof_layers == 0 ||
      state.pusch->nof_layers > max_layers || state.pusch->nof_symbols == 0 || state.pusch->nof_subcarriers == 0 ||
      state.pusch->nof_symbols > max_grid_re / state.pusch->nof_subcarriers ||
      state.pusch->nof_symbols * state.pusch->nof_subcarriers > max_grid_re || state.dmrs->nof_pilots == 0 ||
      (state.dmrs->dmrs_type != 1 && state.dmrs->dmrs_type != 2) || (state.pusch->flags & ~known_pusch_flags) != 0 ||
      (state.dmrs->flags & required_dmrs_flags) != required_dmrs_flags ||
      (state.dmrs->flags & ~required_dmrs_flags) != 0 || !std::isfinite(state.pusch->noise_variance_floor) ||
      state.pusch->noise_variance_floor < 0 ||
      !valid_dimensions(rx_grid, {state.pusch->nof_rx_ports, state.pusch->nof_symbols, state.pusch->nof_subcarriers}) ||
      !valid_dimensions(state.dmrs->reference_symbols, {state.pusch->nof_layers, state.dmrs->nof_pilots}) ||
      !valid_dimensions(state.dmrs->coordinates, {state.dmrs->nof_pilots, 2})) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  state.nsym = state.pusch->nof_symbols;
  state.nsc  = state.pusch->nof_subcarriers;
  return OCUDU_DAPP_OK_V1;
}

/// OCUDU-aligned plane CE (same structure as CUDA estimate_channels_block).
ocudu_dapp_status_v1 estimate_plane(const ocudu_dapp_tensor_view_v1& rx_grid, channel_state& state) noexcept
{
  const uint32_t nports  = state.pusch->nof_rx_ports;
  const uint32_t nlayers = state.pusch->nof_layers;
  const uint32_t nsym    = state.nsym;
  const uint32_t nsc     = state.nsc;
  const size_t   total   = static_cast<size_t>(nports) * nlayers * nsc;

  workspace& ws = tls_workspace();
  try {
    ws.plane.assign(total, cf32{});
    ws.port_noise.assign(nports, state.pusch->noise_variance_floor);
    ws.phase_slope.assign(static_cast<size_t>(nports) * nlayers, 0.0F);
    ws.pilot_sc.assign(nsc, 0);
    ws.pilot_scs_sorted.clear();
    ws.pilot_scs_sorted.reserve(std::min<size_t>(nsc, state.dmrs->nof_pilots));
    ws.joint_corr.assign(nlayers, cf32{});
    ws.joint_energy.assign(nlayers, 0.0F);
    ws.h_flat.assign(nlayers, cf32{});
  } catch (...) {
    return OCUDU_DAPP_RESOURCE_V1;
  }
  state.plane            = &ws.plane;
  state.port_noise       = &ws.port_noise;
  state.phase_slope      = &ws.phase_slope;
  state.pilot_sc         = &ws.pilot_sc;
  state.pilot_scs_sorted = &ws.pilot_scs_sorted;

  // Build pilot SC mask once (O(nof_pilots)).
  for (uint32_t pilot = 0; pilot != state.dmrs->nof_pilots; ++pilot) {
    const uint16_t symbol     = *at<uint16_t>(state.dmrs->coordinates, pilot, 0);
    const uint16_t subcarrier = *at<uint16_t>(state.dmrs->coordinates, pilot, 1);
    if (symbol >= nsym || subcarrier >= nsc) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }
    if (ws.pilot_sc[subcarrier] == 0) {
      ws.pilot_sc[subcarrier] = 1;
      ws.pilot_scs_sorted.push_back(subcarrier);
    }
  }
  std::sort(ws.pilot_scs_sorted.begin(), ws.pilot_scs_sorted.end());
  state.phase_reference_symbol = *at<uint16_t>(state.dmrs->coordinates, 0, 0);

  try {
    ws.pilot_h.assign(static_cast<size_t>(nlayers) * state.dmrs->nof_pilots, cf32{});
  } catch (...) {
    return OCUDU_DAPP_RESOURCE_V1;
  }

  const bool  grid_contig  = contiguous_cf32_grid3(rx_grid);
  const cf32* rx_base      = grid_contig ? grid_base(rx_grid) : nullptr;
  const bool  coord_contig = state.dmrs->coordinates.rank == 2 &&
                            state.dmrs->coordinates.byte_strides[1] == sizeof(uint16_t) &&
                            state.dmrs->coordinates.byte_strides[0] == 2 * sizeof(uint16_t);
  const uint16_t* coords     = coord_contig ? at<uint16_t>(state.dmrs->coordinates, 0, 0) : nullptr;
  const bool      ref_contig = state.dmrs->reference_symbols.rank == 2 &&
                          state.dmrs->reference_symbols.byte_strides[1] == sizeof(cf32) &&
                          state.dmrs->reference_symbols.byte_strides[0] == sizeof(cf32) * state.dmrs->nof_pilots;
  const cf32* refs = ref_contig ? at<cf32>(state.dmrs->reference_symbols, 0, 0) : nullptr;

  for (uint32_t port = 0; port != nports; ++port) {
    std::fill(ws.joint_corr.begin(), ws.joint_corr.end(), cf32{});
    std::fill(ws.joint_energy.begin(), ws.joint_energy.end(), 0.0F);
    std::fill(ws.pilot_h.begin(), ws.pilot_h.end(), cf32{});

    // 1) Per-pilot LS.
    for (uint32_t pilot = 0; pilot != state.dmrs->nof_pilots; ++pilot) {
      const uint16_t symbol = coords != nullptr ? coords[pilot * 2U] : *at<uint16_t>(state.dmrs->coordinates, pilot, 0);
      const uint16_t subcarrier =
          coords != nullptr ? coords[pilot * 2U + 1U] : *at<uint16_t>(state.dmrs->coordinates, pilot, 1);
      const cf32 received = rx_base != nullptr ? rx_base[(static_cast<size_t>(port) * nsym + symbol) * nsc + subcarrier]
                                               : *at<cf32>(rx_grid, port, symbol, subcarrier);
      for (uint32_t layer = 0; layer != nlayers; ++layer) {
        const cf32  reference = refs != nullptr ? refs[static_cast<size_t>(layer) * state.dmrs->nof_pilots + pilot]
                                                : *at<cf32>(state.dmrs->reference_symbols, layer, pilot);
        const float energy    = squared_norm(reference);
        cf32        h{};
        if (energy > 1.0e-12F) {
          h = scale(multiply_conjugate(received, reference), 1.0F / energy);
        }
        ws.pilot_h[static_cast<size_t>(layer) * state.dmrs->nof_pilots + pilot] = h;
        ws.joint_corr[layer] = add(ws.joint_corr[layer], multiply_conjugate(received, reference));
        ws.joint_energy[layer] += energy;
      }
    }

    // 2) average_pairs for 2-layer CDM (OCC over consecutive pilot indices).
    if (nlayers == 2) {
      for (uint32_t pair = 0; pair * 2U + 1U < state.dmrs->nof_pilots; ++pair) {
        const uint32_t p0 = pair * 2U;
        const uint32_t p1 = p0 + 1U;
        for (uint32_t layer = 0; layer != nlayers; ++layer) {
          const size_t base     = static_cast<size_t>(layer) * state.dmrs->nof_pilots;
          const cf32   av       = scale(add(ws.pilot_h[base + p0], ws.pilot_h[base + p1]), 0.5F);
          ws.pilot_h[base + p0] = av;
          ws.pilot_h[base + p1] = av;
        }
      }
    }

    // 3) Estimate the common phase rotation between the first and last DM-RS
    // symbols. The host baseline compensates CFO before TD averaging; averaging
    // unaligned pilots can cancel the channel and rotate every data symbol.
    uint32_t       first_begin  = 0;
    uint32_t       first_end    = 0;
    uint32_t       last_begin   = 0;
    const uint32_t first_symbol = *at<uint16_t>(state.dmrs->coordinates, 0, 0);
    uint32_t       last_symbol  = first_symbol;
    while (first_end < state.dmrs->nof_pilots && *at<uint16_t>(state.dmrs->coordinates, first_end, 0) == first_symbol) {
      ++first_end;
    }
    for (uint32_t pilot = first_end; pilot < state.dmrs->nof_pilots; ++pilot) {
      const uint32_t symbol = *at<uint16_t>(state.dmrs->coordinates, pilot, 0);
      if (symbol != last_symbol) {
        last_symbol = symbol;
        last_begin  = pilot;
      }
    }
    state.phase_reference_symbol = first_symbol;
    if (last_symbol > first_symbol && last_begin != first_begin &&
        state.dmrs->nof_pilots - last_begin == first_end - first_begin) {
      const uint32_t pilots_per_symbol = first_end - first_begin;
      for (uint32_t layer = 0; layer != nlayers; ++layer) {
        cf32         correlation{};
        const size_t layer_base = static_cast<size_t>(layer) * state.dmrs->nof_pilots;
        for (uint32_t pilot = 0; pilot != pilots_per_symbol; ++pilot) {
          const uint32_t first_pilot = first_begin + pilot;
          const uint32_t last_pilot  = last_begin + pilot;
          if (*at<uint16_t>(state.dmrs->coordinates, first_pilot, 1) !=
              *at<uint16_t>(state.dmrs->coordinates, last_pilot, 1)) {
            correlation = {};
            break;
          }
          correlation = add(correlation, multiply_conjugate(ws.pilot_h[layer_base + last_pilot],
                                                            ws.pilot_h[layer_base + first_pilot]));
        }
        if (squared_norm(correlation) > 1.0e-12F) {
          ws.phase_slope[static_cast<size_t>(port) * nlayers + layer] =
              std::atan2(correlation.imag, correlation.real) / static_cast<float>(last_symbol - first_symbol);
        }
      }
    }

    for (uint32_t layer = 0; layer != nlayers; ++layer) {
      ws.h_flat[layer] = ws.joint_energy[layer] > 1.0e-12F ? scale(ws.joint_corr[layer], 1.0F / ws.joint_energy[layer])
                                                           : cf32{1.0F, 0.0F};
    }

    // 4) Align each pilot to the first DM-RS phase and TD-average → one base H per SC.
    // Single pass over pilots: O(nof_pilots · nlayers), not O(unique_sc · nof_pilots).
    try {
      ws.sc_sum.assign(static_cast<size_t>(nlayers) * nsc, cf32{});
      ws.sc_count.assign(nsc, 0.0F);
    } catch (...) {
      return OCUDU_DAPP_RESOURCE_V1;
    }
    for (uint32_t pilot = 0; pilot != state.dmrs->nof_pilots; ++pilot) {
      const uint16_t sc = *at<uint16_t>(state.dmrs->coordinates, pilot, 1);
      if (sc >= nsc) {
        continue;
      }
      ws.sc_count[sc] += 1.0F;
      for (uint32_t layer = 0; layer != nlayers; ++layer) {
        const size_t   pbase  = static_cast<size_t>(layer) * state.dmrs->nof_pilots;
        const size_t   sbase  = static_cast<size_t>(layer) * nsc + sc;
        const uint32_t symbol = *at<uint16_t>(state.dmrs->coordinates, pilot, 0);
        const float    slope  = ws.phase_slope[static_cast<size_t>(port) * nlayers + layer];
        const cf32     aligned =
            rotate(ws.pilot_h[pbase + pilot],
                   -slope * static_cast<float>(static_cast<int32_t>(symbol) - static_cast<int32_t>(first_symbol)));
        ws.sc_sum[sbase] = add(ws.sc_sum[sbase], aligned);
      }
    }
    for (uint16_t sc : ws.pilot_scs_sorted) {
      const float count = ws.sc_count[sc];
      for (uint32_t layer = 0; layer != nlayers; ++layer) {
        const cf32 h_sc =
            count > 0.0F ? scale(ws.sc_sum[static_cast<size_t>(layer) * nsc + sc], 1.0F / count) : ws.h_flat[layer];
        ws.plane[plane_index(state, port, layer, sc)] = h_sc;
      }
    }

    // Match the conventional OCUDU estimator's default frequency-domain
    // strategy. Filtering the aligned TD average is equivalent to filtering
    // each aligned DM-RS symbol before their linear average. The reference
    // symbols already include the configured DMRS boost; LS divided by their
    // energy above, so no additional gain correction belongs here.
    for (uint32_t layer = 0; layer != nlayers; ++layer) {
      if (!smooth_frequency_pilots(ws, state, port, layer)) {
        return OCUDU_DAPP_RESOURCE_V1;
      }
    }

    // 5) FD linear fill / edge-hold for non-pilot SCs (sorted pilot SC list).
    const auto& pscs = ws.pilot_scs_sorted;
    for (uint32_t sc = 0; sc != nsc; ++sc) {
      if (ws.pilot_sc[sc] != 0) {
        continue;
      }
      const auto it        = std::lower_bound(pscs.begin(), pscs.end(), static_cast<uint16_t>(sc));
      const bool has_right = it != pscs.end();
      const bool has_left  = it != pscs.begin();
      const int  left_sc   = has_left ? static_cast<int>(*std::prev(it)) : -1;
      const int  right_sc  = has_right ? static_cast<int>(*it) : -1;

      for (uint32_t layer = 0; layer != nlayers; ++layer) {
        cf32 h_sc = ws.h_flat[layer];
        if (left_sc >= 0 && right_sc >= 0 && right_sc != left_sc) {
          const cf32  sl = ws.plane[plane_index(state, port, layer, static_cast<uint32_t>(left_sc))];
          const cf32  sr = ws.plane[plane_index(state, port, layer, static_cast<uint32_t>(right_sc))];
          const float t  = static_cast<float>(static_cast<int>(sc) - left_sc) / static_cast<float>(right_sc - left_sc);
          h_sc           = {sl.real + t * (sr.real - sl.real), sl.imag + t * (sr.imag - sl.imag)};
        } else if (left_sc >= 0) {
          h_sc = ws.plane[plane_index(state, port, layer, static_cast<uint32_t>(left_sc))];
        } else if (right_sc >= 0) {
          h_sc = ws.plane[plane_index(state, port, layer, static_cast<uint32_t>(right_sc))];
        }
        ws.plane[plane_index(state, port, layer, sc)] = h_sc;
      }
    }

    // Residual noise at pilots (multi-layer reconstruction).
    float residual = 0.0F;
    float valid    = 0.0F;
    for (uint32_t pilot = 0; pilot != state.dmrs->nof_pilots; ++pilot) {
      const uint16_t symbol     = *at<uint16_t>(state.dmrs->coordinates, pilot, 0);
      const uint16_t subcarrier = *at<uint16_t>(state.dmrs->coordinates, pilot, 1);
      const cf32     received   = *at<cf32>(rx_grid, port, symbol, subcarrier);
      cf32           predicted{};
      for (uint32_t layer = 0; layer != nlayers; ++layer) {
        const cf32 h = channel_at(state, port, layer, symbol, subcarrier);
        const cf32 r = *at<cf32>(state.dmrs->reference_symbols, layer, pilot);
        predicted    = add(predicted, multiply(h, r));
      }
      residual += squared_norm({received.real - predicted.real, received.imag - predicted.imag});
      valid += 1.0F;
    }
    // OCUDU baseline normalization. After FD smoothing, the channel is no
    // longer an independent fit to every pilot, so the old N-unique_SC
    // correction overestimated physical noise.
    ws.port_noise[port] = valid > 1.0F ? std::max(state.pusch->noise_variance_floor, residual / (valid - 1.0F))
                                       : state.pusch->noise_variance_floor;
  }
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 write_channel_estimates(const channel_state&             state,
                                             const ocudu_dapp_tensor_view_v1& output) noexcept
{
  if (!host_tensor(output, OCUDU_DAPP_ELEMENT_CF32_V1, 4, OCUDU_DAPP_ACCESS_WRITE_V1) ||
      !valid_dimensions(output, {state.pusch->nof_rx_ports, state.pusch->nof_layers, state.pusch->nof_symbols,
                                 state.pusch->nof_subcarriers}) ||
      state.plane == nullptr) {
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  // Expand the phase-aligned base channel at each OFDM symbol. This matches the
  // baseline's CFO-compensated, time-dependent channel contract.
  for (uint32_t port = 0; port != state.pusch->nof_rx_ports; ++port) {
    for (uint32_t layer = 0; layer != state.pusch->nof_layers; ++layer) {
      for (uint32_t subcarrier = 0; subcarrier != state.nsc; ++subcarrier) {
        for (uint32_t symbol = 0; symbol != state.nsym; ++symbol) {
          *writable_at<cf32>(output, port, layer, symbol, subcarrier) =
              channel_at(state, port, layer, symbol, subcarrier);
        }
      }
    }
  }
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 write_channel_output(const channel_state&                    state,
                                          ocudu_dapp_channel_estimator_output_v1& output) noexcept
{
  const auto status = write_channel_estimates(state, output.channel_estimates);
  if (status != OCUDU_DAPP_OK_V1 || state.port_noise == nullptr ||
      !host_tensor(output.noise_variances, OCUDU_DAPP_ELEMENT_F32_V1, 2, OCUDU_DAPP_ACCESS_WRITE_V1) ||
      !valid_dimensions(output.noise_variances, {state.pusch->nof_rx_ports, state.pusch->nof_layers})) {
    return status != OCUDU_DAPP_OK_V1 ? status : OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  for (uint32_t port = 0; port != state.pusch->nof_rx_ports; ++port) {
    for (uint32_t layer = 0; layer != state.pusch->nof_layers; ++layer) {
      *writable_at<float>(output.noise_variances, port, layer) = (*state.port_noise)[port];
    }
  }
  return OCUDU_DAPP_OK_V1;
}

/// Baseline MMSE with tx_scaling=1 and per-SC H. The one-layer LLR-scaled result is
/// mathematically identical to OCUDU's ZF MRC implementation.
/// When `rx_base` is non-null the grid is treated as dense [port][symbol][sc].
void equalize_re(const ocudu_dapp_tensor_view_v1& rx_grid, const channel_state& state, uint32_t symbol,
                 uint32_t subcarrier, cf32* symbols_out, float* noise_out, const cf32* rx_base = nullptr) noexcept
{
  const uint32_t nlayers = state.pusch->nof_layers;
  const uint32_t nports  = state.pusch->nof_rx_ports;
  const uint32_t nsym    = state.nsym;
  const uint32_t nsc     = state.nsc;
  const auto&    pnoise  = *state.port_noise;

  auto load_y = [&](uint32_t port) noexcept -> cf32 {
    if (rx_base != nullptr) {
      return rx_base[(static_cast<size_t>(port) * nsym + symbol) * nsc + subcarrier];
    }
    return *at<cf32>(rx_grid, port, symbol, subcarrier);
  };

  if (nlayers == 1) {
    cf32  matched{};
    float diagonal       = 0.0F;
    float noise_diagonal = 0.0F;
    for (uint32_t port = 0; port != nports; ++port) {
      const cf32 y      = load_y(port);
      const cf32 h      = channel_at(state, port, 0, symbol, subcarrier);
      matched           = add(matched, multiply_conjugate(y, h));
      const float power = squared_norm(h);
      diagonal += power;
      noise_diagonal += pnoise[port] * power;
    }
    if (diagonal <= 1.0e-12F) {
      symbols_out[0] = {};
      noise_out[0]   = deep_fade_noise;
    } else {
      symbols_out[0] = scale(matched, 1.0F / diagonal);
      noise_out[0]   = std::max(std::max(state.pusch->noise_variance_floor, minimum_soft_demap_noise),
                                noise_diagonal / (diagonal * diagonal));
    }
    return;
  }

  // Two-layer MMSE. Match the host equalizer's pessimistic scalar-noise
  // convention, diagonal loading and per-layer LLR rescaling.
  cf32  matched[2]{};
  float diagonal[2]{};
  cf32  cross{};
  float mmse_noise{};
  for (uint32_t port = 0; port != nports; ++port) {
    const cf32 y   = load_y(port);
    const cf32 h0  = channel_at(state, port, 0, symbol, subcarrier);
    const cf32 h1  = channel_at(state, port, 1, symbol, subcarrier);
    matched[0]     = add(matched[0], multiply_conjugate(y, h0));
    matched[1]     = add(matched[1], multiply_conjugate(y, h1));
    const float p0 = squared_norm(h0);
    const float p1 = squared_norm(h1);
    diagonal[0] += p0;
    diagonal[1] += p1;
    cross      = add(cross, multiply_conjugate(h0, h1));
    mmse_noise = std::max(mmse_noise, pnoise[port]);
  }
  const float gram0       = diagonal[0] + mmse_noise;
  const float gram1       = diagonal[1] + mmse_noise;
  const float det         = gram0 * gram1 - squared_norm(cross);
  const float correction0 = 1.0F - mmse_noise * gram1 / det;
  const float correction1 = 1.0F - mmse_noise * gram0 / det;
  if (det <= 1.0e-12F || correction0 <= 1.0e-12F || correction1 <= 1.0e-12F) {
    symbols_out[0] = {};
    symbols_out[1] = {};
    noise_out[0]   = deep_fade_noise;
    noise_out[1]   = deep_fade_noise;
    return;
  }
  const cf32 cross_b1 = multiply(cross, matched[1]);
  symbols_out[0]      = scale({gram1 * matched[0].real - cross_b1.real, gram1 * matched[0].imag - cross_b1.imag},
                              1.0F / (det * correction0));
  const cf32 xi_conj_m0{cross.real * matched[0].real + cross.imag * matched[0].imag,
                        cross.real * matched[0].imag - cross.imag * matched[0].real};
  symbols_out[1] = scale({gram0 * matched[1].real - xi_conj_m0.real, gram0 * matched[1].imag - xi_conj_m0.imag},
                         1.0F / (det * correction1));
  noise_out[0] =
      std::max(std::max(state.pusch->noise_variance_floor, minimum_soft_demap_noise), 1.0F / correction0 - 1.0F);
  noise_out[1] =
      std::max(std::max(state.pusch->noise_variance_floor, minimum_soft_demap_noise), 1.0F / correction1 - 1.0F);
}

int8_t quantize_llr(float value) noexcept
{
  return static_cast<int8_t>(std::lrint(std::clamp(value, -127.0F, 127.0F)));
}

float axis_level(uint32_t label, uint32_t bits) noexcept
{
  float amplitude = 1.0F;
  float scale     = 2.0F;
  for (uint32_t bit = bits; bit > 1; --bit) {
    const float direction = ((label >> (bit - 1)) & 1U) == 0 ? 1.0F : -1.0F;
    amplitude             = scale - direction * amplitude;
    scale *= 2.0F;
  }
  const float    sign          = (label & 1U) == 0 ? 1.0F : -1.0F;
  const uint32_t modulation    = 1U << (2U * bits);
  const float    normalization = std::sqrt(2.0F * static_cast<float>(modulation - 1U) / 3.0F);
  return sign * amplitude / normalization;
}

/// Precomputed PAM levels for axis_bits in {1,2,3,4} (QPSK … 256-QAM).
struct demap_tables {
  // levels[bits-1][label]  — bits 1..4 → index 0..3, max 16 labels.
  std::array<std::array<float, 16>, 4> levels{};
  demap_tables() noexcept
  {
    for (uint32_t bits = 1; bits <= 4; ++bits) {
      const uint32_t n = 1U << bits;
      for (uint32_t label = 0; label != n; ++label) {
        levels[bits - 1][label] = axis_level(label, bits);
      }
    }
  }
};

const demap_tables& demap_lut() noexcept
{
  static const demap_tables tables{};
  return tables;
}

void demap_axis(float value, float noise, uint32_t bits, uint32_t axis, int8_t* output) noexcept
{
  const uint32_t levels = 1U << bits;
  const auto&    lut    = demap_lut().levels[bits - 1];
  const float    inv_n  = 1.0F / std::max(noise, minimum_soft_demap_noise);

  // Fast path: QPSK (1 bit/axis) — two levels only.
  if (bits == 1) {
    const float d0 = (value - lut[0]) * (value - lut[0]);
    const float d1 = (value - lut[1]) * (value - lut[1]);
    output[axis]   = quantize_llr((d1 - d0) * inv_n);
    return;
  }

  // For each bit, min distance among labels with bit=0 vs bit=1.
  // Walk labels once per bit (max 16) — LUT levels already normalized.
  for (uint32_t bit = 0; bit != bits; ++bit) {
    float min0 = std::numeric_limits<float>::infinity();
    float min1 = std::numeric_limits<float>::infinity();
    for (uint32_t label = 0; label != levels; ++label) {
      const float level = lut[label];
      const float d     = (value - level) * (value - level);
      if (((label >> bit) & 1U) == 0) {
        min0 = d < min0 ? d : min0;
      } else {
        min1 = d < min1 ? d : min1;
      }
    }
    output[axis + 2U * bit] = quantize_llr((min1 - min0) * inv_n);
  }
}

} // namespace

ocudu_dapp_status_v1 ocudu::dapp::reference::estimate_channel(const ocudu_dapp_channel_estimator_input_v1& input,
                                                              ocudu_dapp_channel_estimator_output_v1&      output)
{
  channel_state state;
  auto          status = validate_input(input.rx_grid, input.pusch, input.dmrs, state);
  if (status == OCUDU_DAPP_OK_V1) {
    status = estimate_plane(input.rx_grid, state);
  }
  return status == OCUDU_DAPP_OK_V1 ? write_channel_output(state, output) : status;
}

ocudu_dapp_status_v1 ocudu::dapp::reference::equalize(const ocudu_dapp_equalizer_input_v1& input,
                                                      ocudu_dapp_equalizer_output_v1&      output)
{
  channel_state state;
  auto          status = validate_input(input.rx_grid, input.pusch, input.dmrs, state);
  if (status != OCUDU_DAPP_OK_V1 || state.pusch->nof_layers > 2) {
    return status != OCUDU_DAPP_OK_V1 ? status : OCUDU_DAPP_UNSUPPORTED_V1;
  }
  if ((state.pusch->flags & OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1) == 0) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  status = estimate_plane(input.rx_grid, state);
  if (status != OCUDU_DAPP_OK_V1) {
    return status;
  }
  status                 = write_channel_estimates(state, output.channel_estimates);
  const uint32_t nlayers = state.pusch->nof_layers;
  if (status != OCUDU_DAPP_OK_V1 ||
      !host_tensor(output.equalized_symbols, OCUDU_DAPP_ELEMENT_CF32_V1, 3, OCUDU_DAPP_ACCESS_WRITE_V1) ||
      !host_tensor(output.post_equalization_noise, OCUDU_DAPP_ELEMENT_F32_V1, 3, OCUDU_DAPP_ACCESS_WRITE_V1) ||
      !valid_dimensions(output.equalized_symbols, {nlayers, state.pusch->nof_symbols, state.pusch->nof_subcarriers}) ||
      !valid_dimensions(output.post_equalization_noise,
                        {nlayers, state.pusch->nof_symbols, state.pusch->nof_subcarriers})) {
    return status != OCUDU_DAPP_OK_V1 ? status : OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  const cf32* rx_base = contiguous_cf32_grid3(input.rx_grid) ? grid_base(input.rx_grid) : nullptr;
  for (uint32_t symbol = 0; symbol != state.nsym; ++symbol) {
    for (uint32_t subcarrier = 0; subcarrier != state.nsc; ++subcarrier) {
      cf32  symbols[2]{};
      float noise[2]{};
      equalize_re(input.rx_grid, state, symbol, subcarrier, symbols, noise, rx_base);
      for (uint32_t layer = 0; layer != nlayers; ++layer) {
        *writable_at<cf32>(output.equalized_symbols, layer, symbol, subcarrier)        = symbols[layer];
        *writable_at<float>(output.post_equalization_noise, layer, symbol, subcarrier) = noise[layer];
      }
    }
  }
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 ocudu::dapp::reference::receive(const ocudu_dapp_receiver_input_v1& input,
                                                     ocudu_dapp_receiver_output_v1&      output)
{
  output.reserved0 = 0;
  channel_state state;
  auto          status = validate_input(input.rx_grid, input.pusch, input.dmrs, state);
  if (status != OCUDU_DAPP_OK_V1 || state.pusch->nof_layers != 1) {
    return status != OCUDU_DAPP_OK_V1 ? status : OCUDU_DAPP_UNSUPPORTED_V1;
  }
  if ((state.pusch->flags & OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1) == 0) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const uint32_t modulation_order = static_cast<uint32_t>(state.pusch->modulation);
  if ((modulation_order != 2 && modulation_order != 4 && modulation_order != 6 && modulation_order != 8) ||
      !host_tensor(state.pusch->data_re_mask, OCUDU_DAPP_ELEMENT_U8_V1, 2, OCUDU_DAPP_ACCESS_READ_V1) ||
      !valid_dimensions(state.pusch->data_re_mask, {state.pusch->nof_symbols, state.pusch->nof_subcarriers})) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }

  // Prefer compact data_re_indices + nof_data_re when the host supplies them.
  const bool have_indices =
      host_tensor(state.pusch->data_re_indices, OCUDU_DAPP_ELEMENT_U32_V1, 1, OCUDU_DAPP_ACCESS_READ_V1) &&
      state.pusch->data_re_indices.dimensions[0] > 0;

  uint64_t nof_data_re = 0;
  if (have_indices) {
    nof_data_re = state.pusch->nof_data_re != 0 ? state.pusch->nof_data_re : state.pusch->data_re_indices.dimensions[0];
    if (nof_data_re > state.pusch->data_re_indices.dimensions[0]) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }
  } else {
    for (uint32_t symbol = 0; symbol != state.pusch->nof_symbols; ++symbol) {
      for (uint32_t subcarrier = 0; subcarrier != state.pusch->nof_subcarriers; ++subcarrier) {
        nof_data_re += *at<uint8_t>(state.pusch->data_re_mask, symbol, subcarrier) != 0;
      }
    }
    if (state.pusch->nof_data_re != 0 && state.pusch->nof_data_re != nof_data_re) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }
  }
  if (nof_data_re == 0 || nof_data_re > std::numeric_limits<uint64_t>::max() / modulation_order) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  if (!host_tensor(output.llrs, OCUDU_DAPP_ELEMENT_I8_V1, 1, OCUDU_DAPP_ACCESS_WRITE_V1) ||
      !valid_dimensions(output.llrs, {nof_data_re * modulation_order})) {
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  // Accept the legacy metrics prefix. The symbol tail is independently
  // optional; sizeof(the newest struct) is not a minimum ABI requirement.
  const ocudu_dapp_receiver_metrics_v1* receiver_metrics = nullptr;
  if (output.metrics.metadata.data != nullptr || output.metrics.metadata.size != 0) {
    constexpr size_t minimum = offsetof(ocudu_dapp_receiver_metrics_v1, reserved);
    const auto& wrapper = output.metrics;
    if (wrapper.struct_size < sizeof(wrapper) || wrapper.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
        wrapper.metadata_schema_id != OCUDU_DAPP_METADATA_RECEIVER_METRICS_V1 || wrapper.metadata_schema_version != 1 ||
        wrapper.metadata.data == nullptr || wrapper.metadata.size < minimum ||
        reinterpret_cast<uintptr_t>(wrapper.metadata.data) % alignof(ocudu_dapp_receiver_metrics_v1) != 0) {
      return OCUDU_DAPP_INVALID_OUTPUT_V1;
    }
    receiver_metrics = static_cast<const ocudu_dapp_receiver_metrics_v1*>(wrapper.metadata.data);
    if (receiver_metrics->struct_size < minimum || receiver_metrics->struct_size > wrapper.metadata.size ||
        receiver_metrics->abi_major != OCUDU_DAPP_ABI_MAJOR_V1) {
      return OCUDU_DAPP_INVALID_OUTPUT_V1;
    }
  }
  const auto* symbol_output = receiver_metrics != nullptr &&
                                     receiver_metrics->struct_size >= OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_MIN_SIZE_V1 &&
                                     receiver_metrics->equalized_symbols.struct_size != 0
                                 ? &receiver_metrics->equalized_symbols
                                 : nullptr;
  if (receiver_metrics != nullptr &&
      (!host_tensor(receiver_metrics->post_equalization_noise, OCUDU_DAPP_ELEMENT_F32_V1, 2,
                    OCUDU_DAPP_ACCESS_WRITE_V1) ||
       receiver_metrics->post_equalization_noise.layout_id != OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1 ||
       !valid_dimensions(receiver_metrics->post_equalization_noise, {nof_data_re, 1}))) {
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  if (symbol_output != nullptr &&
      (!host_tensor(*symbol_output, OCUDU_DAPP_ELEMENT_CF32_V1, 2, OCUDU_DAPP_ACCESS_WRITE_V1) ||
       symbol_output->layout_id != OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1 ||
       symbol_output->layout_version != 1 || !valid_dimensions(*symbol_output, {nof_data_re, 1}) ||
       !receiver_symbol_storage_is_disjoint(*symbol_output, input, output, *state.pusch, *state.dmrs,
                                            receiver_metrics->post_equalization_noise))) {
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  status = estimate_plane(input.rx_grid, state);
  if (status != OCUDU_DAPP_OK_V1) {
    return status;
  }

  uint64_t       output_index = 0;
  uint64_t       confidence   = 0;
  const uint32_t axis_bits    = modulation_order / 2;
  const cf32*    rx_base      = contiguous_cf32_grid3(input.rx_grid) ? grid_base(input.rx_grid) : nullptr;

  // Prefer dense LLR output pointer when strides are natural.
  int8_t* llr_base = nullptr;
  if (output.llrs.rank == 1 && output.llrs.byte_strides[0] == 1) {
    llr_base = writable_at<int8_t>(output.llrs, 0);
  }

  auto demap_one = [&](uint32_t symbol, uint32_t subcarrier) noexcept {
    cf32  symbols[2]{};
    float noise[2]{};
    equalize_re(input.rx_grid, state, symbol, subcarrier, symbols, noise, rx_base);
    const uint64_t data_index = output_index / modulation_order;
    if (receiver_metrics != nullptr) {
      *writable_at<float>(receiver_metrics->post_equalization_noise, data_index, 0) = noise[0];
    }
    if (symbol_output != nullptr) {
      *writable_at<cf32>(*symbol_output, data_index, 0) = symbols[0];
    }
    int8_t* llrs = llr_base != nullptr ? llr_base + output_index : writable_at<int8_t>(output.llrs, output_index);
    demap_axis(symbols[0].real, noise[0], axis_bits, 0, llrs);
    demap_axis(symbols[0].imag, noise[0], axis_bits, 1, llrs);
    for (uint32_t bit = 0; bit != modulation_order; ++bit) {
      confidence += static_cast<uint64_t>(std::abs(static_cast<int>(llrs[bit])));
    }
    output_index += modulation_order;
  };

  if (have_indices) {
    // Compact indices: packed as symbol * nof_subcarriers + subcarrier (canonical).
    const uint32_t  nsc      = state.nsc;
    const uint32_t* idx_base = nullptr;
    if (state.pusch->data_re_indices.byte_strides[0] == sizeof(uint32_t)) {
      idx_base = at<uint32_t>(state.pusch->data_re_indices, 0);
    }
    for (uint64_t i = 0; i != nof_data_re; ++i) {
      const uint32_t flat       = idx_base != nullptr ? idx_base[i] : *at<uint32_t>(state.pusch->data_re_indices, i);
      const uint32_t symbol     = flat / nsc;
      const uint32_t subcarrier = flat % nsc;
      if (symbol >= state.nsym || subcarrier >= nsc) {
        return OCUDU_DAPP_INVALID_INPUT_V1;
      }
      demap_one(symbol, subcarrier);
    }
  } else {
    for (uint32_t symbol = 0; symbol != state.nsym; ++symbol) {
      for (uint32_t subcarrier = 0; subcarrier != state.nsc; ++subcarrier) {
        if (*at<uint8_t>(state.pusch->data_re_mask, symbol, subcarrier) == 0) {
          continue;
        }
        demap_one(symbol, subcarrier);
      }
    }
  }

  output.uci = {};
  if (receiver_metrics != nullptr) {
    output.reserved0 |= OCUDU_DAPP_RECEIVER_METRICS_VALID_V1;
  }
  if (symbol_output != nullptr) {
    output.reserved0 |= OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_VALID_V1;
  }
  output.confidence_q16 =
      output_index == 0 ? 0 : static_cast<uint32_t>(std::min<uint64_t>(65536, (confidence / output_index) * 512));
  return OCUDU_DAPP_OK_V1;
}
