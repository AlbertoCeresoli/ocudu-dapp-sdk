#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Generate, build, and optionally certify fresh Class-A CUDA projects."""

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


CASES = (
    ("ce", "scaffold_ce_cuda", "com.deepsig.scaffold.ce.cuda", "00000000-0000-4000-8000-000000000003", "0x00010003"),
    ("equalizer", "scaffold_equalizer_cuda", "com.deepsig.scaffold.equalizer.cuda",
     "00000000-0000-4000-8000-000000000004", "0x00010004"),
    ("receiver", "scaffold_receiver_cuda", "com.deepsig.scaffold.receiver.cuda",
     "00000000-0000-4000-8000-000000000005", "0x00010005"),
)

HEADERS = {
    "ce": "channel_estimator_cuda.h",
    "equalizer": "equalizer_cuda.h",
    "receiver": "receiver_cuda.h",
}


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--generation-only", action="store_true")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--cmake")
    parser.add_argument("--sdk-dir")
    parser.add_argument("--contract")
    parser.add_argument("--certifier")
    parser.add_argument("--cuda-architectures", default="")
    args = parser.parse_args()
    if args.generation_only and args.build_only:
        parser.error("--generation-only and --build-only are mutually exclusive")
    if not args.generation_only and not all((args.cmake, args.sdk_dir, args.contract)):
        parser.error("CUDA scaffold builds require --cmake, --sdk-dir, and --contract")
    if not args.generation_only and not args.build_only and not args.certifier:
        parser.error("CUDA scaffold certification requires --certifier")
    return args


def run(command: list[str], *, expect_success: bool = True, capture: bool = False) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, check=False, text=True, capture_output=capture)
    if (result.returncode == 0) != expect_success:
        raise SystemExit(f"unexpected exit {result.returncode}: {' '.join(command)}\n{result.stdout}\n{result.stderr}")
    return result


def validate_generation(project: Path, kind: str, folder: str, function: str) -> None:
    readme = (project / "README.md").read_text()
    cmake = (project / "CMakeLists.txt").read_text()
    assert "Replace the implementation" in readme
    assert function in readme
    assert "find_package(OCUDUDAppSDK 1 CONFIG REQUIRED)" in cmake
    assert " VERSION 1 LANGUAGES CXX CUDA)" in cmake
    assert "-DVERSION=v1" in cmake
    assert "OCUDU::DAppSDK" in cmake
    assert "@" not in readme
    assert "@" not in cmake

    generated_headers = project / "include" / "ocudu_dapp_sdk"
    assert (generated_headers / HEADERS[kind]).is_file()
    assert not (generated_headers / "reference_phy_algorithms_cuda.h").exists()
    if kind == "equalizer":
        assert (generated_headers / "class_c_config.h").is_file()
        assert (generated_headers / "package_config.h").is_file()
        assert (project / "reference" / "common" / "class_c_config.cpp").is_file()
    algorithm_dirs = {path.name for path in (project / "reference").iterdir() if path.is_dir() and path.name != "common"}
    assert algorithm_dirs == {folder}

    for path in project.rglob("*"):
        if path.is_file() and path.name != "LICENSE":
            # REUSE-IgnoreStart
            assert "SPDX-License-Identifier: BSD-3-Clause-Clear" in path.read_text()
            # REUSE-IgnoreEnd


