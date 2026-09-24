# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""SINR router tests: the routing policy offline, and a live run against the OCUDU loopback
E3 agent (which publishes pusch.symbols for two UEs) when ``OCUDU_E3AP_TEST_SERVER`` is set."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
from sinr_router import SinrRoutePolicy  # noqa: E402


def feed(policy: SinrRoutePolicy, rnti: int, sinr_db: float, start: float, seconds: float, rate_hz: float = 50.0):
    samples = int(seconds * rate_hz)
    for index in range(samples):
        policy.observe(rnti, sinr_db, start + index / rate_hz)
    return start + seconds


class PolicyTests(unittest.TestCase):
    def test_a_new_ue_is_routed_by_its_mean_once_enough_grants_arrived(self) -> None:
        policy = SinrRoutePolicy(threshold_db=12.0, min_samples=5)
        policy.observe(0x4601, 8.0, 0.0)
        self.assertEqual(policy.decide(0.1), ({}, []), "too few grants to decide")
        now = feed(policy, 0x4601, 8.0, 0.1, 1.0)
        now = feed(policy, 0x4602, 18.0, 0.1, 1.0)
        self.assertEqual(policy.decide(now), ({0x4601: "below", 0x4602: "above"}, []))
        self.assertEqual(policy.decide(now + 0.01), ({}, []), "only changes are reported")

    def test_hysteresis_keeps_a_ue_inside_the_band_where_it_is(self) -> None:
        policy = SinrRoutePolicy(threshold_db=12.0, hysteresis_db=2.0, hold_s=0.0, window_s=1.0)
        now = feed(policy, 0x4601, 16.0, 0.0, 1.0)
        self.assertEqual(policy.decide(now)[0], {0x4601: "above"})
        now = feed(policy, 0x4601, 11.5, now, 1.0)  # under the threshold, inside the 11..13 dB band
        self.assertEqual(policy.decide(now)[0], {})
        now = feed(policy, 0x4601, 10.5, now, 1.0)  # below the band
        self.assertEqual(policy.decide(now)[0], {0x4601: "below"})
        now = feed(policy, 0x4601, 12.8, now, 1.0)  # back over the threshold but inside the band
        self.assertEqual(policy.decide(now)[0], {})

    def test_the_hold_time_limits_how_fast_a_ue_can_flap(self) -> None:
        policy = SinrRoutePolicy(threshold_db=12.0, hysteresis_db=0.0, hold_s=3.0, window_s=0.5)
        now = feed(policy, 0x4601, 20.0, 0.0, 0.5)
        self.assertEqual(policy.decide(now)[0], {0x4601: "above"})
        switched = now
        now = feed(policy, 0x4601, 5.0, now, 0.5)
        self.assertEqual(policy.decide(now)[0], {}, "still inside the hold time")
        now = feed(policy, 0x4601, 5.0, now, switched + 3.1 - now)
        self.assertEqual(policy.decide(now)[0], {0x4601: "below"})

    def test_an_idle_routed_ue_is_reported_stale_and_forgotten(self) -> None:
        policy = SinrRoutePolicy(stale_s=5.0)
        now = feed(policy, 0x4601, 20.0, 0.0, 1.0)
        feed(policy, 0x4602, 20.0, 0.0, 0.01)  # too few grants: never routed
        self.assertEqual(policy.decide(now)[0], {0x4601: "above"})
        self.assertEqual(policy.decide(now + 6.0), ({}, [0x4601]))
        self.assertEqual(policy.ues, {})
        self.assertEqual(policy.decide(now + 7.0), ({}, []))

    def test_invalid_parameters_are_rejected(self) -> None:
        for arguments in ({"hysteresis_db": -1.0}, {"window_s": 0.0}, {"stale_s": 0.0}, {"min_samples": 0}):
            with self.assertRaises(ValueError):
                SinrRoutePolicy(**arguments)

    def test_help_parses(self) -> None:
        result = subprocess.run([sys.executable, str(HERE / "sinr_router.py"), "--help"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--threshold-db", result.stdout)


@unittest.skipUnless(os.environ.get("OCUDU_E3AP_TEST_SERVER"), "OCUDU_E3AP_TEST_SERVER not set")
class LiveTests(unittest.TestCase):
    def test_routes_both_ues_and_leaves_the_table_as_it_found_it(self) -> None:
        sys.path.insert(0, str(HERE))
        from _common import connect  # noqa: E402

        with tempfile.TemporaryDirectory() as catalog:
            agent = subprocess.Popen(
                [os.environ["OCUDU_E3AP_TEST_SERVER"], "--catalog", catalog, "--e3dp", "0"],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            try:
                port = 0
                while True:
                    line = agent.stdout.readline()
                    if not line:
                        break
                    if line.startswith("e3ap_port"):
                        port = int(line.split()[1])
                    if line.startswith("ready"):
                        break
                self.assertNotEqual(port, 0)
                # The loopback UEs report 20..24 dB: with a 30 dB threshold both go below.
                run = subprocess.run(
                    [sys.executable, str(HERE / "sinr_router.py"), "--port", str(port), "--below", "conventional",
                     "--above", "primary", "--threshold-db", "30", "--duration", "4", "--period-s", "0.5",
                     "--hold-s", "0", "--min-samples", "3"],
                    capture_output=True, text=True, timeout=60)
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertIn("rnti 0x4601", run.stdout)
                self.assertIn("rnti 0x4602", run.stdout)
                self.assertIn("-> below (conventional)", run.stdout)
                self.assertIn("removed 2 entries", run.stdout)
                with connect("127.0.0.1", port, "sdk-router-check") as client:
                    client.setup()
                    table = client.pusch_routing()["payload"]["puschRouting"]
                    self.assertEqual(table["entries"], [])
                    client.release()

                refused = subprocess.run(
                    [sys.executable, str(HERE / "sinr_router.py"), "--port", str(port), "--below",
                     "00112233-4455-6677-8899-aabbccddeeff", "--above", "conventional", "--duration", "1"],
                    capture_output=True, text=True, timeout=60)
                self.assertNotEqual(refused.returncode, 0)
                self.assertIn("is not active", refused.stderr)
            finally:
                try:
                    agent.stdin.write("quit\n")
                    agent.stdin.flush()
                    agent.wait(timeout=5)
                except Exception:
                    agent.kill()
                time.sleep(0.1)


if __name__ == "__main__":
    unittest.main()
