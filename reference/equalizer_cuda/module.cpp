// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/abi/v1/module.h"

ocudu_dapp_status_v1 populate_equalizer_cuda_module(const ocudu_dapp_host_info_v1* host,
                                                    ocudu_dapp_module_v1*          output) noexcept;

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1* host,
                                                                         ocudu_dapp_module_v1*          output)
{
  return populate_equalizer_cuda_module(host, output);
}
