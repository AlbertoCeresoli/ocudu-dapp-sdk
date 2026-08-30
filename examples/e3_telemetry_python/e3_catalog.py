#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Smallest possible E3AP client: run E3 Setup and print what the gNB offers.

    python3 e3_catalog.py --host 127.0.0.1 --port 36423

The agent answers with its RAN functions. Function 10 lists the telemetry
streams you can subscribe to; function 11 accepts dApp management requests.
"""

from __future__ import annotations

import argparse
import sys

from _common import connect


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=36423)
    args = parser.parse_args()

    with connect(args.host, args.port, "sdk-e3-catalog") as client:
        setup = client.setup()
        print(f"{setup.ran_identifier}: E3AP {setup.protocol_version}, we are dApp {setup.dapp_id}")
        print(f"payload bound {setup.maximum_payload_bytes} B, fragment bound {setup.maximum_fragment_bytes} B")
        for function in setup.ran_functions.values():
            print(f"RAN function {function.ran_function_id} {function.name} {function.version}")
            for stream in function.streams:
                floor = f" (default floor {stream.minimum_interval_us / 1000:.0f} ms" if stream.minimum_interval_us else " (full rate by default"
                floor += f", hard floor {stream.hard_floor_interval_us / 1000:.0f} ms)" if stream.hard_floor_interval_us else ", no hard floor)"
                print(f"  telemetry {stream.telemetry_id}: {stream.name}{floor}")
                print(f"      {stream.description}")
            if function.control_ids:
                print(f"  control identifiers: {function.control_ids} (schema {function.schema_module})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
