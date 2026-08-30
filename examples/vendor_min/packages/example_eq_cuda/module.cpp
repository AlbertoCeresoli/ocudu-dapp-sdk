// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Query trampoline only — keep this TU tiny and pure.
//
// The host may call ocudu_dapp_query_v1 repeatedly (preflight, catalog inspect,
// DU admission). Do not initialize CUDA, open files, or spawn threads here.
// All product state belongs in create/prepare/start (module_support.cpp).

#include "ocudu/dapp/abi/v1/module.h"

ocudu_dapp_status_v1 populate_example_eq_cuda_module(const ocudu_dapp_host_info_v1* host,
                                                     ocudu_dapp_module_v1*          output) noexcept;

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1* host,
                                                                         ocudu_dapp_module_v1*          output)
{
  return populate_example_eq_cuda_module(host, output);
}
