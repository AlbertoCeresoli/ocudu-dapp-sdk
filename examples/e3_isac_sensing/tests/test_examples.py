# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Smoke tests for the SRS-ISAC example: argument parsing and the range-Doppler pipeline."""

import subprocess
import sys
import unittest
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))


class HelpTest(unittest.TestCase):
    def test_help(self) -> None:
        result = subprocess.run([sys.executable, str(HERE / "srs_isac_monitor.py"), "--help"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--cpi", result.stdout)


class PipelineTest(unittest.TestCase):
    def test_detects_moving_reflector_and_ignores_static_channel(self) -> None:
        from srs_isac_monitor import IsacProcessor, slot_index_of

        n, comb, scs, fc = 120, 4, 30_000, 3.41e9
        tone = comb * scs
        period_s = 0.010
        rng = np.random.default_rng(3)
        k = np.arange(n)
        static = 1.0 * np.exp(-2j * np.pi * k * tone * 0.0) + 0.4 * np.exp(-2j * np.pi * k * tone * 200e-9)
        target_delay, target_speed = 700e-9, 2.0  # 210 m bistatic, 2 m/s -> 45 Hz Doppler
        doppler = 2 * target_speed * fc / 299_792_458.0

        proc = IsacProcessor(cpi=64, clutter_alpha=0.05, threshold_db=15.0, guard_bins=1, srs_period_ms=10.0, carrier_hz=fc)
        reports = []
        for i in range(400):
            t = i * period_s
            h = static + 0.005 * (rng.normal(size=n) + 1j * rng.normal(size=n))
            if i >= 200:
                h = h + 0.15 * np.exp(-2j * np.pi * k * tone * target_delay) * np.exp(2j * np.pi * doppler * t)
            sfn, slot = divmod(i * 20, 20)  # one occasion every 20 slots (10 ms at 30 kHz)
            reports += proc.push(0x4601, 0, 0, h, tone, slot_index_of(sfn, slot, 1), sfn, slot, fc)
            if i == 199:
                self.assertEqual(reports, [], "static channel must produce no reports")
        self.assertGreater(len(reports), 0)
        best = max(reports, key=lambda r: r.snr_db)
        self.assertAlmostEqual(best.range_m, target_delay * 299_792_458.0, delta=25.0)  # within ~2 range bins
        # Sign convention (path length increasing vs decreasing) is not pinned down by this example.
        self.assertAlmostEqual(abs(best.speed_mps), target_speed, delta=0.35)

    def test_gap_resets_window(self) -> None:
        from srs_isac_monitor import IsacProcessor, slot_index_of

        proc = IsacProcessor(cpi=8, clutter_alpha=0.1, threshold_db=10.0, guard_bins=1, srs_period_ms=10.0, carrier_hz=3.4e9)
        h = np.ones(120, dtype=np.complex64)
        for i in range(6):
            proc.push(1, 0, 0, h, 120_000, slot_index_of(0, i * 20, 1), 0, i * 20, 3.4e9)
        proc.push(1, 0, 0, h, 120_000, slot_index_of(0, 6 * 20 + 40, 1), 0, 6 * 20 + 40, 3.4e9)  # missed occasion
        self.assertEqual(proc.stats["gaps"], 1)
        self.assertLessEqual(len(proc.links[(1, 0, 0)].window), 1)


if __name__ == "__main__":
    unittest.main()
