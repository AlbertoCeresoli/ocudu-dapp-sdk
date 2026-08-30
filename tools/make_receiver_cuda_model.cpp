// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/receiver_cuda.h"
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>

using ocudu::dapp::reference::make_receiver_cuda_model_artifact_v1;

int main(int argc, char** argv)
{
  if (argc < 2 || argc > 3) {
    std::cerr << "Usage: make_receiver_cuda_model OUTPUT [SCALE]\n";
    return 2;
  }

  float scale = 1.0F;
  if (argc == 3) {
    char* end = nullptr;
    errno     = 0;
    scale     = std::strtof(argv[2], &end);
    if (errno != 0 || end == argv[2] || *end != '\0' || !std::isfinite(scale) || scale <= 0.0F) {
      std::cerr << "SCALE must be a finite positive number\n";
      return 2;
    }
  }

  const auto    artifact = make_receiver_cuda_model_artifact_v1(scale);
  std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(&artifact), sizeof(artifact));
  if (!output) {
    std::cerr << "Failed to write receiver model artifact\n";
    return 1;
  }
  return 0;
}
