#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""CLI for offline demos and portable-E3 Class-C spectrum sensing.

Two modes
---------
* ``--mode offline`` — synthetic grid + PyTorch radiometer (no DU).
* ``--mode live`` — portable E3 consumer via ``ocudu_dapp_class_c_e3``
  (C++ ``portable_e3_consumer``; not a pure-Python APER client).

For **catalog** supervised process packages, use ``spectrum_worker`` /
``srs_isac_worker`` (spawned by ``class_c_python_*d``), not this CLI.

**Placement:** Python is only supported for Class-C **out-of-process** paths
(portable E3 here, or supervised process workers). Not an in-process MODULE.

**Research / education only** — not for production RAN performance.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_ROOT = _HERE.parent
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

try:
    import torch
except ImportError as error:
    print(
        "FAIL: PyTorch is required.\n"
        "  pip install torch\n"
        "  # GPU: pip install torch --index-url https://download.pytorch.org/whl/cu124",
        file=sys.stderr,
    )
    print(f"detail: {error}", file=sys.stderr)
    raise SystemExit(2) from error

from sensing.spectrum_sensing import radiometer, select_device, synthetic_spectrum_grid  # noqa: E402


def run_offline(args: argparse.Namespace) -> int:
    device = select_device(args.device)
    grid = synthetic_spectrum_grid(
        rx=args.rx,
        symbols=args.symbols,
        prbs=args.prbs,
        occupied_prbs=tuple(args.occupied_prbs),
        seed=args.seed,
    )
    result = radiometer(
        grid,
        amplitude_scale=args.amplitude_scale,
        power_threshold=args.threshold,
        device=str(device),
    )
    print(
        json.dumps(
            {
                "mode": "offline",
                "research_only": True,
                "torch": torch.__version__,
                "cuda_available": torch.cuda.is_available(),
                "result": result.as_dict(),
            },
            indent=2,
        )
    )
    return 0


def run_live(args: argparse.Namespace) -> int:
    try:
        import ocudu_dapp_class_c_e3 as e3
    except ImportError as error:
        print(
            "FAIL: native extension ocudu_dapp_class_c_e3 not found.\n"
            "Build SDK with -DOCUDU_DAPP_BUILD_PYTHON_CLASS_C=ON against a host\n"
            "dapp_sdk install whose E3 libs are POSITION_INDEPENDENT_CODE, then:\n"
            "  export PYTHONPATH=/path/to/sdk/build:$PYTHONPATH",
            file=sys.stderr,
        )
        print(f"detail: {error}", file=sys.stderr)
        return 2

    device = select_device(args.device)
    consumer = e3.PortableE3Consumer(
        socket_path=args.socket,
        subscription_hex=args.subscription,
        interface_id=int(e3.INTERFACE_SPECTRUM),
        sample_every_n=args.sample_every,
        timeout_ms=args.timeout_ms,
        cell_id=args.cell,
    )
    consumer.subscribe()
    print(
        json.dumps(
            {
                "mode": "live",
                "research_only": True,
                "transport": "ocudu_dapp_class_c_e3 (C++ portable_e3_consumer)",
                "socket": args.socket,
                "subscription": args.subscription,
                "sample_every": args.sample_every,
                "device": str(device),
                "torch": torch.__version__,
                "cuda_available": torch.cuda.is_available(),
            }
        ),
        flush=True,
    )
    seen = 0
    try:
        deadline = time.monotonic() + args.max_seconds if args.max_seconds > 0 else None
        while args.max_objects <= 0 or seen < args.max_objects:
            if deadline is not None and time.monotonic() >= deadline:
                break
            pub = consumer.poll()
            if pub is None:
                time.sleep(args.poll_sleep)
                continue
            if pub.spectrum is None:
                print(
                    json.dumps(
                        {
                            "warning": "publication without decoded spectrum",
                            "sequence": pub.publication_sequence,
                        }
                    ),
                    flush=True,
                )
                continue
            spec = pub.spectrum
            result = radiometer(
                spec.grid,
                amplitude_scale=spec.amplitude_scale,
                power_threshold=args.threshold,
                device=str(device),
            )
            print(
                json.dumps(
                    {
                        "sequence": spec.publication_sequence,
                        "cell_id": spec.cell_id,
                        "dropped_objects": spec.dropped_objects,
                        "shape": list(spec.shape),
                        "result": result.as_dict(),
                    }
                ),
                flush=True,
            )
            seen += 1
    finally:
        try:
            consumer.unsubscribe()
        except Exception as error:  # pragma: no cover
            print(f"unsubscribe warning: {error}", file=sys.stderr)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("offline", "live"), default="offline")
    parser.add_argument(
        "--device",
        default=None,
        help="Torch device. Default: cuda if available else cpu",
    )
    parser.add_argument("--threshold", type=float, default=1.0)
    parser.add_argument("--amplitude-scale", type=float, default=1.0)
    parser.add_argument("--rx", type=int, default=2)
    parser.add_argument("--symbols", type=int, default=14)
    parser.add_argument("--prbs", type=int, default=25)
    parser.add_argument("--occupied-prbs", type=int, nargs="*", default=[3, 4, 5, 10])
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--socket", default="/run/ocudu/dapp-e3.sock")
    parser.add_argument("--subscription", default="404142434445464748494a4b4c4d4e4f")
    parser.add_argument("--sample-every", type=int, default=10)
    parser.add_argument("--cell", type=int, default=None)
    parser.add_argument("--timeout-ms", type=int, default=1000)
    parser.add_argument("--poll-sleep", type=float, default=0.01)
    parser.add_argument("--max-objects", type=int, default=0)
    parser.add_argument("--max-seconds", type=float, default=0.0)
    args = parser.parse_args()
    return run_offline(args) if args.mode == "offline" else run_live(args)


if __name__ == "__main__":
    raise SystemExit(main())
