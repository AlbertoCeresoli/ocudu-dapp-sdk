# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""SRS dataset example tests.

Offline, the SDK's golden vectors are published the way the gNB publishes ``srs.channel`` records
(one per port pair, with the pilots and the reference sequence); the capture, dataset and export
path must hand back the module input byte for byte. With ``OCUDU_E3AP_TEST_SERVER`` set, the
capture runs against the OCUDU loopback agent.
"""

from __future__ import annotations

import math
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
from _common import ocudu_e3  # noqa: E402
from srs_dataset import METADATA, DatasetWriter, OccasionAssembler, SrsDataset, module_metadata  # noqa: E402

GOLDEN = HERE.parents[1] / "tests" / "data" / "srs_channel_estimator_vectors.bin"


def read_vectors(path: Path):
    """Minimal reader of the OSRSCEV1 format (include/ocudu_dapp_sdk/srs_channel_estimator_vectors.h)."""
    data = path.read_bytes()
    assert data[:8] == b"OSRSCEV1"
    version, count = struct.unpack_from("<II", data, 8)
    assert version == 1
    offset, cases = 16, []
    for _ in range(count):
        (size,) = struct.unpack_from("<I", data, offset)
        metadata = data[offset + 4 : offset + 4 + size]
        fields = METADATA.unpack(metadata)
        rx, tx, _start, symbols, length, _comb, sets = fields[3:10]
        offset += 4 + size

        def take(count_, dtype, itemsize):
            nonlocal offset
            array = np.frombuffer(data, dtype=dtype, count=count_, offset=offset).copy()
            offset += count_ * itemsize
            return array

        rx_symbols = take(sets * rx * symbols * length, np.complex64, 8).reshape(sets, rx, symbols, length)
        references = take(tx * length, np.complex64, 8).reshape(tx, length)
        channel = take(rx * tx, np.complex64, 8).reshape(rx, tx)
        measurements = take(8, np.float32, 4)
        alignment = take(4, np.float64, 8)
        cases.append({"metadata": metadata, "fields": fields, "rx_symbols": rx_symbols, "references": references,
                      "channel": channel, "measurements": measurements, "time_alignment": alignment})
    assert offset == len(data)
    return cases


def gnb_records(case, sfn: int):
    """The srs.channel records the gNB publishes for one occasion."""
    f = case["fields"]
    rx_ports, tx_ports, start, symbols, length, comb, sets, n_cs_max = f[3:11]
    pilot_set, cyclic_shift, first_subcarrier = f[11:15], f[15:19], f[19:21]
    group, number, scs_hz, numerology, _grid, _flags, rnti = f[21:28]
    noise, epre, rsrp = (float(v) for v in case["measurements"][:3])
    records = []
    for tx in range(tx_ports):
        pilots = case["rx_symbols"][pilot_set[tx]]
        for rx in range(rx_ports):
            estimate = pilots[rx].mean(axis=0) * np.conj(case["references"][tx])
            records.append(ocudu_e3.SrsChannelRecord(
                rnti=rnti, radio_slot=ocudu_e3.RadioSlot(sfn, 3, numerology), nof_subcarriers=length,
                comb_size=comb, nof_rx_ports=rx_ports, nof_tx_ports=tx_ports, rx_port_index=rx, tx_port_index=tx,
                start_symbol=start, nof_symbols=symbols, rsrp_db=rsrp, epre_db=epre, noise_variance=noise,
                time_alignment_ns=float(case["time_alignment"][0]) * 1e9,
                mapping_initial_subcarrier=first_subcarrier[pilot_set[tx]], comb_offset=0, cyclic_shift=0,
                sequence_id=0, sequence_group=group, sequence_number=number, cyclic_shift_index=cyclic_shift[tx],
                cyclic_shift_maximum=n_cs_max, bandwidth_index=0, frequency_position=0, frequency_shift=0,
                frequency_hopping=0, coefficients=estimate.astype(np.complex64).tobytes(),
                received_pilots=pilots[rx].astype(np.complex64).tobytes(),
                reference_symbols=case["references"][tx].astype(np.complex64).tobytes()))
    return records


class DatasetTests(unittest.TestCase):
    def test_the_dataset_hands_back_the_exact_dapp_input_of_every_golden_occasion(self) -> None:
        cases = read_vectors(GOLDEN)
        assembler = OccasionAssembler()
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "golden.h5")
            writer = DatasetWriter(path)
            for index, case in enumerate(cases):
                records = gnb_records(case, sfn=index)
                occasions = [assembler.add(record, monotonic_ns=index) for record in reversed(records)]
                self.assertTrue(all(o is None for o in occasions[:-1]))
                self.assertIsNotNone(occasions[-1])
                writer.add(occasions[-1])
            writer.close()
            self.assertEqual(assembler.incomplete, 0)

            vectors = os.path.join(directory, "golden.bin")
            result = subprocess.run([sys.executable, str(HERE / "export_srs_vectors.py"), path, "--output", vectors],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            exported = read_vectors(Path(vectors))
            self.assertEqual(len(exported), len(cases))
            # Occasions of one shape come back together, so match them by their SFN-stamped RNTI and metadata.
            by_key = {(c["fields"][27], c["rx_symbols"].tobytes()): c for c in cases}
            for vector in exported:
                golden = by_key[(vector["fields"][27], vector["rx_symbols"].tobytes())]
                # Everything the estimator reads is identical; the carrier width is only a bound.
                self.assertEqual(vector["fields"][:25], golden["fields"][:25])
                self.assertEqual(vector["fields"][26:29], golden["fields"][26:29])
                self.assertAlmostEqual(vector["fields"][29], golden["fields"][29], delta=1e-18)
                self.assertTrue(np.array_equal(vector["references"], golden["references"]))
                self.assertTrue(np.allclose(vector["measurements"][:3], golden["measurements"][:3]))

    def test_the_reader_restores_occasions_and_their_labels(self) -> None:
        case = next(c for c in read_vectors(GOLDEN) if c["fields"][9] == 2)  # interleaved pilots
        occasion = None
        assembler = OccasionAssembler()
        for record in gnb_records(case, sfn=7):
            occasion = assembler.add(record) or occasion
        occasion.kpi = {"pusch_snr_db": 21.5, "ul_mcs": 27.0}
        self.assertTrue(occasion.interleaved)
        self.assertEqual(module_metadata(occasion)[:100], case["metadata"][:100])
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "one.h5")
            writer = DatasetWriter(path)
            writer.add(occasion)
            writer.close()
            with SrsDataset(path) as dataset:
                self.assertEqual(len(dataset), 1)
                restored = next(dataset.occasions())
                self.assertEqual(dataset.shapes, [occasion.shape])
        self.assertTrue(np.array_equal(restored.rx_symbols(), case["rx_symbols"]))
        self.assertTrue(np.array_equal(restored.reference, case["references"]))
        self.assertEqual(restored.first_subcarrier, occasion.first_subcarrier)
        self.assertEqual(restored.kpi["pusch_snr_db"], 21.5)
        self.assertTrue(math.isnan(restored.kpi["cqi"]))
        self.assertTrue(np.allclose(restored.channel_matrix(), occasion.channel_matrix()))

    def test_lossy_streams_count_incomplete_occasions_instead_of_writing_them(self) -> None:
        case = read_vectors(GOLDEN)[6]  # two rx ports, two tx ports
        assembler = OccasionAssembler(max_pending=2)
        for sfn in range(4):
            records = gnb_records(case, sfn=sfn)
            for record in records[:-1]:  # the last port pair of every occasion is lost
                self.assertIsNone(assembler.add(record))
        self.assertEqual(assembler.incomplete, 2)
        with self.assertRaises(ValueError):
            OccasionAssembler(max_pending=0)

    def test_scripts_parse_help(self) -> None:
        for script, option in (("capture_srs_dataset.py", "--interval-ms"), ("export_srs_vectors.py", "--output")):
            result = subprocess.run([sys.executable, str(HERE / script), "--help"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(option, result.stdout)


@unittest.skipUnless(os.environ.get("OCUDU_E3AP_TEST_SERVER"), "OCUDU_E3AP_TEST_SERVER not set")
class LiveTests(unittest.TestCase):
    def test_captures_occasions_from_the_loopback_agent(self) -> None:
        with tempfile.TemporaryDirectory() as catalog:
            agent = subprocess.Popen([os.environ["OCUDU_E3AP_TEST_SERVER"], "--catalog", catalog, "--e3dp", "0"],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                port = 0
                for line in iter(agent.stdout.readline, ""):
                    if line.startswith("e3ap_port"):
                        port = int(line.split()[1])
                    if line.startswith("ready"):
                        break
                self.assertNotEqual(port, 0)
                output = os.path.join(catalog, "loopback.h5")
                run = subprocess.run([sys.executable, str(HERE / "capture_srs_dataset.py"), "--port", str(port),
                                      "--duration", "3", "--output", output], capture_output=True, text=True,
                                     timeout=60)
                self.assertEqual(run.returncode, 0, run.stderr)
                with SrsDataset(output) as dataset:
                    self.assertGreater(len(dataset), 0)
                    occasion = next(dataset.occasions())
                    # The loopback agent publishes one-pair estimates without the dApp input.
                    self.assertEqual((occasion.nof_rx_ports, occasion.nof_tx_ports), (1, 1))
                    self.assertIsNone(occasion.pilots)
                    self.assertAlmostEqual(occasion.rsrp_db, -70.0)
            finally:
                try:
                    agent.stdin.write("quit\n")
                    agent.stdin.flush()
                    agent.wait(timeout=5)
                except Exception:
                    agent.kill()
                for stream in (agent.stdin, agent.stdout, agent.stderr):
                    stream.close()


if __name__ == "__main__":
    unittest.main()
