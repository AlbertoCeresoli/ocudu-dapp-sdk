// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once
#include "ocudu/dapp/use_cases/v1/receiver.h"
#include <cstdint>
#include <initializer_list>
#include <limits>

namespace ocudu::dapp::reference {
// Pointer-domain views only, after the receiver's normal tensor validation.
// Check the full declared lease, not just the elements written this grant.
inline bool receiver_symbol_storage_is_disjoint(const ocudu_dapp_tensor_view_v1& symbols,
                                                const ocudu_dapp_receiver_input_v1& input,
                                                const ocudu_dapp_receiver_output_v1& output,
                                                const ocudu_dapp_pusch_metadata_v1& pusch,
                                                const ocudu_dapp_dmrs_metadata_v1& dmrs,
                                                const ocudu_dapp_tensor_view_v1& noise) noexcept
{
  const auto interval = [](const ocudu_dapp_tensor_view_v1& v, uint64_t& begin, uint64_t& end) {
    constexpr auto maximum = std::numeric_limits<uintptr_t>::max();
    if (v.native_handle == 0 || v.native_handle > maximum || v.byte_offset > maximum - v.native_handle) {
      return false;
    }
    begin = v.native_handle + v.byte_offset;
    if (v.byte_length == 0 || v.byte_length > maximum - begin) {
      return false;
    }
    end = begin + v.byte_length;
    return true;
  };
  uint64_t begin{}, end{};
  // CUDA float2 stores require eight-byte alignment; host CF32 may use four.
  const uint64_t alignment = symbols.domain == OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1 ? 2 * sizeof(float) : alignof(float);
  if (!interval(symbols, begin, end) || begin % alignment != 0) {
    return false;
  }
  for (const auto* view : {&input.rx_grid, &dmrs.reference_symbols, &dmrs.coordinates, &pusch.data_re_indices,
                           &pusch.data_re_mask, &output.llrs, &noise}) {
    if (view->struct_size == 0) {
      continue;
    }
    uint64_t other_begin{}, other_end{};
    if (!interval(*view, other_begin, other_end) || (begin < other_end && other_begin < end)) {
      return false;
    }
  }
  if (symbols.domain != OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1) {
    for (const auto* wrapper : {&input.pusch, &input.dmrs, &output.metrics}) {
      const auto other = reinterpret_cast<uintptr_t>(wrapper->metadata.data);
      if (wrapper->metadata.size > std::numeric_limits<uintptr_t>::max() - other ||
          (begin < other + wrapper->metadata.size && other < end)) {
        return false;
      }
    }
  }
  return true;
}
} // namespace ocudu::dapp::reference
