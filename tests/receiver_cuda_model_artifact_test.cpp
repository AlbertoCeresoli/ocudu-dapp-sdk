// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/reference_phy_algorithms_cuda.h"
#include <cmath>
#include <gtest/gtest.h>

using namespace ocudu::dapp::reference;

TEST(receiver_cuda_model_artifact, creates_bounded_scaled_nr_qam_levels)
{
  const auto baseline = make_receiver_cuda_model_artifact_v1();
  const auto scaled   = make_receiver_cuda_model_artifact_v1(0.75F);

  EXPECT_EQ(baseline.magic, receiver_cuda_model_magic_v1);
  EXPECT_EQ(baseline.version, receiver_cuda_model_version_v1);
  EXPECT_EQ(baseline.level_count, receiver_cuda_model_level_count_v1);
  for (uint32_t bits = 1; bits <= 4; ++bits) {
    const uint32_t offset = (bits - 1U) * 16U;
    const uint32_t count  = 1U << bits;
    for (uint32_t first = 0; first != count; ++first) {
      EXPECT_TRUE(std::isfinite(baseline.constellation_levels[offset + first]));
      EXPECT_FLOAT_EQ(scaled.constellation_levels[offset + first],
                      0.75F * baseline.constellation_levels[offset + first]);
      for (uint32_t second = first + 1; second != count; ++second) {
        EXPECT_NE(baseline.constellation_levels[offset + first], baseline.constellation_levels[offset + second]);
      }
    }
    for (uint32_t unused = count; unused != 16U; ++unused) {
      EXPECT_EQ(baseline.constellation_levels[offset + unused], 0.0F);
    }
  }
}
