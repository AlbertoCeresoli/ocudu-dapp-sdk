// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

/// \file
/// Umbrella header for the open Class-A CUDA reference algorithms in
/// `ocudu::dapp::reference`: channel estimator (CE), equalizer (CE + EQ) and
/// full receiver (CE + EQ + demap). Include the individual headers for one
/// depth only; commercial trees should prefer the vendor-stable entry points
/// in `ref_class_a_cuda.h` instead.

#pragma once

#include "channel_estimator_cuda.h"
#include "equalizer_cuda.h"
#include "receiver_cuda.h"
