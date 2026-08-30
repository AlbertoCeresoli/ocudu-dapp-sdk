#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Catalog entrypoint: Class-C SRS CIR estimate (supervised process).

Receives UE SRS UL channel estimates and runs a **simple PyTorch CIR** power
profile (early-delay taps). This is **not** a full ISAC / tracking pipeline.

**Placement:** Class-C **out_of_process** only (Python cannot be an in-process
MODULE for Class A/B/C).

**Research / education only** — not for production RAN performance. Prefer
native C++/CUDA packages (``ref_srs_isacd_*``) for product deployments.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parent.parent
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from sensing.process_loop import run_process_worker  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--control-fd", type=int, required=True, help="DU control socket (required)")
    p.add_argument("--interface-id", type=lambda x: int(x, 0), default=0x00010002)
    p.add_argument("--timeout-ms", type=int, default=5000, help="Route admission timeout")
    p.add_argument("--wait-ms", type=int, default=50, help="Poll period (heartbeats serviced here)")
    p.add_argument("--device", default=None, help="Torch device (default: cuda if available else cpu)")
    p.add_argument("--max-publications", type=int, default=0, help="0 = run until control closes")
    p.add_argument(
        "--observer-only",
        action="store_true",
        help="Do not send CIR results to the host (still sense + log)",
    )
    p.add_argument(
        "--hold-lease",
        action="store_true",
        help="Reserved for zero-copy experiments (loop still copies by default)",
    )
    p.add_argument("--json-log", action="store_true", help="Also mirror lifecycle events to stdout")
    p.add_argument("--log-file", default=None, help="Append JSON events (publications go here)")
    args = p.parse_args(argv)
    return run_process_worker(
        role="srs_isac",
        control_fd=args.control_fd,
        interface_id=args.interface_id,
        timeout_ms=args.timeout_ms,
        wait_ms=args.wait_ms,
        device=args.device,
        max_publications=args.max_publications,
        observer_only=args.observer_only,
        hold_lease=args.hold_lease,
        json_log=args.json_log,
        log_path=args.log_file,
    )


if __name__ == "__main__":
    raise SystemExit(main())
