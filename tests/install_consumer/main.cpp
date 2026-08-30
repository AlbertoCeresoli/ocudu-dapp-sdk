// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/abi/v1/base.h"
#include "ocudu_dapp_sdk/class_c_algorithms.h"
#include "ocudu_dapp_sdk/class_c_input.h"
#include "ocudu_dapp_sdk/module_support.h"
#include "ocudu_dapp_sdk/process_client.h"
#include "ocudu_dapp_sdk/reference_phy_algorithms.h"
#include "ocudu_dapp_sdk/reference_scheduler_pipeline.h"

static_assert(OCUDU_DAPP_ABI_MAJOR_V1 == 1u);

int main()
{
  ocudu::dapp::reference::strided_complex_view       tensor{};
  ocudu::dapp::reference::class_c_input              input{};
  ocudu::dapp::reference::scheduler_reference_config scheduler{};
  ocudu::dapp::sdk::process_interface_expectation    process{};
  return tensor.rank == 0 && input.source_sequence == 0 && !scheduler.emit_layers && process.interface_id == 0 ? 0 : 1;
}
