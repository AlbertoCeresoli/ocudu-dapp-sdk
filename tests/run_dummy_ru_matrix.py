#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Run dummy-RU package matrix for reference packages in this SDK repo only.

Host tools come from OCUDU_* env vars. Does not pull commercial/deepsig catalogs.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


def env_path(name: str, *fallback: Path) -> Path | None:
    value = os.environ.get(name)
    if value:
        path = Path(value)
        return path if path.exists() else None
    for candidate in fallback:
        if candidate is not None and Path(candidate).exists():
            return Path(candidate)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path(os.environ.get("OCUDU_DAPP_SDK_BUILD", Path(__file__).resolve().parents[1] / "build-cuda")),
    )
    parser.add_argument("--report", type=Path, default=None)
    parser.add_argument("--include-failures", action="store_true")
    parser.add_argument("--include-test-fixtures", action="store_true")
    args = parser.parse_args()

    sdk_root = Path(__file__).resolve().parents[1]
    host_root = Path(os.environ.get("OCUDU_HOST_SOURCE_DIR", sdk_root.parent / "ocudu"))

    matrix = env_path(
        "OCUDU_DAPP_MATRIX",
        host_root / "tests/integrationtests/dapp/run_all_package_dummy_ru.py",
    )
    if matrix is None or not matrix.is_file():
        print("OCUDU_DAPP_MATRIX not found (run_all_package_dummy_ru.py)", file=sys.stderr)
        return 2

    odu = env_path(
        "OCUDU_ODU",
        host_root / "build-gpu-release/apps/du/odu",
    )
    odu_cpu = env_path(
        "OCUDU_ODU_CPU",
        host_root / "build/apps/du/odu",
        odu,
    )
    preflight = env_path(
        "OCUDU_DAPP_PREFLIGHT",
        host_root / "build-gpu-release/apps/tools/dapp_preflight/ocudu-dapp-preflight",
    )
    preflight_cpu = env_path(
        "OCUDU_DAPP_PREFLIGHT_CPU",
        host_root / "build/apps/tools/dapp_preflight/ocudu-dapp-preflight",
        preflight,
    )
    client = env_path(
        "OCUDU_DAPPCTL_PY",
        host_root / "apps/tools/dappctl/ocudu_dappctl.py",
    )
    schema = env_path(
        "OCUDU_DAPP_E3_SCHEMA",
        host_root / "schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1",
    )
    for label, path in (
        ("OCUDU_ODU", odu),
        ("OCUDU_DAPP_PREFLIGHT", preflight),
        ("OCUDU_DAPPCTL_PY", client),
        ("OCUDU_DAPP_E3_SCHEMA", schema),
    ):
        if path is None or not path.is_file():
            print(f"missing host tool {label}", file=sys.stderr)
            return 2
    if not args.build_dir.is_dir():
        print(f"build-dir missing: {args.build_dir}", file=sys.stderr)
        return 2

    report = args.report or (args.build_dir / "reports" / "dummy-ru-matrix.json")
    report.parent.mkdir(parents=True, exist_ok=True)

    cmd = [
        sys.executable,
        str(matrix),
        "--catalog",
        str(args.build_dir.resolve()),
        "--odu",
        str(odu.resolve()),
        "--odu-cpu",
        str((odu_cpu or odu).resolve()),
        "--preflight-helper",
        str(preflight.resolve()),
        "--preflight-helper-cpu",
        str((preflight_cpu or preflight).resolve()),
        "--client",
        str(client.resolve()),
        "--schema",
        str(schema.resolve()),
        "--report",
        str(report.resolve()),
        "--time-scaling",
        "2",
        "--settle-seconds",
        "3",
        "--metric-timeout",
        "45",
    ]
    if args.include_failures:
        cmd.append("--include-failures")
    if args.include_test_fixtures:
        cmd.append("--include-test-fixtures")

    print("==> sdk dummy-ru matrix:", " ".join(cmd), flush=True)
    return subprocess.run(cmd, env=os.environ.copy()).returncode


if __name__ == "__main__":
    raise SystemExit(main())
