#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Follow SRS channel estimates and the gNB metrics report for one or all UEs.

    python3 srs_channel_watch.py --duration 30

Prints, per SRS occasion, the wideband channel coefficient magnitude/phase per
(rx, tx) port pair together with RSRP and timing alignment, and every gNB
metrics report's UE table (RNTI, PUSCH SINR, UL MCS). A sensing dApp would
replace the print statements with its estimator; nothing else changes.
"""

from __future__ import annotations

import argparse
import cmath
import sys
import time

from _common import connect, ocudu_e3

SrsChannelRecord = ocudu_e3.SrsChannelRecord
GnbMetricsJsonRecord = ocudu_e3.GnbMetricsJsonRecord


def ue_rows(report: dict) -> list[str]:
    rows = []
    cells = report.get("cells") or []
    ue_lists = [c.get("ue_list", []) for c in cells if isinstance(c, dict)]
    if not ue_lists and "ue_list" in report:
        ue_lists = [report["ue_list"]]
    for ue_list in ue_lists:
        for entry in ue_list or []:
            ue = entry.get("ue_container", entry) if isinstance(entry, dict) else {}
            if isinstance(ue, dict) and "rnti" in ue:
                rows.append(f"    rnti 0x{int(ue['rnti']):04x} pusch_snr={ue.get('pusch_snr_db')} ul_mcs={ue.get('ul_mcs')} "
                            f"dl_mcs={ue.get('dl_mcs')} cqi={ue.get('cqi')} ul_brate_kbps={ue.get('ul_brate')}")
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=36423)
    parser.add_argument("--rnti", action="append")
    parser.add_argument("--duration", type=float, default=30.0)
    args = parser.parse_args()

    rntis = [int(value, 0) for value in args.rnti] if args.rnti else []
    configs = [ocudu_e3.SubscriptionConfig("srs.channel", rnti_filter=rntis)] if rntis else []
    deadline = time.monotonic() + args.duration
    with connect(args.host, args.port, "sdk-srs-watch") as client:
        client.setup()
        client.subscribe(["srs.channel", "gnb.metrics_json"], configs=configs)
        for indication in client.indications(timeout=1.0, stop=lambda: time.monotonic() >= deadline):
            record = indication.record
            if isinstance(record, SrsChannelRecord):
                coefficients = list(record.samples())
                h = complex(coefficients[0]) if coefficients else 0j
                print(f"srs rnti 0x{record.rnti:04x} sfn {record.radio_slot.sfn}.{record.radio_slot.slot} "
                      f"rx{record.rx_port_index}/tx{record.tx_port_index} |h|={abs(h):.3g} arg={cmath.phase(h):+.2f} rad "
                      f"rsrp={record.rsrp_db} dB ta={record.time_alignment_ns} ns comb={record.comb_size} "
                      f"seq_id={record.sequence_id} dropped={indication.dropped_objects}")
            elif isinstance(record, GnbMetricsJsonRecord):
                report = record.json()
                rows = ue_rows(report)
                print(f"gnb metrics report ({len(record.json_bytes)} B): {len(rows)} UE rows")
                for row in rows:
                    print(row)
        client.release()
    return 0


if __name__ == "__main__":
    sys.exit(main())
