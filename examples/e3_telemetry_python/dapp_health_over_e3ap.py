#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Query dApp inventory, health, and per-instance metrics over E3AP.

    python3 dapp_health_over_e3ap.py
    python3 dapp_health_over_e3ap.py --watch 2

Management requests (E3SM-OCUDU-DAPP-MGMT-V1) travel as dApp control actions
on RAN function 11; the agent acknowledges and returns the ManagementResponse
in a control response. No root-owned socket is involved, so this can run from
an ordinary user account or another host that reaches the E3AP port.
"""

from __future__ import annotations

import argparse
import sys
import time

from _common import connect


def scalar(value):
    """normalize() renders TypedValue as {"<kind>Value": x}; 64-bit counters arrive as big-endian "hex:" octets."""
    if isinstance(value, dict) and len(value) == 1:
        value = next(iter(value.values()))
    if isinstance(value, str) and value.startswith("hex:"):
        return int(value[4:], 16)
    return value


def show(client) -> None:
    inventory = client.inventory()
    payload = inventory.get("payload", {}).get("inventory", {})
    instances = payload.get("instances", [])
    print(f"inventory generation {payload.get('generation')}: {len(payload.get('packages', []))} packages, {len(instances)} instances")
    for instance in instances:
        print(f"  {instance['instanceId']}  {instance['globalName']:<40} {instance['state']:<10} {instance['backend']}/{instance['placement']}")
    health = client.health().get("payload", {}).get("health", {})
    keys = ("total", "deadlineMisses", "fallbacks", "breakerOpened", "deviceErrors", "sourceRingDrops")
    print("health: " + ", ".join(f"{key}={health.get(key)}" for key in keys))
    for instance in instances:
        metrics = client.metrics(instance["instanceId"]).get("payload", {}).get("metrics", {})
        samples = metrics.get("samples", [])
        interesting = {s["name"]: scalar(s["value"])
                       for s in samples if s["name"].endswith(("invoke.count", "fallback.count", "breaker.open", "deadline_misses"))}
        print(f"  metrics {instance['globalName']}: {interesting}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=36423)
    parser.add_argument("--watch", type=float, default=0.0, help="repeat every N seconds")
    args = parser.parse_args()

    with connect(args.host, args.port, "sdk-health") as client:
        setup = client.setup()
        if setup.management is None:
            print("this agent does not expose the management RAN function", file=sys.stderr)
            return 1
        while True:
            show(client)
            if args.watch <= 0:
                break
            time.sleep(args.watch)
    return 0


if __name__ == "__main__":
    sys.exit(main())
