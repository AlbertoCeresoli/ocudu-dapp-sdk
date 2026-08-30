#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Install the SDK into a clean prefix and build a separate consumer."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile


def run(command: list[str]) -> None:
    subprocess.run(command, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--prefix-path", default="")
    parser.add_argument("--cuda-architectures", default="")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="ocudu-dapp-install-") as temporary:
        root = pathlib.Path(temporary)
        prefix = root / "prefix"
        consumer_build = root / "consumer"
        run([args.cmake, "--install", args.build, "--prefix", str(prefix)])

        prefix_path = str(prefix)
        if args.prefix_path:
            prefix_path = f"{prefix_path};{args.prefix_path}"
        configure = [
            args.cmake,
            "-S",
            args.source,
            "-B",
            str(consumer_build),
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_PREFIX_PATH={prefix_path}",
        ]
        if args.cuda_architectures:
            configure.append(f"-DCMAKE_CUDA_ARCHITECTURES={args.cuda_architectures}")
        run(configure)
        run([args.cmake, "--build", str(consumer_build), "--parallel", "2"])
        run([str(consumer_build / "ocudu_dapp_cpu_consumer")])
        cuda_consumer = consumer_build / "ocudu_dapp_cuda_consumer"
        if cuda_consumer.exists():
            run([str(cuda_consumer)])

        docs = list(prefix.glob("share/doc/**/README.md"))
        if len(docs) < 3 or len({path.relative_to(prefix) for path in docs}) != len(docs):
            raise RuntimeError("installed documentation hierarchy is incomplete or colliding")
        required = [
            prefix / "include/ocudu_dapp_sdk/generated/class-c-publication-v1_generated.h",
            prefix / "lib/cmake/OCUDUDapp/OCUDUDappConfig.cmake",
            prefix / "bin/ocudu-dapp-scaffold-class-a-cuda",
            prefix / "share/doc/ocudu_dapp_sdk/REUSE.toml",
            prefix / "share/doc/ocudu_dapp_sdk/LICENSES/BSD-3-Clause-Clear.txt",
        ]
        missing = [str(path) for path in required if not path.exists()]
        if missing:
            raise RuntimeError(f"incomplete install tree: {missing}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
