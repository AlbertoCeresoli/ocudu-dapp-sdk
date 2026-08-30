#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Require the Class-A certifier to reject an intentionally invalid DSO."""

from __future__ import annotations

import argparse
import subprocess


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("certifier")
    parser.add_argument("module")
    parser.add_argument("expected_error")
    args = parser.parse_args()

    result = subprocess.run(
        [args.certifier, args.module, "0x00010003", "--shape-set", "compact"],
        check=False,
        text=True,
        capture_output=True,
    )
    if result.returncode == 0 or args.expected_error not in result.stderr:
        raise SystemExit(
            f"expected rejection containing {args.expected_error!r}, got exit {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


if __name__ == "__main__":
    main()
