// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/use_cases/v1/receiver.h"
#include <cmath>
#include <cstddef>
#include <type_traits>

namespace ocudu::dapp::reference {

/// Instance-owned read-only CUDA state used by the receiver example.
struct receiver_cuda_state {
  const float* constellation_levels{};
};

/// Per-worker CUDA scratch prepared outside invocation for concurrent receiver lanes.
struct receiver_cuda_worker_state {
  void*       storage{};
  uint32_t    nof_rx_ports{};
  uint32_t    nof_layers{};
  std::size_t storage_bytes{};
};

inline constexpr uint32_t receiver_cuda_model_magic_v1       = 0x52434d31U;
inline constexpr uint16_t receiver_cuda_model_version_v1     = 1;
inline constexpr uint16_t receiver_cuda_model_level_count_v1 = 64;

/// Reference-private pointer-free model artifact accepted by the CUDA receiver example.
struct receiver_cuda_model_artifact_v1 {
  uint32_t magic{receiver_cuda_model_magic_v1};
  uint16_t version{receiver_cuda_model_version_v1};
  uint16_t level_count{receiver_cuda_model_level_count_v1};
  float    constellation_levels[receiver_cuda_model_level_count_v1]{};
};
static_assert(sizeof(receiver_cuda_model_artifact_v1) == 264);
static_assert(std::is_trivially_copyable_v<receiver_cuda_model_artifact_v1>);

/// Build the pointer-free reference receiver artifact used by E3 model management.
inline receiver_cuda_model_artifact_v1 make_receiver_cuda_model_artifact_v1(float scale = 1.0F) noexcept
{
  receiver_cuda_model_artifact_v1 result;
  for (uint32_t bits = 1; bits <= 4; ++bits) {
    const uint32_t count = 1U << bits;
    for (uint32_t label = 0; label != count; ++label) {
      float amplitude = 1.0F;
      float step      = 2.0F;
      for (uint32_t bit = bits; bit > 1; --bit) {
        const float direction = ((label >> (bit - 1U)) & 1U) == 0 ? 1.0F : -1.0F;
        amplitude             = step - direction * amplitude;
        step *= 2.0F;
      }
      const float sign          = (label & 1U) == 0 ? 1.0F : -1.0F;
      const float normalization = std::sqrt(2.0F * static_cast<float>((1U << (2U * bits)) - 1U) / 3.0F);
      result.constellation_levels[(bits - 1U) * 16U + label] = scale * sign * amplitude / normalization;
    }
  }
  return result;
}

ocudu_dapp_status_v1 initialize_receiver_cuda_state(receiver_cuda_state& state, const float* levels = nullptr) noexcept;
void                 release_receiver_cuda_state(receiver_cuda_state& state) noexcept;
ocudu_dapp_status_v1 initialize_receiver_cuda_worker_state(receiver_cuda_worker_state& state,
                                                           uint32_t                    max_nof_rx_ports = 64,
                                                           uint32_t                    max_nof_layers   = 2) noexcept;
void                 release_receiver_cuda_worker_state(receiver_cuda_worker_state& state) noexcept;

/// Queue the standalone CE+EQ+demapping reference on the host-provided CUDA stream.
ocudu_dapp_status_v1 receive_cuda(const ocudu_dapp_receiver_input_v1& input, ocudu_dapp_receiver_output_v1& output,
                                  const receiver_cuda_state& state, receiver_cuda_worker_state& worker);

} // namespace ocudu::dapp::reference
