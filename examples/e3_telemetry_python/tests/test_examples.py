# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Example smoke tests: argument parsing offline, and a live round trip against
the OCUDU loopback E3 agent when ``OCUDU_E3AP_TEST_SERVER`` is set."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
SCRIPTS = ("e3_catalog.py", "watch_pusch_quality.py", "srs_channel_watch.py", "dapp_health_over_e3ap.py", "record_telemetry.py")
OFFLINE_SCRIPTS = ("export_pusch_burst.py",)


class HelpTests(unittest.TestCase):
    def test_every_script_parses_help(self) -> None:
        for script in SCRIPTS:
            result = subprocess.run([sys.executable, str(HERE / script), "--help"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("--port", result.stdout)
        for script in OFFLINE_SCRIPTS:
            result = subprocess.run([sys.executable, str(HERE / script), "--help"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("--output", result.stdout)


@unittest.skipUnless(os.environ.get("OCUDU_E3AP_TEST_SERVER"), "OCUDU_E3AP_TEST_SERVER not set")
class LiveTests(unittest.TestCase):
    def test_catalog_and_health_against_loopback_agent(self) -> None:
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
                catalog_run = subprocess.run(
                    [sys.executable, str(HERE / "e3_catalog.py"), "--port", str(port)],
                    capture_output=True, text=True, timeout=30)
                self.assertEqual(catalog_run.returncode, 0, catalog_run.stderr)
                self.assertIn("pusch.symbols", catalog_run.stdout)
                health_run = subprocess.run(
                    [sys.executable, str(HERE / "dapp_health_over_e3ap.py"), "--port", str(port)],
                    capture_output=True, text=True, timeout=30)
                self.assertEqual(health_run.returncode, 0, health_run.stderr)
                self.assertIn("health:", health_run.stdout)
                watch_run = subprocess.run(
                    [sys.executable, str(HERE / "watch_pusch_quality.py"), "--port", str(port), "--duration", "2"],
                    capture_output=True, text=True, timeout=30)
                self.assertEqual(watch_run.returncode, 0, watch_run.stderr)
                self.assertIn("rnti 0x46", watch_run.stdout)
            finally:
                try:
                    agent.stdin.write("quit\n")
                    agent.stdin.flush()
                    agent.wait(timeout=5)
                except Exception:
                    agent.kill()
                time.sleep(0.1)


class OfflineRoundTripTests(unittest.TestCase):
    """flatten() -> Hdf5Writer -> export_pusch_burst.export() without a gNB."""

    def test_pusch_symbols_hdf5_round_trip(self) -> None:
        try:
            import h5py  # noqa: F401
            import numpy as np
        except ImportError:
            self.skipTest("h5py/numpy not available")
        sys.path.insert(0, str(HERE))
        try:
            import export_pusch_burst
            import record_telemetry
        finally:
            sys.path.pop(0)

        class Obj:
            def __init__(self, **kw):
                self.__dict__.update(kw)

        grant = Obj(harq_id=1, rv=0, new_data=True, tbs_bytes=277, ldpc_base_graph=2,
                    tbs_lbrm_bytes=159749, mcs_index=17, mcs_table=0, n_id=1, n_rs_id=None,
                    transform_precoding=False, cyclic_prefix=0, start_symbol=0, nof_symbols=14,
                    dmrs_symbol_mask=0x2004, dmrs_type=1, nof_cdm_groups_without_data=2,
                    dmrs_scrambling_id=1, n_scid=False, bwp_start_rb=0, bwp_size_rb=51,
                    prb_mask=bytes([0xff, 0x07] + [0] * 33), nof_layers=1, nof_rx_ports=1,
                    dc_position=306, nof_harq_ack_bits=0, nof_csi_part1_bits=0,
                    csi_part2_expected=False, alpha_scaling=1.0, beta_offset_harq_ack=0,
                    beta_offset_csi_part1=0, beta_offset_csi_part2=0)
        samples = [complex(i % 3 - 1, (i % 5 - 2) / 2.0) for i in range(11 * 12 * 12)]
        record = Obj(rnti=0x4601, radio_slot=Obj(sfn=100, slot=4, numerology=1), nof_prb=11,
                     nof_data_symbols=12, nof_layers=1, modulation_bits=4, sinr_db=None,
                     evm=float("nan"), target_code_rate=0.55, crc_ok=1, symbols_truncated=False,
                     grant_valid=True, grant=grant, samples=lambda: samples,
                     equalized_symbols=b"")
        indication = Obj(stream="pusch.symbols", record=record, cell_id=0, sequence=7,
                         monotonic_ns=123456789, dropped_objects=0)
        stream = Obj(name="pusch.symbols", telemetry_id=1, interface_id=0x20001,
                     description="test", minimum_interval_us=0, hard_floor_interval_us=0)
        setup = Obj(ran_identifier="test-cell", protocol_version="1.0.0", dapp_id=1,
                    ran_functions={10: Obj(streams=[stream])})
        args = Obj(interval_ms=0, drop_policy="dropOldest", rnti=[], streams=["pusch.symbols"])

        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "cap.h5")
            writer = record_telemetry.Hdf5Writer(path, setup, args)
            writer.add(indication)
            writer.close()
            import h5py
            with h5py.File(path) as fh:
                group = fh["pusch.symbols"]
                self.assertEqual(int(group["rnti"][0]), 0x4601)
                self.assertEqual(int(group["grant_tbs_bytes"][0]), 277)
                self.assertEqual(int(group["sample_count"][0]), len(samples))
                base = os.path.join(tmp, "burst")
                export_pusch_burst.export(group, 0, base)
            with open(base + ".json") as fh:
                exported = json.load(fh)
            data = np.fromfile(base + ".cf32", dtype=np.complex64)
        self.assertEqual(exported["rnti"], 0x4601)
        self.assertEqual(exported["grant"]["tbs_bytes"], 277)
        self.assertEqual(exported["grant"]["mcs_index"], 17)
        self.assertEqual(exported["grant"]["prb_mask"], grant.prb_mask.hex())
        self.assertTrue(exported["crc_ok"])
        # Optional link-quality fields must be numbers when present, never null.
        for key in ("sinr_db", "evm", "target_code_rate"):
            if key in exported:
                self.assertIsInstance(exported[key], float)
        self.assertNotIn("sinr_db", exported)  # was None in the record
        self.assertNotIn("evm", exported)      # was NaN in the record
        self.assertAlmostEqual(exported["target_code_rate"], 0.55, places=5)
        self.assertEqual(data.size, len(samples))


if __name__ == "__main__":
    unittest.main(verbosity=2)
