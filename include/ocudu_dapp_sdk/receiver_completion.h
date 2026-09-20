// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once
#include "ocudu/dapp/use_cases/v1/receiver_completion.h"
#include <cstddef>

namespace ocudu::dapp::reference {
inline bool host_supports_receiver_completion(const ocudu_dapp_host_info_v1* host) noexcept
{
  return host && host->struct_size >= offsetof(ocudu_dapp_host_info_v1, reserved) &&
         host->abi_major == OCUDU_DAPP_ABI_MAJOR_V1 &&
         (host->host_capabilities & OCUDU_DAPP_HOST_CAP_RECEIVER_COMPLETION_V1) != 0;
}

/// Call this check in the ordinary module query too, not just the extension
/// query: an older host never looks up the new symbol. The invoke-only callback
/// of a completion-required module should decline without enqueueing work.
inline ocudu_dapp_status_v1
populate_receiver_completion(const ocudu_dapp_host_info_v1* host, ocudu_dapp_receiver_completion_api_v1* output,
                             const ocudu_dapp_receiver_completion_api_v1& implementation) noexcept
{
  if (!host_supports_receiver_completion(host))
    return OCUDU_DAPP_UNSUPPORTED_V1;
  if (!output || output->struct_size < sizeof(*output) || output->abi_major != OCUDU_DAPP_ABI_MAJOR_V1)
    return OCUDU_DAPP_INVALID_INPUT_V1;
  if (implementation.struct_size != sizeof(implementation) || implementation.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
      implementation.abi_minor || !implementation.invoke || !implementation.inspect || !implementation.release ||
      !implementation.retire)
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  for (auto value : implementation.reserved)
    if (value)
      return OCUDU_DAPP_INVALID_OUTPUT_V1;
  *output = implementation;
  return OCUDU_DAPP_OK_V1;
}
} // namespace ocudu::dapp::reference
