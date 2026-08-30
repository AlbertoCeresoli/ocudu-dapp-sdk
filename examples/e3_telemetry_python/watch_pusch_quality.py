#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Subscribe to PUSCH link quality and CRC outcomes and keep a per-UE summary.

    python3 watch_pusch_quality.py --duration 20
    python3 watch_pusch_quality.py --rnti 0x4601 --iq-preview

Shows how an observer dApp combines two telemetry streams: `pusch.symbols`
(SINR, EVM, equalized I/Q per grant) and `pusch.crc` (per-transport-block
outcome) into a rolling BLER and a constellation preview. The RNTI filter is
applied by the gNB before the data leaves the store, so the association only
carries the UE you asked for.
"""

from __future__ import annotations

import argparse
import collections
import sys
import time

from _common import connect, ocudu_e3

PuschSymbolsRecord = ocudu_e3.PuschSymbolsRecord
PuschCrcRecord = ocudu_e3.PuschCrcRecord


class UeSummary:
    def __init__(self) -> None:
        self.crc = collections.deque(maxlen=200)
        self.sinr = collections.deque(maxlen=50)
        self.last_mod = 0
        self.last_prb = 0
        self.grants = 0

    def bler(self) -> float | None:
        return None if not self.crc else 1.0 - sum(self.crc) / len(self.crc)

    def mean_sinr(self) -> float | None:
        return None if not self.sinr else sum(self.sinr) / len(self.sinr)


def preview(record: PuschSymbolsRecord, cells: int = 9) -> str:
    """Tiny ASCII constellation for a quick sanity check of the I/Q payload."""
    samples = list(record.samples())[:512]
    if not samples:
        return ""
    peak = max(max(abs(s.real), abs(s.imag)) for s in samples) or 1.0
    grid = [[" "] * cells for _ in range(cells)]
    for s in samples:
        x = min(cells - 1, int((s.real / peak + 1) / 2 * (cells - 1) + 0.5))
        y = min(cells - 1, int((1 - (s.imag / peak + 1) / 2) * (cells - 1) + 0.5))
        grid[y][x] = "*"
    return "\n".join("    |" + "".join(row) + "|" for row in grid)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=36423)
    parser.add_argument("--rnti", action="append", help="only this RNTI (repeatable, e.g. 0x4601)")
    parser.add_argument("--periodicity-ms", type=int, default=100, help="per-UE publication floor for symbols")
    parser.add_argument("--duration", type=float, default=15.0)
    parser.add_argument("--iq-preview", action="store_true", help="print an ASCII constellation per symbol record")
    args = parser.parse_args()

    rntis = [int(value, 0) for value in args.rnti] if args.rnti else []
    configs = [ocudu_e3.SubscriptionConfig("pusch.symbols", rnti_filter=rntis, minimum_interval_ms=args.periodicity_ms)]
    if rntis:
        configs.append(ocudu_e3.SubscriptionConfig("pusch.crc", rnti_filter=rntis))

    summaries: dict[int, UeSummary] = collections.defaultdict(UeSummary)
    deadline = time.monotonic() + args.duration
    with connect(args.host, args.port, "sdk-pusch-watch") as client:
        client.setup()
        subscription = client.subscribe(["pusch.symbols", "pusch.crc"], configs=configs)
        print(f"subscription {subscription.subscription_id}; watching for {args.duration:.0f} s", file=sys.stderr)
        last_report = time.monotonic()
        for indication in client.indications(timeout=1.0, stop=lambda: time.monotonic() >= deadline):
            record = indication.record
            if isinstance(record, PuschCrcRecord):
                summaries[record.rnti].crc.append(1 if record.crc_ok else 0)
            elif isinstance(record, PuschSymbolsRecord):
                summary = summaries[record.rnti]
                summary.grants += 1
                summary.last_mod = record.modulation_bits
                summary.last_prb = record.nof_prb
                if record.sinr_db is not None:
                    summary.sinr.append(record.sinr_db)
                if args.iq_preview:
                    print(f"rnti 0x{record.rnti:04x} sfn {record.radio_slot.sfn}.{record.radio_slot.slot} "
                          f"Qm={record.modulation_bits} prb={record.nof_prb} dropped={indication.dropped_objects}")
                    print(preview(record))
            if time.monotonic() - last_report >= 1.0:
                last_report = time.monotonic()
                report(summaries)
        # Idle UEs produce few grants; always close with a final summary.
        report(summaries, final=True)
        client.release()
    return 0


def report(summaries, final: bool = False) -> None:
    if not summaries:
        if final:
            print("no PUSCH activity observed (no scheduled uplink grants for the selected RNTIs)")
        return
    for rnti, summary in sorted(summaries.items()):
        bler = summary.bler()
        sinr = summary.mean_sinr()
        print(f"{'final ' if final else ''}rnti 0x{rnti:04x}: grants={summary.grants:5d} Qm={summary.last_mod} prb={summary.last_prb:3d} "
              f"sinr={'n/a' if sinr is None else f'{sinr:5.1f} dB'} "
              f"bler={'n/a' if bler is None else f'{bler:6.3f}'} ({len(summary.crc)} TBs)")


if __name__ == "__main__":
    sys.exit(main())
