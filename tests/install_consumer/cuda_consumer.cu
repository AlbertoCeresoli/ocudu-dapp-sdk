// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/channel_estimator_cuda.h"
#include "ocudu_dapp_sdk/equalizer_cuda.h"
#include "ocudu_dapp_sdk/process_cuda_client.h"
#include "ocudu_dapp_sdk/receiver_cuda.h"
#include "ocudu_dapp_sdk/spectrum_cuda.h"
#include "ocudu_dapp_sdk/srs_isac_cuda.h"

int main()
{
  ocudu::dapp::sdk::cuda_publication_view view{};
  return view.size == 0 ? 0 : 1;
}
