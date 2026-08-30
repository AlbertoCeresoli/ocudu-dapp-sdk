#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

import torch  # noqa: E402

from sensing.srs_cir import estimate_cir, synthetic_srs_channel  # noqa: E402


class SrsCirTest(unittest.TestCase):
    def test_finds_injected_delay_bin(self) -> None:
        h = synthetic_srs_channel(frequency_points=48, delay_bin=5, snr_db=30.0, seed=1)
        r = estimate_cir(h, noise_variance=0.01, device="cpu")
        self.assertEqual(r.backend, "torch")
        self.assertGreater(r.tap_count, 5)
        # Peak should be near injected delay under high SNR.
        self.assertLessEqual(abs(r.peak_delay_bin - 5), 1)
        self.assertGreater(r.peak_cir_power, r.mean_channel_power * 0.1)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