def validate_failed_publication_cleanup(generator: Path, output_root: Path) -> None:
    spec = importlib.util.spec_from_file_location("ocudu_dapp_scaffold_class_a_cuda", generator)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    previous_dont_write_bytecode = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous_dont_write_bytecode
    output = output_root / "failed"
    args = SimpleNamespace(
        kind="ce",
        target="failed_ce_cuda",
        global_name="com.deepsig.scaffold.failed.ce.cuda",
        package_id="00000000-0000-4000-8000-000000000099",
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
    suffix = "generation" if args.generation_only else "build" if args.build_only else "certification"
    output_root = args.binary / f"scaffold-{suffix}-test"
    shutil.rmtree(output_root, ignore_errors=True)
    output_root.mkdir(parents=True)
    generator = args.source / "tools/scaffold_class_a_cuda.py"
    verifier = args.source / "cmake/verify_package_manifest.cmake"
    validate_failed_publication_cleanup(generator, output_root)

    for kind, target, global_name, package_id, interface_id in CASES:
        project = output_root / kind
        generate = [
            sys.executable, str(generator), "--kind", kind, "--target", target,
            "--global-name", global_name, "--package-id", package_id,
            "--vendor", "DeepSig scaffold test",
            "--output", str(project),
        ]
        run(generate)
        assert not list(output_root.glob(f".{project.name}.tmp-*"))
        run(generate, expect_success=False)
        assert not list(output_root.glob(f".{project.name}.tmp-*"))

        folder = {"ce": "channel_estimator_cuda", "equalizer": "equalizer_cuda", "receiver": "receiver_cuda"}[kind]
        function = {"ce": "estimate_channel_cuda", "equalizer": "equalize_cuda", "receiver": "receive_cuda"}[kind]
        validate_generation(project, kind, folder, function)
        if args.generation_only:
            continue

        configure = [args.cmake, "-S", str(project), "-B", str(project / "build"),
                     f"-DOCUDUDAppSDK_DIR={args.sdk_dir}"]
        if args.cuda_architectures:
            configure.append(f"-DCMAKE_CUDA_ARCHITECTURES={args.cuda_architectures}")
        run(configure)
        run([args.cmake, "--build", str(project / "build"), "--parallel"])

        artifact = project / "build" / f"{target}.so"
        manifest = project / "build" / f"{target}.manifest.json"
        sbom = project / "build" / f"{target}.spdx.json"
        run([args.contract, str(artifact), interface_id, "1", "1", global_name, "DeepSig scaffold test", "v1"])
        model = project / "build" / "receiver-model.bin"
        if kind == "receiver":
            run([str(project / "build" / "make_receiver_cuda_model"), str(model), "1.0"])
            assert model.stat().st_size == 264
        if not args.build_only:
            certify = [args.certifier, str(artifact), interface_id]
            if kind == "receiver":
                certify.extend([
                    "--model-artifact", str(model),
                    "--model-id", "scaffold-baseline",
                    "--candidate-model-id", "scaffold-candidate",
                    "--model-version", "1",
                    "--model-generation", "1",
                ])
            certificate = json.loads(run(certify, capture=True).stdout)
            assert certificate["outputStorage"] == {
                "byteOffset": 256,
                "guardBytes": 256,
                "boundsChecked": True,
                "identityChecked": True,
                "completeWritesChecked": True,
            }
            assert certificate["coordinateBasis"] == "rxGrid"
            assert certificate["invocations"] == 96
            multi_symbol = [shape for shape in certificate["shapes"] if shape["allocationSymbols"] > 1]
            assert multi_symbol == [{
                "prb": 25,
                "ports": 2,
                "layers": 2,
                "startSymbol": 2,
                "startPrb": 6,
                "gridSymbols": 14,
                "gridPrb": 106,
                "allocationSymbols": 4,
                "dmrsSymbols": [2, 5],
                "pilots": 4,
                "dataRe": 1194,
            }]
        run([args.cmake, f"-DMANIFEST={manifest}", f"-DSBOM={sbom}", f"-DARTIFACT={artifact}",
             "-DEXPECTED_CAPABILITIES=1", "-P", str(verifier)])

        document = json.loads(manifest.read_text())
        assert document["global_name"] == global_name
        assert document["package_id"] == package_id
        assert document["version"] == "v1"
        assert document["schema_version"] == 1
        assert document["provenance"]["sbom"] == sbom.name
        assert document["provenance"]["format"] == "spdx-2.3-json"
        assert stat.S_IMODE(artifact.stat().st_mode) == 0o555
        assert stat.S_IMODE(manifest.stat().st_mode) == 0o444
        assert stat.S_IMODE(sbom.stat().st_mode) == 0o444
        readme = (project / "README.md").read_text()
        assert "representative 25- and 106-PRB/two-port/two-layer" in readme
        assert "poisoned CUDA" in readme


if __name__ == "__main__":
    main()
