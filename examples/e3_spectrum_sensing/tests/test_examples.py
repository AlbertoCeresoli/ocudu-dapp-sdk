# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Smoke tests for the spectrum sensing example (argument parsing and detector logic)."""

import subprocess
import sys
import unittest
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))


class HelpTest(unittest.TestCase):
    def test_help(self) -> None:
        result = subprocess.run([sys.executable, str(HERE / "emitter_detector.py"), "--help"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--threshold-db", result.stdout)


class DetectorTest(unittest.TestCase):
    def test_reports_unscheduled_energy_and_ignores_scheduled(self) -> None:
        from emitter_detector import Detector
        from ocudu_e3.classc import SpectrumSnapshot

        rng = np.random.default_rng(1)
        symbols, prb = 14, 24
        occupancy = np.full((symbols, prb), 1, dtype=np.uint8)  # planned empty
        occupancy[:, 20:24] = 2  # our PUSCH

        def snapshot(sfn, slot, emitter):
            grid = (rng.normal(size=(1, symbols, prb * 12)) + 1j * rng.normal(size=(1, symbols, prb * 12))) * 0.01
            grid[:, :, 20 * 12:] *= 30  # strong PUSCH where scheduled: must not be reported
            if emitter:
                grid[:, :, 5 * 12:8 * 12] *= 30  # external emitter on PRB 5..7
            return SpectrumSnapshot(0, sfn, slot, 1, 0, sfn * 10_000_000 + slot * 500_000, 3_500_000_000, 24 * 12 * 30_000,
                                    1, symbols, prb * 12, 1.0, 0, "symbol_prb", True, occupancy, grid.astype(np.complex64))

        detector = Detector(threshold_db=10.0, min_slots=2, floor_alpha=0.05)
        events = []
        for i in range(30):
            events += detector.detect(snapshot(i // 20, i % 20, emitter=False))
        self.assertEqual(events, [], "scheduled PUSCH must not be reported")
        for i in range(30, 40):
            events += detector.detect(snapshot(i // 20, i % 20, emitter=True))
        starts = [e for e in events if e["event"] == "emitter"]
        self.assertEqual(len(starts), 1)
        self.assertEqual((starts[0]["prb_first"], starts[0]["prb_last"]), (5, 7))
        self.assertAlmostEqual(starts[0]["bandwidth_hz"], 3 * 12 * 30_000)
        for i in range(40, 50):
            events += detector.detect(snapshot(i // 20, i % 20, emitter=False))
        self.assertEqual(len([e for e in events if e["event"] == "emitter_end"]), 1)


if __name__ == "__main__":
    unittest.main()
