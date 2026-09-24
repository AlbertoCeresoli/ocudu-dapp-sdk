#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Capture an SRS channel-estimation dataset from a live cell over E3.

    python3 capture_srs_dataset.py --duration 60 --output srs.h5
    python3 capture_srs_dataset.py --rnti 0x4601 --occasions 5000 --interval-ms 0 --output ue.h5

Subscribes to ``srs.channel`` (one record per receive/transmit port pair of every SRS occasion)
and to the gNB metrics report, assembles the port pairs of each occasion and writes the occasions
into an HDF5 dataset (see ``srs_dataset.py``): per occasion shape, the least-squares estimates, the
received pilots and reference sequences that form the SRS channel-estimator dApp input, the gNB's
channel matrix and measurements as labels, and the UE's PUSCH SINR, UL MCS and CQI at that moment.

``--interval-ms 0`` asks for every occasion; the gNB enforces its advertised hard floor. The
stream is lossy: occasions with a missing port pair are counted, not written.
"""

from __future__ import annotations

import argparse
import sys
import time
from typing import Dict

from _common import connect, ocudu_e3
from srs_dataset import DatasetWriter, OccasionAssembler

SrsChannelRecord = ocudu_e3.SrsChannelRecord
GnbMetricsJsonRecord = ocudu_e3.GnbMetricsJsonRecord


def ue_kpis(report: dict) -> Dict[int, Dict[str, float]]:
    """Per-RNTI PUSCH SINR, UL MCS and CQI from one gNB metrics report."""
    result: Dict[int, Dict[str, float]] = {}
    cells = report.get("cells") or []
    ue_lists = [cell.get("ue_list", []) for cell in cells if isinstance(cell, dict)]
    if not ue_lists and "ue_list" in report:
        ue_lists = [report["ue_list"]]
    for ue_list in ue_lists:
        for entry in ue_list or []:
            ue = entry.get("ue_container", entry) if isinstance(entry, dict) else {}
            if not isinstance(ue, dict) or "rnti" not in ue:
                continue
            values = {}
            for name, key in (("pusch_snr_db", "pusch_snr_db"), ("ul_mcs", "ul_mcs"), ("cqi", "cqi")):
                value = ue.get(key)
                if isinstance(value, (int, float)):
                    values[name] = float(value)
            result[int(ue["rnti"])] = values
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=36423)
    parser.add_argument("--rnti", action="append", default=[], help="capture only these UEs (repeatable)")
    parser.add_argument("--duration", type=float, default=60.0, help="seconds to capture")
    parser.add_argument("--occasions", type=int, default=0, help="stop after this many occasions (0: no limit)")
    parser.add_argument("--interval-ms", type=int, default=0,
                        help="per-UE capture interval requested from the gNB (0: every occasion)")
    parser.add_argument("--no-kpi", action="store_true", help="do not subscribe to the gNB metrics report")
    parser.add_argument("--output", required=True, help="HDF5 file to write")
    args = parser.parse_args()

    rntis = [int(value, 0) for value in args.rnti]
    configs = [ocudu_e3.SubscriptionConfig("srs.channel", minimum_interval_ms=args.interval_ms, rnti_filter=rntis)]
    streams = ["srs.channel"] + ([] if args.no_kpi else ["gnb.metrics_json"])
    assembler = OccasionAssembler()
    kpis: Dict[int, Dict[str, float]] = {}
    written = records = dropped = 0
    deadline = time.monotonic() + args.duration

    with connect(args.host, args.port, "sdk-srs-dataset") as client:
        setup = client.setup()
        writer = DatasetWriter(args.output, {
            "ran_identifier": setup.ran_identifier, "e3ap_version": setup.protocol_version,
            "requested_interval_ms": args.interval_ms, "rnti_filter": ",".join(hex(r) for r in rntis)})
        try:
            client.subscribe(streams, configs=configs)
            done = lambda: time.monotonic() >= deadline or (args.occasions and written >= args.occasions)  # noqa
            for indication in client.indications(timeout=1.0, stop=done):
                dropped += indication.dropped_objects
                record = indication.record
                if isinstance(record, GnbMetricsJsonRecord):
                    try:
                        kpis.update(ue_kpis(record.json()))
                    except ValueError:
                        pass
                    continue
                if not isinstance(record, SrsChannelRecord):
                    continue
                records += 1
                occasion = assembler.add(record, indication.monotonic_ns)
                if occasion is None:
                    continue
                occasion.kpi = dict(kpis.get(occasion.rnti, {}))
                writer.add(occasion)
                written += 1
            client.release()
        finally:
            writer.close({"records": records, "occasions": written, "incomplete": assembler.incomplete,
                          "inconsistent": assembler.inconsistent, "dropped_objects": dropped})

    print(f"{written} occasions ({writer.with_pilots} with the dApp input) from {records} records into "
          f"{args.output}; incomplete {assembler.incomplete}, inconsistent {assembler.inconsistent}, "
          f"dropped by the gNB {dropped}")
    for shape, count in sorted(writer.counts.items()):
        print(f"  {shape}: {count}")
    return 0 if written else 1


if __name__ == "__main__":
    sys.exit(main())
