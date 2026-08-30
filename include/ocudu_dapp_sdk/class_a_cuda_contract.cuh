// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include "ocudu/dapp/use_cases/v1/equalizer.h"
#include "ocudu/dapp/use_cases/v1/receiver.h"
#include <cmath>
#include <cstdint>
#include <cuda_bf16.h>
#include <cuda_runtime.h>

namespace ocudu::dapp::reference::cuda_contract {

/// Resolve an ABI tensor view to its typed (device) base pointer.
template <typename T>
T* pointer(const ocudu_dapp_tensor_view_v1& view) noexcept
{
  return reinterpret_cast<T*>(static_cast<uintptr_t>(view.native_handle) + view.byte_offset);
}

/// True when the view is dense row-major with an innermost stride of
/// \p element_bytes and its extent fits inside `byte_length`.
inline bool contiguous(const ocudu_dapp_tensor_view_v1& view, uint32_t element_bytes) noexcept
{
  if (view.rank == 0 || view.rank > OCUDU_DAPP_MAX_TENSOR_RANK_V1 ||
      view.byte_strides[view.rank - 1] != element_bytes) {
    return false;
  }
  uint64_t stride = element_bytes;
  for (uint32_t dimension = view.rank; dimension != 0; --dimension) {
    if (view.byte_strides[dimension - 1] != stride || view.dimensions[dimension - 1] == 0) {
      return false;
    }
    stride *= view.dimensions[dimension - 1];
  }
  return stride <= view.byte_length;
}

/// True when the view is a contiguous CUDA-device tensor with the expected
/// element type, rank and access bits — the admission check every Class-A CUDA
/// entry point runs on its ABI tensors.
inline bool device_tensor(const ocudu_dapp_tensor_view_v1& view, uint32_t element, uint32_t rank, uint32_t access,
                          uint32_t element_bytes) noexcept
{
  return view.struct_size >= sizeof(view) && view.domain == OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1 &&
         view.backend_id == OCUDU_DAPP_BACKEND_CUDA_V1 && view.element_type == element && view.rank == rank &&
         (view.access & access) == access && view.native_handle != 0 && contiguous(view, element_bytes);
}

/// Extract the PUSCH metadata block, or nullptr when the schema id, size or
/// struct_size does not match. The pointer aliases the host's metadata buffer.
inline const ocudu_dapp_pusch_metadata_v1* pusch_metadata(const ocudu_dapp_typed_metadata_v1& typed) noexcept
{
  if (typed.metadata_schema_id != OCUDU_DAPP_METADATA_PUSCH_V1 || typed.metadata.data == nullptr ||
      typed.metadata.size < sizeof(ocudu_dapp_pusch_metadata_v1)) {
    return nullptr;
  }
  const auto* value = static_cast<const ocudu_dapp_pusch_metadata_v1*>(typed.metadata.data);
  return value->struct_size >= sizeof(*value) ? value : nullptr;
}

/// Extract the DM-RS metadata block, or nullptr when the schema id, size or
/// struct_size does not match. The pointer aliases the host's metadata buffer.
inline const ocudu_dapp_dmrs_metadata_v1* dmrs_metadata(const ocudu_dapp_typed_metadata_v1& typed) noexcept
{
  if (typed.metadata_schema_id != OCUDU_DAPP_METADATA_DMRS_V1 || typed.metadata.data == nullptr ||
      typed.metadata.size < sizeof(ocudu_dapp_dmrs_metadata_v1)) {
    return nullptr;
  }
  const auto* value = static_cast<const ocudu_dapp_dmrs_metadata_v1*>(typed.metadata.data);
  return value->struct_size >= sizeof(*value) ? value : nullptr;
}

struct inputs {
  const uint32_t* grid;
  const float2*   references;
  const uint16_t* coordinates;
  const uint32_t* data_indices;
  cudaStream_t    stream;
  uint32_t        nof_rx_ports;
  uint32_t        nof_layers;
  /// Full resource-grid dimensions (indexing stride for grid / plane tensors).
  uint32_t nof_symbols;
  uint32_t nof_subcarriers;
  /// Allocated PUSCH region from metadata (CE/EQ work is limited to this span).
  uint32_t alloc_start_symbol;
  uint32_t alloc_nof_symbols;
  uint32_t alloc_start_sc;
  uint32_t alloc_nof_sc;
  uint32_t nof_data_re;
  uint32_t nof_pilots;
  uint32_t dmrs_type;
  uint64_t dmrs_symbol_mask;
  /// Optional C-RNTI. Zero means that per-UE state must not be retained.
  uint32_t rnti;
  uint32_t subcarrier_spacing_hz;
  uint32_t grid_nof_subcarriers;
  float    noise_variance_floor;
  /// Transmitter DC subcarrier (grid index) or no_dc_subcarrier. The UE's LO leakage sits on this RE, so its
  /// DM-RS pilot is unusable for channel estimation and its data RE must be demapped as an erasure.
  uint32_t dc_subcarrier;
};

inline constexpr uint32_t no_dc_subcarrier = 0xFFFFFFFFu;

#ifndef OCUDU_DAPP_PUSCH_RESERVED_DC_SUBCARRIER_PLUS_ONE_V1
// Older use-case headers predate the DC slot; the wire layout is the same.
#define OCUDU_DAPP_PUSCH_RESERVED_DC_SUBCARRIER_PLUS_ONE_V1 3u
#endif

/// Validate one Class-A CUDA invocation (CE / equalizer / receiver input all
/// share the pusch/dmrs/rx_grid/invocation members) against the v1 admitted
/// envelope: 1-2 layers, up to 64 rx ports, DM-RS type 1/2, complete data-RE
/// selection, contiguous device tensors of the expected types. On success
/// fills \p result with device pointers, the invocation's CUDA stream and the
/// unpacked geometry (including the reserved-slot RNTI, SCS, grid width and
/// DC subcarrier); on failure returns false and \p result is unspecified.
template <typename Input>
bool validate_inputs(const Input& input, inputs& result) noexcept
{
  constexpr uint32_t required_dmrs_flags =
      OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
  const auto* pusch = pusch_metadata(input.pusch);
  const auto* dmrs  = dmrs_metadata(input.dmrs);
  if (pusch == nullptr || dmrs == nullptr || input.invocation.execution_stream == 0 || pusch->nof_layers == 0 ||
      pusch->nof_layers > 2 || pusch->nof_rx_ports == 0 || pusch->nof_rx_ports > 64 || dmrs->nof_pilots == 0 ||
      pusch->nof_symbols == 0 || pusch->nof_subcarriers == 0 || (dmrs->dmrs_type != 1 && dmrs->dmrs_type != 2) ||
      dmrs->nof_pilots > 4096 || pusch->nof_data_re == 0 || !std::isfinite(pusch->noise_variance_floor) ||
      pusch->noise_variance_floor < 0 ||
      (pusch->flags & ~(OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1 | OCUDU_DAPP_PUSCH_DC_OVERLAP_V1)) != 0 ||
      (pusch->flags & OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1) == 0 || dmrs->flags != required_dmrs_flags ||
      !device_tensor(input.rx_grid, OCUDU_DAPP_ELEMENT_CBF16_V1, 3, OCUDU_DAPP_ACCESS_READ_V1, sizeof(uint32_t)) ||
      !device_tensor(dmrs->reference_symbols, OCUDU_DAPP_ELEMENT_CF32_V1, 2, OCUDU_DAPP_ACCESS_READ_V1,
                     sizeof(float2)) ||
      !device_tensor(dmrs->coordinates, OCUDU_DAPP_ELEMENT_U16_V1, 2, OCUDU_DAPP_ACCESS_READ_V1, sizeof(uint16_t)) ||
      !device_tensor(pusch->data_re_indices, OCUDU_DAPP_ELEMENT_U32_V1, 1, OCUDU_DAPP_ACCESS_READ_V1,
                     sizeof(uint32_t)) ||
      input.rx_grid.dimensions[0] != pusch->nof_rx_ports || input.rx_grid.dimensions[1] > UINT32_MAX ||
      input.rx_grid.dimensions[2] > UINT32_MAX ||
      input.rx_grid.dimensions[1] < static_cast<uint64_t>(pusch->start_symbol) + pusch->nof_symbols ||
      input.rx_grid.dimensions[2] < static_cast<uint64_t>(pusch->start_subcarrier) + pusch->nof_subcarriers ||
      dmrs->coordinates.dimensions[0] < dmrs->nof_pilots || dmrs->coordinates.dimensions[1] != 2 ||
      dmrs->reference_symbols.dimensions[0] != pusch->nof_layers ||
      dmrs->reference_symbols.dimensions[1] < dmrs->nof_pilots ||
      pusch->data_re_indices.dimensions[0] < pusch->nof_data_re ||
      pusch->data_re_indices.layout_id != OCUDU_DAPP_LAYOUT_PUSCH_DATA_RE_INDICES_U32_V1) {
    return false;
  }
  result.grid               = pointer<const uint32_t>(input.rx_grid);
  result.references         = pointer<const float2>(dmrs->reference_symbols);
  result.coordinates        = pointer<const uint16_t>(dmrs->coordinates);
  result.data_indices       = pointer<const uint32_t>(pusch->data_re_indices);
  result.stream             = reinterpret_cast<cudaStream_t>(static_cast<uintptr_t>(input.invocation.execution_stream));
  result.nof_rx_ports       = pusch->nof_rx_ports;
  result.nof_layers         = pusch->nof_layers;
  result.nof_symbols        = static_cast<uint32_t>(input.rx_grid.dimensions[1]);
  result.nof_subcarriers    = static_cast<uint32_t>(input.rx_grid.dimensions[2]);
  result.alloc_start_symbol = pusch->start_symbol;
  result.alloc_nof_symbols  = pusch->nof_symbols;
  result.alloc_start_sc     = pusch->start_subcarrier;
  result.alloc_nof_sc       = pusch->nof_subcarriers;
  result.nof_data_re        = pusch->nof_data_re;
  result.nof_pilots         = dmrs->nof_pilots;
  result.dmrs_type          = dmrs->dmrs_type;
  result.dmrs_symbol_mask   = dmrs->reserved[0];
  result.rnti               = static_cast<uint32_t>(pusch->reserved[OCUDU_DAPP_PUSCH_RESERVED_RNTI_V1]);
  result.subcarrier_spacing_hz =
      static_cast<uint32_t>(pusch->reserved[OCUDU_DAPP_PUSCH_RESERVED_SUBCARRIER_SPACING_HZ_V1]);
  result.grid_nof_subcarriers =
      static_cast<uint32_t>(pusch->reserved[OCUDU_DAPP_PUSCH_RESERVED_GRID_NOF_SUBCARRIERS_V1]);
  result.noise_variance_floor = pusch->noise_variance_floor;
  // Hosts that publish the DC location encode it as index + 1 (zero = unknown / older host).
  const uint64_t dc_plus_one = pusch->reserved[OCUDU_DAPP_PUSCH_RESERVED_DC_SUBCARRIER_PLUS_ONE_V1];
  result.dc_subcarrier       = (dc_plus_one != 0 && dc_plus_one <= result.nof_subcarriers)
                                   ? static_cast<uint32_t>(dc_plus_one - 1U)
                                   : no_dc_subcarrier;
  return true;
}

__device__ inline float2 unpack_cbf16(uint32_t value)
{
  return __bfloat1622float2(*reinterpret_cast<const __nv_bfloat162*>(&value));
}

__device__ inline float squared_norm(float2 value)
{
  return value.x * value.x + value.y * value.y;
}

__device__ inline float2 rotate(float2 value, float phase)
{
  float sine{};
  float cosine{};
  sincosf(phase, &sine, &cosine);
  return {value.x * cosine - value.y * sine, value.x * sine + value.y * cosine};
}

inline constexpr uint32_t warp_threads              = 32;
inline constexpr uint32_t channel_reduction_threads = 128;
inline constexpr uint32_t channel_reduction_warps   = channel_reduction_threads / warp_threads;
inline constexpr float    minimum_soft_demap_noise  = 1.0e-4F;

/// OCUDU baseline residual normalization. With the default FD smoothing, the
/// estimate is no longer an independent fit at each pilot subcarrier.
__device__ inline float pilot_residual_degrees_of_freedom(const inputs& input, float nof_valid_pilots)
{
  (void)input;
  return fmaxf(1.0F, nof_valid_pilots - 1.0F);
}

__device__ inline float warp_sum(float value)
{
#pragma unroll
  for (uint32_t offset = warp_threads / 2U; offset != 0; offset /= 2U) {
    value += __shfl_down_sync(0xffffffffU, value, offset);
  }
  return value;
}

__device__ inline float2 warp_sum(float2 value)
{
  value.x = warp_sum(value.x);
  value.y = warp_sum(value.y);
  return value;
}

/// Device atomic add for float2 (component-wise).
__device__ inline void atomic_add_float2(float2* address, float2 value)
{
  atomicAdd(&address->x, value.x);
  atomicAdd(&address->y, value.y);
}

/// Per-pilot LS sample for one layer set. Updates joint corr/energy accumulators.
template <uint32_t Layers>
__device__ inline bool ls_pilot_sample(const inputs& input, uint32_t port, uint32_t pilot, float2 h_out[Layers],
                                       float2 local_corr[Layers], float local_energy[Layers])
{
  const uint32_t nsc        = input.nof_subcarriers;
  const uint32_t nsym       = input.nof_symbols;
  const uint32_t plane      = nsym * nsc;
  const uint32_t symbol     = input.coordinates[2U * pilot];
  const uint32_t subcarrier = input.coordinates[2U * pilot + 1U];
  // The pilot on the transmitter DC subcarrier carries LO leakage; treating it as missing lets the
  // frequency-domain fill interpolate the channel there instead of smearing the spur into its neighbours.
  if (symbol >= nsym || subcarrier >= nsc || subcarrier == input.dc_subcarrier) {
#pragma unroll
    for (uint32_t layer = 0; layer != Layers; ++layer) {
      h_out[layer] = {0.0F, 0.0F};
    }
    return false;
  }
  const float2 received = unpack_cbf16(input.grid[port * plane + symbol * nsc + subcarrier]);
#pragma unroll
  for (uint32_t layer = 0; layer != Layers; ++layer) {
    const float2 reference = input.references[layer * input.nof_pilots + pilot];
    const float  energy    = squared_norm(reference);
    float2       h{0.0F, 0.0F};
    if (energy > 1.0e-12F) {
      h.x = (received.x * reference.x + received.y * reference.y) / energy;
      h.y = (received.y * reference.x - received.x * reference.y) / energy;
    }
    h_out[layer] = h;
    local_corr[layer].x += received.x * reference.x + received.y * reference.y;
    local_corr[layer].y += received.y * reference.x - received.x * reference.y;
    local_energy[layer] += energy;
  }
  return true;
}

inline constexpr uint32_t fd_filter_length = 15U;
inline constexpr uint32_t fd_half_filter   = fd_filter_length / 2U;

__device__ inline float fd_rc_coefficient(uint32_t index)
{
  // Keep these bit-for-bit aligned with the live fused CUDA PUSCH baseline
  // (pusch_e2e.cu::d_rc_filter). The CUDA baseline filters pilot positions
  // with 15 taps and renormalizes the in-band taps at allocation edges.
  constexpr float values[fd_filter_length] = {-0.0391585909F, -0.0287990728F, 0.0000000000F, 0.0445245383F,
                                              0.0970724536F,  0.1467045930F,  0.1821531663F, 0.1950058248F,
                                              0.1821531663F,  0.1467045930F,  0.0970724536F, 0.0445245383F,
                                              0.0000000000F,  -0.0287990728F, -0.0391585909F};
  return values[index];
}

__host__ __device__ inline uint32_t unique_pilot_capacity(uint32_t nof_pilots, uint64_t dmrs_symbol_mask,
                                                          uint32_t nof_subcarriers, uint32_t dmrs_type,
                                                          uint32_t alloc_nof_subcarriers) noexcept
{
#ifdef __CUDA_ARCH__
  const uint32_t nof_dmrs_symbols = static_cast<uint32_t>(__popcll(dmrs_symbol_mask));
#else
  const uint32_t nof_dmrs_symbols = static_cast<uint32_t>(__builtin_popcountll(dmrs_symbol_mask));
#endif
  if (nof_dmrs_symbols != 0U && nof_pilots % nof_dmrs_symbols == 0U) {
    const uint32_t capacity = nof_pilots / nof_dmrs_symbols;
    return capacity < nof_subcarriers ? capacity : nof_subcarriers;
  }
  // Backward-compatible bound for hosts that predate the reserved DMRS mask.
  // Type-1 port 1000 has 6 pilot RE/RB; type-2 has 4. A sparse RB mask only
  // makes this span-derived capacity more conservative.
  const uint32_t nof_rbs          = alloc_nof_subcarriers / 12U + (alloc_nof_subcarriers % 12U != 0U ? 1U : 0U);
  const uint32_t pilots_per_rb    = dmrs_type == 1U ? 6U : 4U;
  const uint32_t pattern_capacity = nof_rbs * pilots_per_rb;
  const uint32_t bounded_pilots   = nof_pilots < pattern_capacity ? nof_pilots : pattern_capacity;
  return bounded_pilots < nof_subcarriers ? bounded_pilots : nof_subcarriers;
}

/// Least-squares CE → phase-aligned compact H [layer][sc].
/// Layout: compact_channels[layer * compact_stride + sc]. Use compact_stride == nsc for a
/// pure compact buffer, or compact_stride == plane to park H in symbol-0 of a full plane.
/// Only the allocated SC span is filled.
template <uint32_t Layers>
__device__ inline void estimate_channels_compact(const inputs& input, uint32_t port, unsigned char* storage,
                                                 float2* compact_channels, uint32_t compact_stride,
                                                 float* phase_output = nullptr)
{
  static_assert(Layers == 1 || Layers == 2);
  const uint32_t nsc = input.nof_subcarriers;
  // Fill the full grid SC span so CE/EQ ABI planes match certifier expectations.
  // (Cost is O(nsc·gap) after the O(nof_pilots) TD path — still far cheaper than O(nsc·pilots).)
  const uint32_t sc0      = 0;
  const uint32_t sc1      = nsc;
  const uint32_t alloc_sc = nsc;

  // Shared: joint reduction, per-SC pilot count, CFO correlation and phase
  // slope, plus bounded pilot-domain FD smoothing scratch.
  auto*          joint_corr       = reinterpret_cast<float2*>(storage);
  auto*          joint_energy     = reinterpret_cast<float*>(joint_corr + Layers * channel_reduction_warps);
  auto*          sc_count         = joint_energy + Layers * channel_reduction_warps;
  const uint32_t sc_count_storage = (alloc_sc + 1U) & ~1U;
  auto*          phase_corr       = reinterpret_cast<float2*>(sc_count + sc_count_storage);
  auto*          phase_slope      = reinterpret_cast<float*>(phase_corr + Layers * channel_reduction_warps);
  const uint32_t pilot_capacity =
      unique_pilot_capacity(input.nof_pilots, input.dmrs_symbol_mask, nsc, input.dmrs_type, input.alloc_nof_sc);
  const uintptr_t metadata_address   = (reinterpret_cast<uintptr_t>(phase_slope + Layers) + 7U) & ~uintptr_t{7U};
  auto*           fd_metadata        = reinterpret_cast<uint32_t*>(metadata_address);
  auto*           pilot_scs          = reinterpret_cast<uint16_t*>(fd_metadata + 2U);
  const uint32_t  pilot_sc_storage   = (pilot_capacity + 3U) & ~3U;
  auto*           fd_extended        = reinterpret_cast<float2*>(pilot_scs + pilot_sc_storage);
  const uint32_t  fd_extended_stride = pilot_capacity;

  float2 local_corr[Layers]{};
  float  local_energy[Layers]{};

  for (uint32_t i = threadIdx.x; i < Layers * alloc_sc; i += blockDim.x) {
    const uint32_t layer                          = i / alloc_sc;
    const uint32_t sc                             = sc0 + (i % alloc_sc);
    compact_channels[layer * compact_stride + sc] = {0.0F, 0.0F};
  }
  for (uint32_t i = threadIdx.x; i < alloc_sc; i += blockDim.x) {
    sc_count[i] = 0.0F;
  }
  for (uint32_t i = threadIdx.x; i < Layers * fd_extended_stride; i += blockDim.x) {
    fd_extended[i] = {0.0F, 0.0F};
  }
  __syncthreads();

  // Estimate common phase rotation between the first and last DM-RS symbols.
  // The live host supplies the mask in dmrs.reserved[0]. With one DM-RS symbol
  // (or older hosts that omit the mask), phase compensation is a no-op.
  const uint32_t nof_dmrs_symbols = static_cast<uint32_t>(__popcll(input.dmrs_symbol_mask));
  const uint32_t first_dmrs_symbol =
      input.dmrs_symbol_mask != 0 ? static_cast<uint32_t>(__ffsll(static_cast<long long>(input.dmrs_symbol_mask)) - 1)
                                  : 0U;
  const uint32_t last_dmrs_symbol =
      input.dmrs_symbol_mask != 0 ? 63U - static_cast<uint32_t>(__clzll(input.dmrs_symbol_mask)) : first_dmrs_symbol;
  const bool phase_geometry_valid =
      nof_dmrs_symbols >= 2 && last_dmrs_symbol > first_dmrs_symbol && input.nof_pilots % nof_dmrs_symbols == 0;
  const uint32_t pilots_per_symbol = phase_geometry_valid ? input.nof_pilots / nof_dmrs_symbols : 0U;
  const uint32_t last_begin        = phase_geometry_valid ? (nof_dmrs_symbols - 1U) * pilots_per_symbol : 0U;
  float2         local_phase_corr[Layers]{};
  if constexpr (Layers == 1) {
    for (uint32_t pilot = threadIdx.x; pilot < pilots_per_symbol; pilot += blockDim.x) {
      float2     first_h[Layers]{};
      float2     last_h[Layers]{};
      float2     dummy_corr[Layers]{};
      float      dummy_energy[Layers]{};
      const bool first_ok = ls_pilot_sample<Layers>(input, port, pilot, first_h, dummy_corr, dummy_energy);
      dummy_corr[0]       = {};
      dummy_energy[0]     = 0.0F;
      const bool last_ok  = ls_pilot_sample<Layers>(input, port, last_begin + pilot, last_h, dummy_corr, dummy_energy);
      if (first_ok && last_ok &&
          input.coordinates[2U * pilot + 1U] == input.coordinates[2U * (last_begin + pilot) + 1U]) {
        local_phase_corr[0].x += last_h[0].x * first_h[0].x + last_h[0].y * first_h[0].y;
        local_phase_corr[0].y += last_h[0].y * first_h[0].x - last_h[0].x * first_h[0].y;
      }
    }
  } else {
    for (uint32_t pair = threadIdx.x; pair * 2U + 1U < pilots_per_symbol; pair += blockDim.x) {
      const uint32_t first0 = pair * 2U;
      const uint32_t first1 = first0 + 1U;
      const uint32_t last0  = last_begin + first0;
      const uint32_t last1  = last0 + 1U;
      float2         first_h0[Layers]{};
      float2         first_h1[Layers]{};
      float2         last_h0[Layers]{};
      float2         last_h1[Layers]{};
      float2         dummy_corr[Layers]{};
      float          dummy_energy[Layers]{};
      const bool     ok = ls_pilot_sample<Layers>(input, port, first0, first_h0, dummy_corr, dummy_energy) &&
                      ls_pilot_sample<Layers>(input, port, first1, first_h1, dummy_corr, dummy_energy) &&
                      ls_pilot_sample<Layers>(input, port, last0, last_h0, dummy_corr, dummy_energy) &&
                      ls_pilot_sample<Layers>(input, port, last1, last_h1, dummy_corr, dummy_energy);
      if (ok && input.coordinates[2U * first0 + 1U] == input.coordinates[2U * last0 + 1U] &&
          input.coordinates[2U * first1 + 1U] == input.coordinates[2U * last1 + 1U]) {
#pragma unroll
        for (uint32_t layer = 0; layer != Layers; ++layer) {
          const float2 first_h{0.5F * (first_h0[layer].x + first_h1[layer].x),
                               0.5F * (first_h0[layer].y + first_h1[layer].y)};
          const float2 last_h{0.5F * (last_h0[layer].x + last_h1[layer].x),
                              0.5F * (last_h0[layer].y + last_h1[layer].y)};
          local_phase_corr[layer].x += last_h.x * first_h.x + last_h.y * first_h.y;
          local_phase_corr[layer].y += last_h.y * first_h.x - last_h.x * first_h.y;
        }
      }
    }
  }

  const uint32_t lane = threadIdx.x % warp_threads;
  const uint32_t warp = threadIdx.x / warp_threads;
#pragma unroll
  for (uint32_t layer = 0; layer != Layers; ++layer) {
    local_phase_corr[layer] = warp_sum(local_phase_corr[layer]);
    if (lane == 0) {
      phase_corr[layer * channel_reduction_warps + warp] = local_phase_corr[layer];
    }
  }
  __syncthreads();
  if (warp == 0) {
#pragma unroll
    for (uint32_t layer = 0; layer != Layers; ++layer) {
      float2 correlation =
          lane < channel_reduction_warps ? phase_corr[layer * channel_reduction_warps + lane] : float2{0.0F, 0.0F};
      correlation = warp_sum(correlation);
      if (lane == 0) {
        const float magnitude = squared_norm(correlation);
        const float slope =
            phase_geometry_valid && magnitude > 1.0e-12F
                ? atan2f(correlation.y, correlation.x) / static_cast<float>(last_dmrs_symbol - first_dmrs_symbol)
                : 0.0F;
        phase_slope[layer] = slope;
        if (phase_output != nullptr) {
          phase_output[layer] = slope;
        }
      }
    }
  }
  __syncthreads();

  auto add_to_sc = [&](uint32_t symbol, uint32_t sc, const float2 h[Layers]) {
    if (sc >= nsc) {
      return;
    }
#pragma unroll
    for (uint32_t layer = 0; layer != Layers; ++layer) {
      const float phase = -phase_slope[layer] *
                          static_cast<float>(static_cast<int32_t>(symbol) - static_cast<int32_t>(first_dmrs_symbol));
      atomic_add_float2(&compact_channels[layer * compact_stride + sc], rotate(h[layer], phase));
    }
    atomicAdd(&sc_count[sc], 1.0F);
  };

  if constexpr (Layers == 2) {
    for (uint32_t pair = threadIdx.x; pair * 2U + 1U < input.nof_pilots; pair += blockDim.x) {
      const uint32_t p0 = pair * 2U;
      const uint32_t p1 = p0 + 1U;
      float2         h0[Layers]{};
      float2         h1[Layers]{};
      const bool     ok0 = ls_pilot_sample<Layers>(input, port, p0, h0, local_corr, local_energy);
      const bool     ok1 = ls_pilot_sample<Layers>(input, port, p1, h1, local_corr, local_energy);
      const uint32_t c0  = input.coordinates[2U * p0 + 1U];
      const uint32_t c1  = input.coordinates[2U * p1 + 1U];
      const uint32_t s0  = input.coordinates[2U * p0];
      const uint32_t s1  = input.coordinates[2U * p1];
      if (ok0 && ok1) {
        float2 av[Layers]{};
#pragma unroll
        for (uint32_t layer = 0; layer != Layers; ++layer) {
          av[layer] = {0.5F * (h0[layer].x + h1[layer].x), 0.5F * (h0[layer].y + h1[layer].y)};
        }
        add_to_sc(s0, c0, av);
        add_to_sc(s1, c1, av);
      } else if (ok0) {
        add_to_sc(s0, c0, h0);
      } else if (ok1) {
        add_to_sc(s1, c1, h1);
      }
    }
    if ((input.nof_pilots & 1U) != 0 && threadIdx.x == 0) {
      const uint32_t pilot = input.nof_pilots - 1U;
      float2         h[Layers]{};
      if (ls_pilot_sample<Layers>(input, port, pilot, h, local_corr, local_energy)) {
        add_to_sc(input.coordinates[2U * pilot], input.coordinates[2U * pilot + 1U], h);
      }
    }
  } else {
    for (uint32_t pilot = threadIdx.x; pilot < input.nof_pilots; pilot += blockDim.x) {
      float2 h[Layers]{};
      if (!ls_pilot_sample<Layers>(input, port, pilot, h, local_corr, local_energy)) {
        continue;
      }
      add_to_sc(input.coordinates[2U * pilot], input.coordinates[2U * pilot + 1U], h);
    }
  }

#pragma unroll
  for (uint32_t layer = 0; layer != Layers; ++layer) {
    local_corr[layer]   = warp_sum(local_corr[layer]);
    local_energy[layer] = warp_sum(local_energy[layer]);
    if (lane == 0) {
      joint_corr[layer * channel_reduction_warps + warp]   = local_corr[layer];
      joint_energy[layer * channel_reduction_warps + warp] = local_energy[layer];
    }
  }
  __syncthreads();

  float2 h_flat[Layers]{};
  if (warp == 0) {
#pragma unroll
    for (uint32_t layer = 0; layer != Layers; ++layer) {
      float2 corr =
          lane < channel_reduction_warps ? joint_corr[layer * channel_reduction_warps + lane] : float2{0.0F, 0.0F};
      float energy = lane < channel_reduction_warps ? joint_energy[layer * channel_reduction_warps + lane] : 0.0F;
      corr         = warp_sum(corr);
      energy       = warp_sum(energy);
      if (lane == 0) {
        h_flat[layer]     = energy > 1.0e-12F ? float2{corr.x / energy, corr.y / energy} : float2{1.0F, 0.0F};
        joint_corr[layer] = h_flat[layer];
      }
    }
  }
  __syncthreads();
#pragma unroll
  for (uint32_t layer = 0; layer != Layers; ++layer) {
    h_flat[layer] = joint_corr[layer];
  }

  for (uint32_t sc = threadIdx.x; sc < nsc; sc += blockDim.x) {
    const float count = sc_count[sc];
    if (count <= 0.0F) {
      continue;
    }
#pragma unroll
    for (uint32_t layer = 0; layer != Layers; ++layer) {
      float2& h = compact_channels[layer * compact_stride + sc];
      h.x /= count;
      h.y /= count;
    }
  }
  __syncthreads();

  // Build the sorted unique pilot list once. Coordinates are shared across
  // layers and repeated across DMRS symbols, so the bounded capacity is
  // nof_pilots / nof_dmrs_symbols for valid metadata.
  if (threadIdx.x == 0) {
    uint32_t pilot_count = 0U;
    bool     overflow    = false;
    for (uint32_t sc = 0; sc != nsc; ++sc) {
      if (sc_count[sc] > 0.0F) {
        if (pilot_count < pilot_capacity) {
          pilot_scs[pilot_count] = static_cast<uint16_t>(sc);
        } else {
          overflow = true;
        }
        ++pilot_count;
      }
    }
    fd_metadata[0] = overflow ? 0U : pilot_count;
    fd_metadata[1] = overflow ? 1U : 0U;
  }
  __syncthreads();

  const uint32_t pilot_count            = fd_metadata[0];
  const uint32_t pilots_per_rb          = input.dmrs_type == 1U ? 6U : 4U;
  const bool     complete_pusch_pattern = pilot_count >= pilots_per_rb && pilot_count % pilots_per_rb == 0U;
  if (complete_pusch_pattern) {
    for (uint32_t index = threadIdx.x; index < Layers * pilot_count; index += blockDim.x) {
      const uint32_t layer                            = index / pilot_count;
      const uint32_t pilot                            = index % pilot_count;
      const uint32_t sc                               = pilot_scs[pilot];
      fd_extended[layer * fd_extended_stride + pilot] = compact_channels[layer * compact_stride + sc];
    }
    __syncthreads();

    for (uint32_t index = threadIdx.x; index < Layers * pilot_count; index += blockDim.x) {
      const uint32_t layer = index / pilot_count;
      const uint32_t pilot = index % pilot_count;
      float2         filtered{0.0F, 0.0F};
      float          filter_sum = 0.0F;
      for (uint32_t tap = 0; tap != fd_filter_length; ++tap) {
        const int32_t source =
            static_cast<int32_t>(pilot) + static_cast<int32_t>(tap) - static_cast<int32_t>(fd_half_filter);
        if (source >= 0 && source < static_cast<int32_t>(pilot_count)) {
          const float2 value       = fd_extended[layer * fd_extended_stride + static_cast<uint32_t>(source)];
          const float  coefficient = fd_rc_coefficient(tap);
          filtered.x += coefficient * value.x;
          filtered.y += coefficient * value.y;
          filter_sum += coefficient;
        }
      }
      // Match pusch_e2e.cu edge handling exactly: skip out-of-band taps and
      // renormalize the remaining coefficient mass instead of extrapolating.
      if (filter_sum > 1.0e-6F) {
        filtered.x /= filter_sum;
        filtered.y /= filter_sum;
      }
      compact_channels[layer * compact_stride + pilot_scs[pilot]] = filtered;
    }
    __syncthreads();
  }

  // Outward FD linear fill / edge-hold. Use the sorted pilot metadata instead
  // of scanning left/right from every empty subcarrier. The old scan was
  // effectively O(nsc^2) for valid-but-sparse boundary inputs (for example two
  // pilot coordinates across 275 PRB) and could consume milliseconds before a
  // Class-A dApp reached its own algorithm. Binary search keeps this bounded at
  // O(nsc log(pilots)) without changing interpolation or edge semantics.
  for (uint32_t sc = threadIdx.x; sc < nsc; sc += blockDim.x) {
    if (sc_count[sc] > 0.0F) {
      continue;
    }
    uint32_t lower = 0U;
    uint32_t upper = pilot_count;
    while (lower < upper) {
      const uint32_t middle = lower + (upper - lower) / 2U;
      if (static_cast<uint32_t>(pilot_scs[middle]) < sc) {
        lower = middle + 1U;
      } else {
        upper = middle;
      }
    }
    const bool     has_right = lower < pilot_count;
    const bool     has_left  = lower > 0U;
    const uint32_t left      = has_left ? static_cast<uint32_t>(pilot_scs[lower - 1U]) : 0U;
    const uint32_t right     = has_right ? static_cast<uint32_t>(pilot_scs[lower]) : 0U;
#pragma unroll
    for (uint32_t layer = 0; layer != Layers; ++layer) {
      float2 h_sc = h_flat[layer];
      if (has_left && has_right) {
        const float2 sl = compact_channels[layer * compact_stride + left];
        const float2 sr = compact_channels[layer * compact_stride + right];
        const float  t  = static_cast<float>(sc - left) / static_cast<float>(right - left);
        h_sc.x          = sl.x + t * (sr.x - sl.x);
        h_sc.y          = sl.y + t * (sr.y - sl.y);
      } else if (has_left) {
        h_sc = compact_channels[layer * compact_stride + left];
      } else if (has_right) {
        h_sc = compact_channels[layer * compact_stride + right];
      }
      compact_channels[layer * compact_stride + sc] = h_sc;
    }
  }
  __syncthreads();
  (void)sc0;
  (void)sc1;
  (void)alloc_sc;
}

/// Expand a phase-aligned compact H into a time-dependent [layer][symbol×sc] plane.
/// compact_channels uses compact_stride (nsc or plane); plane uses plane_stride (usually plane).
template <uint32_t Layers>
__device__ inline void expand_channels_plane(const inputs& input, const float2* compact_channels,
                                             uint32_t compact_stride, const float* phase_slope,
                                             uint32_t phase_reference_symbol, float2* plane_channels,
                                             uint32_t plane_stride)
{
  static_assert(Layers == 1 || Layers == 2);
  const uint32_t nsc   = input.nof_subcarriers;
  const uint32_t nsym  = input.nof_symbols;
  const uint32_t total = Layers * nsc;
  for (uint32_t i = threadIdx.x; i < total; i += blockDim.x) {
    const uint32_t layer = i / nsc;
    const uint32_t sc    = i % nsc;
    const float2   base  = compact_channels[layer * compact_stride + sc];
    for (uint32_t symbol = 0; symbol != nsym; ++symbol) {
      const float phase = phase_slope[layer] * static_cast<float>(static_cast<int32_t>(symbol) -
                                                                  static_cast<int32_t>(phase_reference_symbol));
      plane_channels[layer * plane_stride + symbol * nsc + sc] = rotate(base, phase);
    }
  }
  __syncthreads();
}

/// Zero a full channel plane (CE/EQ ABI outputs that must be completely written).
template <uint32_t Layers>
__device__ inline void zero_channels_plane(float2* plane_channels, uint32_t plane)
{
  for (uint32_t i = threadIdx.x; i < Layers * plane; i += blockDim.x) {
    plane_channels[i] = {0.0F, 0.0F};
  }
  __syncthreads();
}

/// Full-plane CE for ABI (compact estimate + expand). `output_channels` is [layer][plane].
template <uint32_t Layers>
__device__ inline void estimate_channels_block(const inputs& input, uint32_t port, unsigned char* storage,
                                               float2* output_channels, uint32_t channel_stride)
{
  static_assert(Layers == 1 || Layers == 2);
  const uint32_t nsc   = input.nof_subcarriers;
  const uint32_t plane = input.nof_symbols * nsc;
  zero_channels_plane<Layers>(output_channels, plane);
  auto*          joint_corr       = reinterpret_cast<float2*>(storage);
  auto*          joint_energy     = reinterpret_cast<float*>(joint_corr + Layers * channel_reduction_warps);
  auto*          sc_count         = joint_energy + Layers * channel_reduction_warps;
  const uint32_t sc_count_storage = (input.nof_subcarriers + 1U) & ~1U;
  auto*          phase_corr       = reinterpret_cast<float2*>(sc_count + sc_count_storage);
  auto*          phase_slope      = reinterpret_cast<float*>(phase_corr + Layers * channel_reduction_warps);
  // Park compact H in symbol-0 of each layer (stride = channel_stride == plane).
  estimate_channels_compact<Layers>(input, port, storage, output_channels, channel_stride);
  const uint32_t phase_reference_symbol =
      input.dmrs_symbol_mask != 0 ? static_cast<uint32_t>(__ffsll(static_cast<long long>(input.dmrs_symbol_mask)) - 1)
                                  : 0U;
  expand_channels_plane<Layers>(input, output_channels, channel_stride, phase_slope, phase_reference_symbol,
                                output_channels, channel_stride);
}

/// Noise residual using symbol-flat compact H (look up by pilot SC only).
template <uint32_t Layers>
__device__ inline void estimate_noise_compact(const inputs& input, uint32_t port, const float2* compact_channels,
                                              const float* phase_slope, unsigned char* storage, float* output_noise,
                                              uint32_t output_count, float* output_scheduler_noise = nullptr)
{
  static_assert(Layers == 1 || Layers == 2);
  auto*          residual       = reinterpret_cast<float*>(storage);
  auto*          valid          = residual + channel_reduction_warps;
  float          local_residual = 0.0F;
  float          local_valid    = 0.0F;
  const uint32_t plane          = input.nof_symbols * input.nof_subcarriers;
  const uint32_t nsc            = input.nof_subcarriers;
  const uint32_t phase_reference_symbol =
      input.dmrs_symbol_mask != 0 ? static_cast<uint32_t>(__ffsll(static_cast<long long>(input.dmrs_symbol_mask)) - 1)
                                  : 0U;
  for (uint32_t pilot = threadIdx.x; pilot < input.nof_pilots; pilot += blockDim.x) {
    const uint32_t symbol     = input.coordinates[2U * pilot];
    const uint32_t subcarrier = input.coordinates[2U * pilot + 1U];
    if (symbol >= input.nof_symbols || subcarrier >= input.nof_subcarriers || subcarrier == input.dc_subcarrier) {
      continue;
    }
    const float2 received = unpack_cbf16(input.grid[port * plane + symbol * nsc + subcarrier]);
    float2       predicted{0.0F, 0.0F};
#pragma unroll
    for (uint32_t layer = 0; layer != Layers; ++layer) {
      const float  phase     = phase_slope[layer] * static_cast<float>(static_cast<int32_t>(symbol) -
                                                                       static_cast<int32_t>(phase_reference_symbol));
      const float2 channel   = rotate(compact_channels[layer * nsc + subcarrier], phase);
      const float2 reference = input.references[layer * input.nof_pilots + pilot];
      predicted.x += channel.x * reference.x - channel.y * reference.y;
      predicted.y += channel.x * reference.y + channel.y * reference.x;
    }
    local_residual += squared_norm({received.x - predicted.x, received.y - predicted.y});
    local_valid += 1.0F;
  }
  local_residual      = warp_sum(local_residual);
  local_valid         = warp_sum(local_valid);
  const uint32_t lane = threadIdx.x % warp_threads;
  const uint32_t warp = threadIdx.x / warp_threads;
  if (lane == 0) {
    residual[warp] = local_residual;
    valid[warp]    = local_valid;
  }
  __syncthreads();
  if (warp == 0) {
    float block_residual = lane < channel_reduction_warps ? residual[lane] : 0.0F;
    float block_valid    = lane < channel_reduction_warps ? valid[lane] : 0.0F;
    block_residual       = warp_sum(block_residual);
    block_valid          = warp_sum(block_valid);
    if (lane == 0) {
      const float physical_noise = block_valid <= 1.0F
                                       ? input.noise_variance_floor
                                       : fmaxf(input.noise_variance_floor,
                                               block_residual / pilot_residual_degrees_of_freedom(input, block_valid));
      for (uint32_t index = 0; index != output_count; ++index) {
        output_noise[index] = physical_noise;
        if (output_scheduler_noise != nullptr) {
          // This reference estimator does not maintain a separate channel-error
          // covariance. Report the unbiased physical disturbance rather than the
          // old self-fit residual, which is necessarily optimistic.
          output_scheduler_noise[index] = physical_noise;
        }
      }
    }
  }
}

/// Noise residual from full-plane channels (CE ABI path after expand).
template <uint32_t Layers>
__device__ inline void estimate_noise_block(const inputs& input, uint32_t port, const float2* channels,
                                            uint32_t channel_stride, unsigned char* storage, float* output_noise,
                                            uint32_t output_count, float* output_scheduler_noise = nullptr)
{
  // After expand, H is symbol-flat — pilot SC lookup via compact-equivalent indexing works
  // when channels layout is [layer][plane] and we read symbol 0 of each SC; use compact path
  // with a view that strides by plane but we only need sc: channel[layer*stride + sc] == H
  // after expand writes every symbol the same H. Reading symbol 0 is fine:
  static_assert(Layers == 1 || Layers == 2);
  auto*          residual       = reinterpret_cast<float*>(storage);
  auto*          valid          = residual + channel_reduction_warps;
  float          local_residual = 0.0F;
  float          local_valid    = 0.0F;
  const uint32_t plane          = input.nof_symbols * input.nof_subcarriers;
  const uint32_t nsc            = input.nof_subcarriers;
  for (uint32_t pilot = threadIdx.x; pilot < input.nof_pilots; pilot += blockDim.x) {
    const uint32_t symbol     = input.coordinates[2U * pilot];
    const uint32_t subcarrier = input.coordinates[2U * pilot + 1U];
    if (symbol >= input.nof_symbols || subcarrier >= input.nof_subcarriers || subcarrier == input.dc_subcarrier) {
      continue;
    }
    const uint32_t re_index = symbol * nsc + subcarrier;
    const float2   received = unpack_cbf16(input.grid[port * plane + re_index]);
    float2         predicted{0.0F, 0.0F};
#pragma unroll
    for (uint32_t layer = 0; layer != Layers; ++layer) {
      const float2 channel   = channels[layer * channel_stride + re_index];
      const float2 reference = input.references[layer * input.nof_pilots + pilot];
      predicted.x += channel.x * reference.x - channel.y * reference.y;
      predicted.y += channel.x * reference.y + channel.y * reference.x;
    }
    local_residual += squared_norm({received.x - predicted.x, received.y - predicted.y});
    local_valid += 1.0F;
  }
  local_residual      = warp_sum(local_residual);
  local_valid         = warp_sum(local_valid);
  const uint32_t lane = threadIdx.x % warp_threads;
  const uint32_t warp = threadIdx.x / warp_threads;
  if (lane == 0) {
    residual[warp] = local_residual;
    valid[warp]    = local_valid;
  }
  __syncthreads();
  if (warp == 0) {
    float block_residual = lane < channel_reduction_warps ? residual[lane] : 0.0F;
    float block_valid    = lane < channel_reduction_warps ? valid[lane] : 0.0F;
    block_residual       = warp_sum(block_residual);
    block_valid          = warp_sum(block_valid);
    if (lane == 0) {
      const float physical_noise = block_valid <= 1.0F
                                       ? input.noise_variance_floor
                                       : fmaxf(input.noise_variance_floor,
                                               block_residual / pilot_residual_degrees_of_freedom(input, block_valid));
      for (uint32_t index = 0; index != output_count; ++index) {
        output_noise[index] = physical_noise;
        if (output_scheduler_noise != nullptr) {
          // This reference estimator does not maintain a separate channel-error
          // covariance. Report the unbiased physical disturbance rather than the
          // old self-fit residual, which is necessarily optimistic.
          output_scheduler_noise[index] = physical_noise;
        }
      }
    }
  }
}

/// Per-RE equalizer output: one equalized symbol and one post-equalization
/// noise variance per layer (index 0..Layers-1). On the transmitter DC
/// subcarrier, or for a singular channel, the symbol is zero and the noise is
/// 1e6 — an erasure for the soft demapper.
struct equalized_result {
  float2 symbols[2];
  float  noise[2];
};

/// Shared-memory bytes for compact CE: joint reduction + per-SC pilot counts (full grid width).
inline uint32_t channel_setup_shared_bytes_for(uint32_t layers, uint32_t nof_subcarriers, uint32_t nof_pilots,
                                               uint64_t dmrs_symbol_mask, uint32_t dmrs_type,
                                               uint32_t alloc_nof_subcarriers) noexcept
{
  const uint32_t joint = layers * channel_reduction_warps * (static_cast<uint32_t>(sizeof(float2)) + sizeof(float));
  const uint32_t phase = layers * channel_reduction_warps * static_cast<uint32_t>(sizeof(float2)) +
                         layers * static_cast<uint32_t>(sizeof(float));
  const uint32_t sc_count_storage = (nof_subcarriers + 1U) & ~1U;
  const uint32_t pilot_capacity =
      unique_pilot_capacity(nof_pilots, dmrs_symbol_mask, nof_subcarriers, dmrs_type, alloc_nof_subcarriers);
  const uint32_t pilot_sc_storage = (pilot_capacity + 3U) & ~3U;
  uint32_t       bytes            = joint + sc_count_storage * static_cast<uint32_t>(sizeof(float)) + phase;
  bytes                           = (bytes + 7U) & ~7U;
  bytes += 2U * static_cast<uint32_t>(sizeof(uint32_t));
  bytes += pilot_sc_storage * static_cast<uint32_t>(sizeof(uint16_t));
  bytes += layers * pilot_capacity * static_cast<uint32_t>(sizeof(float2));
  return bytes;
}

/// Shared bytes of the joint-reduction segment alone (the prefix of the layout
/// sized by \ref channel_setup_shared_bytes_for).
template <uint32_t Layers>
inline constexpr uint32_t channel_setup_shared_bytes_base =
    channel_reduction_warps * Layers * (sizeof(float2) + sizeof(float));

/// Baseline MMSE with tx_scaling=1. With one layer and LLR rescaling this is
/// mathematically identical to OCUDU's ZF MRC implementation.
/// `compact_h`: when true, channels are [port][layer][sc] with channel_stride = nsc;
/// when false, channels are full [port][layer][symbol×sc] with channel_stride = plane.
template <uint32_t Layers, bool CompactH = false>
__device__ inline equalized_result equalize(const inputs& input, const float2* channels, uint32_t channel_stride,
                                            const float* port_noise, uint32_t index,
                                            const float* phase_slopes = nullptr)
{
  static_assert(Layers == 1 || Layers == 2);
  equalized_result result{};
  const uint32_t   plane = input.nof_symbols * input.nof_subcarriers;
  const uint32_t   nsc   = input.nof_subcarriers;
  const uint32_t   sc    = CompactH ? (index % nsc) : index;
  if ((index % nsc) == input.dc_subcarrier) {
    // The data RE on the transmitter DC subcarrier is dominated by LO leakage: hand the demapper an erasure
    // instead of a confident wrong symbol.
#pragma unroll
    for (uint32_t layer = 0; layer != Layers; ++layer) {
      result.symbols[layer] = {0.0F, 0.0F};
      result.noise[layer]   = 1.0e6F;
    }
    return result;
  }
  float2 matched[2]{};
  float  diagonal[2]{};
  float2 cross{0.0F, 0.0F};
  float  noise_diagonal[2]{};
  float  mmse_noise = 0.0F;
  for (uint32_t port = 0; port != input.nof_rx_ports; ++port) {
    const float2 y = unpack_cbf16(input.grid[port * plane + index]);
    float2       h0 =
        CompactH ? channels[(port * Layers) * channel_stride + sc] : channels[(port * Layers) * channel_stride + index];
    if constexpr (CompactH) {
      if (phase_slopes != nullptr) {
        const uint32_t symbol = index / nsc;
        const uint32_t phase_reference_symbol =
            input.dmrs_symbol_mask != 0
                ? static_cast<uint32_t>(__ffsll(static_cast<long long>(input.dmrs_symbol_mask)) - 1)
                : 0U;
        h0 = rotate(h0, phase_slopes[port * Layers] * static_cast<float>(static_cast<int32_t>(symbol) -
                                                                         static_cast<int32_t>(phase_reference_symbol)));
      }
    }
    matched[0].x += y.x * h0.x + y.y * h0.y;
    matched[0].y += y.y * h0.x - y.x * h0.y;
    diagonal[0] += squared_norm(h0);
    noise_diagonal[0] += port_noise[port] * squared_norm(h0);
    if constexpr (Layers == 2) {
      float2 h1 = CompactH ? channels[(port * Layers + 1U) * channel_stride + sc]
                           : channels[(port * Layers + 1U) * channel_stride + index];
      if constexpr (CompactH) {
        if (phase_slopes != nullptr) {
          const uint32_t symbol = index / nsc;
          const uint32_t phase_reference_symbol =
              input.dmrs_symbol_mask != 0
                  ? static_cast<uint32_t>(__ffsll(static_cast<long long>(input.dmrs_symbol_mask)) - 1)
                  : 0U;
          h1 = rotate(
              h1, phase_slopes[port * Layers + 1U] *
                      static_cast<float>(static_cast<int32_t>(symbol) - static_cast<int32_t>(phase_reference_symbol)));
        }
      }
      matched[1].x += y.x * h1.x + y.y * h1.y;
      matched[1].y += y.y * h1.x - y.x * h1.y;
      diagonal[1] += squared_norm(h1);
      cross.x += h0.x * h1.x + h0.y * h1.y;
      cross.y += h0.x * h1.y - h0.y * h1.x;
      mmse_noise = fmaxf(mmse_noise, port_noise[port]);
    }
  }
  if constexpr (Layers == 1) {
    const float denominator = fmaxf(diagonal[0], 1.0e-12F);
    if (diagonal[0] <= 1.0e-12F) {
      result.symbols[0] = {0.0F, 0.0F};
      result.noise[0]   = 1.0e6F;
    } else {
      result.symbols[0] = {matched[0].x / denominator, matched[0].y / denominator};
      result.noise[0]   = fmaxf(fmaxf(input.noise_variance_floor, minimum_soft_demap_noise),
                                noise_diagonal[0] / (denominator * denominator));
    }
  } else {
    const float  gram0         = diagonal[0] + mmse_noise;
    const float  gram1         = diagonal[1] + mmse_noise;
    const float  determinant   = gram0 * gram1 - squared_norm(cross);
    const float  correction0   = 1.0F - mmse_noise * gram1 / determinant;
    const float  correction1   = 1.0F - mmse_noise * gram0 / determinant;
    const float2 cross_b1      = {cross.x * matched[1].x - cross.y * matched[1].y,
                                  cross.x * matched[1].y + cross.y * matched[1].x};
    const float2 conj_cross_b0 = {cross.x * matched[0].x + cross.y * matched[0].y,
                                  cross.x * matched[0].y - cross.y * matched[0].x};
    if (determinant <= 1.0e-12F || correction0 <= 1.0e-12F || correction1 <= 1.0e-12F) {
      result.symbols[0] = {0.0F, 0.0F};
      result.symbols[1] = {0.0F, 0.0F};
      result.noise[0]   = 1.0e6F;
      result.noise[1]   = 1.0e6F;
    } else {
      result.symbols[0] = {(gram1 * matched[0].x - cross_b1.x) / (determinant * correction0),
                           (gram1 * matched[0].y - cross_b1.y) / (determinant * correction0)};
      result.symbols[1] = {(gram0 * matched[1].x - conj_cross_b0.x) / (determinant * correction1),
                           (gram0 * matched[1].y - conj_cross_b0.y) / (determinant * correction1)};
      result.noise[0]   = fmaxf(fmaxf(input.noise_variance_floor, minimum_soft_demap_noise), 1.0F / correction0 - 1.0F);
      result.noise[1]   = fmaxf(fmaxf(input.noise_variance_floor, minimum_soft_demap_noise), 1.0F / correction1 - 1.0F);
    }
  }
  return result;
}

/// Map the sticky CUDA launch error to an ABI status after enqueuing kernels.
/// Uses cudaPeekAtLastError, so the error is observed without being cleared
/// and no synchronization is introduced.
inline ocudu_dapp_status_v1 launch_status() noexcept
{
  return cudaPeekAtLastError() == cudaSuccess ? OCUDU_DAPP_OK_V1 : OCUDU_DAPP_INTERNAL_ERROR_V1;
}

} // namespace ocudu::dapp::reference::cuda_contract
