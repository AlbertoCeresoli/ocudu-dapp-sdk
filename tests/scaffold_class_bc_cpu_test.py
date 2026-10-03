#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Generate, build, and contract-test fresh Class-B/C CPU projects."""

from __future__ import annotations

import argparse
import importlib.util
import json
import shutil
import stat
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace


# kind, target, global name, package id, interface id, contract class, mask, capabilities
CASES = (
    ("scheduler", "scaffold_scheduler_cpu", "com.deepsig.scaffold.scheduler.cpu",
     "00000000-0000-4000-8000-000000000106", "0x00010006", "2", "1", "0"),
    ("spectrum", "scaffold_spectrum_cpu", "com.deepsig.scaffold.spectrum.cpu",
     "00000000-0000-4000-8000-000000000101", "0x00010001", "3", "3", "7"),
    ("srs-isac", "scaffold_srs_isac_cpu", "com.deepsig.scaffold.srs-isac.cpu",
     "00000000-0000-4000-8000-000000000102", "0x00010002", "3", "3", "0"),
)

FOLDERS = {"scheduler": "scheduler", "spectrum": "spectrum", "srs-isac": "srs_isac"}


def run(command, expect_success=True, capture=False):
    result = subprocess.run(command, capture_output=capture, text=True)
    if expect_success and result.returncode != 0:
        raise AssertionError(f"command failed ({result.returncode}): {command}")
    if not expect_success and result.returncode == 0:
        raise AssertionError(f"command unexpectedly succeeded: {command}")
    return result


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--generation-only", action="store_true")
    parser.add_argument("--cmake")
    parser.add_argument("--sdk-dir")
    parser.add_argument("--contract")
    args = parser.parse_args()
    if not args.generation_only and not all((args.cmake, args.sdk_dir, args.contract)):
        parser.error("full scaffold tests require --cmake, --sdk-dir, and --contract")
    return args


def validate_generation(project: Path, kind: str, target: str) -> None:
    import re

    folder = FOLDERS[kind]
    cmake = (project / "CMakeLists.txt").read_text()
    assert "find_package(OCUDUDAppSDK 1 CONFIG REQUIRED)" in cmake
    assert "LANGUAGES CXX)" in cmake and "CUDA" not in cmake
    assert f"{target}_package" in cmake
    assert not re.search(r"@[A-Z_]+@", cmake), "unresolved template token in CMakeLists"
    readme = (project / "README.md").read_text()
    assert "module_contract_test" in readme
    assert not re.search(r"@[A-Z_]+@", readme), "unresolved template token in README"
    assert (project / "reference" / folder / "module.cpp").is_file()
    assert (project / "reference" / "common" / "module_support.cpp").is_file()
    assert (project / "include" / "ocudu_dapp_sdk" / "module_support.h").is_file()
    if kind == "scheduler":
        assert "Class-B specifics" in readme
        assert (project / "reference" / "common" / "reference_scheduler_pipeline.cpp").is_file()
        assert (project / "include" / "ocudu_dapp_sdk" / "reference_scheduler_pipeline.h").is_file()
    else:
        assert "Class-C specifics" in readme
        assert (project / "reference" / "common" / "class_c_algorithms.cpp").is_file()
        assert (project / "include" / "ocudu_dapp_sdk" / "class_c_algorithms.h").is_file()
    algorithm_dirs = {path.name for path in (project / "reference").iterdir() if path.is_dir()}
    assert algorithm_dirs == {folder, "common"}
    for path in project.rglob("*"):
        if path.is_file() and path.name != "LICENSE":
            # REUSE-IgnoreStart
            assert "SPDX-License-Identifier: BSD-3-Clause-Clear" in path.read_text()
            # REUSE-IgnoreEnd


def validate_failed_publication_cleanup(generator: Path, output_root: Path) -> None:
    spec = importlib.util.spec_from_file_location("ocudu_dapp_scaffold_class_bc_cpu", generator)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    previous = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous
    output = output_root / "failed"
    args = SimpleNamespace(
        kind="scheduler",
        target="failed_scheduler_cpu",
        global_name="com.deepsig.scaffold.failed.scheduler.cpu",
        package_id="00000000-0000-4000-8000-000000000199",
        output=output,
        vendor="DeepSig scaffold test",
        version="v1",
    )
    try:
        module.publish(args, output_root / "missing-source", output)
    except FileNotFoundError:
        pass
    else:
        raise AssertionError("missing scaffold source unexpectedly succeeded")
    assert not output.exists()
    assert not list(output_root.glob(f".{output.name}.tmp-*"))


def main() -> None:
    args = arguments()
    suffix = "generation" if args.generation_only else "full"
    output_root = args.binary / f"scaffold-bc-{suffix}-test"
    shutil.rmtree(output_root, ignore_errors=True)
    output_root.mkdir(parents=True)
    generator = args.source / "tools/scaffold_class_bc_cpu.py"
    verifier = args.source / "cmake/verify_package_manifest.cmake"
    validate_failed_publication_cleanup(generator, output_root)

    for kind, target, global_name, package_id, interface_id, contract_class, mask, capabilities in CASES:
        project = output_root / kind
        generate = [
            sys.executable, str(generator), "--kind", kind, "--target", target,
            "--global-name", global_name, "--package-id", package_id,
            "--vendor", "DeepSig scaffold test",
            "--output", str(project),
        ]
        run(generate)
        assert not list(output_root.glob(f".{project.name}.tmp-*"))
        run(generate, expect_success=False)  # refuses to overwrite
        validate_generation(project, kind, target)
        if args.generation_only:
            continue

        run([args.cmake, "-S", str(project), "-B", str(project / "build"),
             f"-DOCUDUDAppSDK_DIR={args.sdk_dir}"])
        run([args.cmake, "--build", str(project / "build"), "--parallel"])

        artifact = project / "build" / f"{target}.so"
        manifest = project / "build" / f"{target}.manifest.json"
        sbom = project / "build" / f"{target}.spdx.json"
        run([args.contract, str(artifact), interface_id, contract_class, mask,
             global_name, "DeepSig scaffold test", "v1"])
        run([args.cmake, f"-DMANIFEST={manifest}", f"-DSBOM={sbom}", f"-DARTIFACT={artifact}",
             f"-DEXPECTED_CAPABILITIES={capabilities}", "-P", str(verifier)])

        document = json.loads(manifest.read_text())
        assert document["global_name"] == global_name
        assert document["package_id"] == package_id
        assert document["version"] == "v1"
        assert document["provenance"]["sbom"] == sbom.name
        assert stat.S_IMODE(artifact.stat().st_mode) == 0o555
        assert stat.S_IMODE(manifest.stat().st_mode) == 0o444
        assert stat.S_IMODE(sbom.stat().st_mode) == 0o444


if __name__ == "__main__":
    main()
