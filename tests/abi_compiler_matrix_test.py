#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import argparse
import pathlib
import subprocess
import tempfile


def run(command: list[str]) -> str:
    return subprocess.run(command, check=True, text=True, capture_output=True).stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=pathlib.Path)
    parser.add_argument("--expected", required=True, type=pathlib.Path)
    parser.add_argument("--include", required=True, type=pathlib.Path)
    parser.add_argument("--gcc", required=True)
    parser.add_argument("--gxx", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--clangxx", required=True)
    args = parser.parse_args()

    expected = "".join(
        line for line in args.expected.read_text(encoding="utf-8").splitlines(keepends=True) if not line.startswith("#")
    )
    variants = (
        ("gcc-c11", args.gcc, "c", "c11"),
        ("clang-c11", args.clang, "c", "c11"),
        ("gcc-cxx17", args.gxx, "c++", "c++17"),
        ("clang-cxx17", args.clangxx, "c++", "c++17"),
    )
    with tempfile.TemporaryDirectory(prefix="ocudu-dapp-abi-") as temporary:
        root = pathlib.Path(temporary)
        for name, compiler, language, standard in variants:
            executable = root / name
            run(
                [
                    compiler,
                    "-x",
                    language,
                    f"-std={standard}",
                    "-Wall",
                    "-Wextra",
                    "-Wpedantic",
                    "-Werror",
                    f"-I{args.include}",
                    str(args.source),
                    "-o",
                    str(executable),
                ]
            )
            actual = run([str(executable)])
            if actual != expected:
                raise RuntimeError(f"{name} public ABI fingerprint differs from {args.expected}")
    print("public ABI v1 fingerprint matches for GCC/Clang C11/C++17")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
