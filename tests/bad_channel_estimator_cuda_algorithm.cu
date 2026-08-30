// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/channel_estimator_cuda.h"
#include <cuda_runtime.h>

namespace ocudu::dapp::reference {
#if !defined(OCUDU_CERTIFIER_BAD_STORAGE_SUBSTITUTION)
namespace {

__global__ void write_after_output(float2* output, size_t count)
{
  output[count] = {0.0F, 0.0F};
}

} // namespace
#endif

ocudu_dapp_status_v1 estimate_channel_cuda(const ocudu_dapp_channel_estimator_input_v1& input,
                                           ocudu_dapp_channel_estimator_output_v1&      output,
                                           const channel_estimator_cuda_state&, channel_estimator_cuda_worker_state&)
{
#if defined(OCUDU_CERTIFIER_BAD_STORAGE_SUBSTITUTION)
  output.channel_estimates.native_handle += output.channel_estimates.byte_offset;
  output.channel_estimates.byte_offset = 0;
  return OCUDU_DAPP_OK_V1;
#else
  auto* output_pointer = reinterpret_cast<float2*>(static_cast<uintptr_t>(output.channel_estimates.native_handle) +
                                                   output.channel_estimates.byte_offset);
  const size_t count   = output.channel_estimates.byte_length / sizeof(float2);
  const auto   stream  = reinterpret_cast<cudaStream_t>(input.invocation.execution_stream);
  write_after_output<<<1, 1, 0, stream>>>(output_pointer, count);
  return cudaPeekAtLastError() == cudaSuccess ? OCUDU_DAPP_OK_V1 : OCUDU_DAPP_DEVICE_ERROR_V1;
#endif
}

} // namespace ocudu::dapp::reference
