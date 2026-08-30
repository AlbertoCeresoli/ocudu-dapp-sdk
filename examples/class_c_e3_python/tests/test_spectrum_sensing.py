#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Offline unit tests for Python Class-C spectrum sensing (PyTorch required)."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

try:
    import torch
except ImportError as error:  # pragma: no cover
    raise SystemExit(
        "PyTorch is required for example.class_c_e3_python tests. pip install torch"
    ) from error

from sensing.spectrum_sensing import radiometer, select_device, synthetic_spectrum_grid  # noqa: E402


class SpectrumSensingTest(unittest.TestCase):
    def test_select_device_prefers_cuda_when_available(self) -> None:
        dev = select_device(None)
        if torch.cuda.is_available():
            self.assertEqual(dev.type, "cuda")
        else:
            self.assertEqual(dev.type, "cpu")

    def test_radiometer_detects_occupied_prbs(self) -> None:
        grid = synthetic_spectrum_grid(prbs=25, occupied_prbs=(2, 3, 4), signal_amp=3.0, noise_sigma=0.05, seed=1)
        result = radiometer(grid, power_threshold=0.5)
        self.assertEqual(result.backend, "torch")
        self.assertEqual(result.nof_prbs, 25)
        self.assertGreater(result.occupied_fraction, 0.05)
        self.assertLess(result.occupied_fraction, 0.5)
        self.assertGreater(result.mean_power, 0.0)

    def test_explicit_cpu_device(self) -> None:
        grid = synthetic_spectrum_grid(prbs=12, occupied_prbs=(1,), seed=2)
        result = radiometer(grid, device="cpu")
        self.assertEqual(result.device, "cpu")
        self.assertEqual(result.nof_subcarriers, 12 * 12)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
