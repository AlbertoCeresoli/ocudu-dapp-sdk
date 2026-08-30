#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Export recorded PUSCH bursts for offline replay.

    python3 export_pusch_burst.py capture.h5 --index 0 --output burst0
    python3 export_pusch_burst.py capture.h5 --all --output bursts/
    ocudu-pusch-replay burst0.json --verbose

Reads the ``/pusch.symbols`` group written by ``record_telemetry.py`` and
writes, per burst, ``<name>.json`` (record fields and grant parameters) and
``<name>.cf32`` (equalized data symbols, interleaved little-endian float32
re/im, symbol-major then RE then layer). ``ocudu-pusch-replay`` (ocudu
``apps/tools/pusch_replay``) demaps, descrambles, de-multiplexes UCI,
rate-dematches, LDPC-decodes and CRC-checks the burst with the gNB's own
receiver components and compares the result with the ``crc_ok`` the gNB
reported.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys

import h5py
import numpy as np

GRANT_PREFIX = "grant_"


def scalar(value):
    if isinstance(value, (bytes, np.bytes_)):
        return value.decode()
    if isinstance(value, np.generic):
        value = value.item()
    if isinstance(value, float) and value != value:  # NaN -> null
        return None
    return value


def export(group, index: int, base: str) -> str:
    fields = {}
    grant = {}
    for key in group:
        if key in ("samples", "sample_offset", "sample_count"):
            continue
        value = scalar(group[key][index])
        if key.startswith(GRANT_PREFIX):
            grant[key[len(GRANT_PREFIX):]] = value
        else:
            fields[key] = value
    # Optional grant scalars: the recorder writes NaN / -1 / "" for "absent".
    for optional in ("mcs_index", "mcs_table", "n_rs_id", "dc_position"):
        if optional in grant and (grant[optional] is None or grant[optional] in (-1, "")):
            grant[optional] = None
    for key in ("mcs_index", "mcs_table", "n_rs_id", "dc_position"):
        if grant.get(key) is not None:
            grant[key] = int(grant[key])
    crc = fields.get("crc_ok")
    record = {
        "rnti": int(fields["rnti"]),
        "radio_slot": {"sfn": int(fields["radio_slot_sfn"]), "slot": int(fields["radio_slot_slot"]),
                       "numerology": int(fields["radio_slot_numerology"])},
        "nof_prb": int(fields["nof_prb"]),
        "nof_data_symbols": int(fields["nof_data_symbols"]),
        "nof_layers": int(fields["nof_layers"]),
        "modulation_bits": int(fields["modulation_bits"]),
        "crc_ok": None if crc is None or crc == -1 else bool(crc),
        "symbols_truncated": bool(fields.get("symbols_truncated", False)),
        "sequence": int(fields.get("sequence", 0)),
        "grant": grant,
        "samples_file": os.path.basename(base) + ".cf32",
    }
    # Optional link-quality fields are omitted rather than written as null so
    # readers can rely on "present means a number" (ocudu-pusch-replay uses
    # nlohmann value() defaults, which only apply to absent keys).
    for key in ("sinr_db", "evm", "target_code_rate"):
        try:
            value = float(fields.get(key))
        except (TypeError, ValueError):
            continue
        if math.isfinite(value):
            record[key] = value
    offset = int(group["sample_offset"][index])
    count = int(group["sample_count"][index])
    samples = np.asarray(group["samples"][offset:offset + count], dtype=np.complex64)
    samples.astype("<c8").tofile(base + ".cf32")
    with open(base + ".json", "w", encoding="utf-8") as handle:
        json.dump(record, handle, indent=2)
    return base + ".json"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", help="HDF5 file from record_telemetry.py")
    parser.add_argument("--index", type=int, default=0, help="burst index within /pusch.symbols")
    parser.add_argument("--all", action="store_true", help="export every burst into --output (a directory)")
    parser.add_argument("--only-complete", action="store_true", help="skip bursts flagged symbols_truncated")
    parser.add_argument("--output", default="burst", help="output base name, or directory with --all")
    args = parser.parse_args()

    with h5py.File(args.capture, "r") as handle:
        if "pusch.symbols" not in handle:
            print("capture has no /pusch.symbols group", file=sys.stderr)
            return 1
        group = handle["pusch.symbols"]
        if not any(key.startswith(GRANT_PREFIX) for key in group):
            print("capture predates grant parameters; re-record against a current agent", file=sys.stderr)
            return 1
        count = group["rnti"].shape[0]
        if args.all:
            os.makedirs(args.output, exist_ok=True)
            written = 0
            for index in range(count):
                if args.only_complete and bool(group["symbols_truncated"][index]):
                    continue
                export(group, index, os.path.join(args.output, f"burst{index:05d}"))
                written += 1
            print(f"exported {written} of {count} bursts to {args.output}/")
        else:
            if not 0 <= args.index < count:
                print(f"index {args.index} out of range (0..{count - 1})", file=sys.stderr)
                return 1
            print(export(group, args.index, args.output))
    return 0


if __name__ == "__main__":
    sys.exit(main())
