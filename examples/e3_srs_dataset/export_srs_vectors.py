#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Export captured SRS occasions as SRS channel-estimator vectors for offline replay.

    python3 export_srs_vectors.py srs.h5 --output live.bin
    ocudu-dapp-srs-replay live.bin

Writes the occasions that carry the dApp input into the vector format of the SDK's golden tests
(``include/ocudu_dapp_sdk/srs_channel_estimator_vectors.h``): the module input exactly as the gNB
adapter builds it, paired with what the live cell's estimator reported. ``ocudu-dapp-srs-replay``
runs the reference SRS channel estimator on every occasion and compares it with the cell.

The live telemetry carries the time alignment but not its search resolution or bounds; those
fields are written as NaN and not compared.
"""

from __future__ import annotations

import argparse
import math
import struct
import sys

import numpy as np

from srs_dataset import METADATA, SrsDataset, module_metadata

MAGIC = b"OSRSCEV1"
NAN = math.nan


def encode(occasion) -> bytes:
    metadata = module_metadata(occasion)
    measurements = np.full(8, NAN, dtype=np.float32)
    measurements[0] = occasion.noise_variance
    measurements[1] = occasion.epre_db
    measurements[2] = occasion.rsrp_db
    time_alignment_s = occasion.time_alignment_ns * 1e-9
    measurements[3] = time_alignment_s
    return b"".join((
        struct.pack("<I", METADATA.size), metadata,
        occasion.rx_symbols().astype(np.complex64).tobytes(),
        occasion.reference.astype(np.complex64).tobytes(),
        occasion.channel_matrix().astype(np.complex64).tobytes(),
        measurements.tobytes(),
        struct.pack("<4d", time_alignment_s, NAN, NAN, NAN),
    ))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dataset", help="HDF5 file written by capture_srs_dataset.py")
    parser.add_argument("--output", required=True, help="vector file to write")
    parser.add_argument("--limit", type=int, default=0, help="export at most this many occasions (0: all)")
    args = parser.parse_args()

    cases = []
    skipped = 0
    with SrsDataset(args.dataset) as dataset:
        for occasion in dataset.occasions():
            if occasion.pilots is None:
                skipped += 1
                continue
            cases.append(encode(occasion))
            if args.limit and len(cases) >= args.limit:
                break
    with open(args.output, "wb") as file:
        file.write(MAGIC + struct.pack("<II", 1, len(cases)))
        for case in cases:
            file.write(case)
    print(f"{len(cases)} occasions into {args.output}"
          + (f"; {skipped} without the dApp input skipped" if skipped else ""))
    return 0 if cases else 1


if __name__ == "__main__":
    sys.exit(main())
