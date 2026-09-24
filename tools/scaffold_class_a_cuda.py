#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Create an independently buildable Class-A CUDA dApp from one reference depth."""

from __future__ import annotations

import argparse
import re
import shutil
import tempfile
import uuid
from pathlib import Path


KINDS = {
    "ce": ("channel_estimator_cuda", "0x00010003", "org.ocudu.reference.channel-estimator.cuda"),
    "equalizer": ("equalizer_cuda", "0x00010004", "org.ocudu.reference.equalizer.cuda"),
    "receiver": ("receiver_cuda", "0x00010005", "org.ocudu.reference.receiver.cuda"),
}

ALGORITHM_FUNCTIONS = {
    "ce": "estimate_channel_cuda",
    "equalizer": "equalize_cuda",
    "receiver": "receive_cuda",
}

ALGORITHM_HEADERS = {
    "ce": "channel_estimator_cuda.h",
    "equalizer": "equalizer_cuda.h",
    "receiver": "receiver_cuda.h",
}

CMAKE_TEMPLATE = """# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

cmake_minimum_required(VERSION 3.18)
project(@TARGET@ VERSION @CMAKE_VERSION@ LANGUAGES CXX CUDA)

# Default Release (-O3): unoptimized Class-A packages miss PUSCH deadlines OTA.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Release recommended for OTA)" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
  message(STATUS "CMAKE_BUILD_TYPE defaulted to Release (-O3)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wall>
    $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
    $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>
    $<$<COMPILE_LANGUAGE:CXX>:-Werror>)
endif()

# Host ABI only. Open Class-A CUDA sources are vendored into this tree by the
# scaffold so a vendor build does not need a sibling ocudu-dapp-sdk checkout.
find_package(OCUDUDAppSDK 1 CONFIG REQUIRED)
find_package(CUDAToolkit REQUIRED)

add_library(dapp_support STATIC @SUPPORT_SOURCES@)
set_target_properties(dapp_support PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(dapp_support PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dapp_support PUBLIC OCUDU::DAppSDK)

add_library(dapp_algorithm STATIC @ALGORITHM_SOURCES@)
set_target_properties(dapp_algorithm PROPERTIES
  POSITION_INDEPENDENT_CODE ON CUDA_SEPARABLE_COMPILATION ON)
target_include_directories(dapp_algorithm PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dapp_algorithm PUBLIC OCUDU::DAppSDK CUDA::cudart)

add_library(@TARGET@ MODULE @MODULE_SOURCES@)
set_target_properties(@TARGET@ PROPERTIES PREFIX "")
target_link_libraries(@TARGET@ PRIVATE dapp_support dapp_algorithm)

@MODEL_TOOL@

set(manifest ${PROJECT_BINARY_DIR}/@TARGET@.manifest.json)
set(sbom ${PROJECT_BINARY_DIR}/@TARGET@.spdx.json)
add_custom_command(
  OUTPUT ${manifest} ${sbom}
  COMMAND ${CMAKE_COMMAND}
    -DOUTPUT=${manifest}
    -DSBOM=${sbom}
    -DARTIFACT=$<TARGET_FILE:@TARGET@>
    -DPACKAGE_ID=@PACKAGE_ID@
    -DGLOBAL_NAME=@GLOBAL_NAME@
    "-DVENDOR=@VENDOR@"
    -DVERSION=@VERSION@
    -DDAPP_CLASS=A
    -DINTERFACE_ID=@INTERFACE_ID@
    -DPACKAGE_PLACEMENT=in_process
    -DINTERFACE_PLACEMENT=in_process
    -DCAPABILITIES=1
    -DBACKEND=cuda
    -P ${PROJECT_SOURCE_DIR}/cmake/write_package_manifest.cmake
  DEPENDS @TARGET@ ${PROJECT_SOURCE_DIR}/cmake/write_package_manifest.cmake
  VERBATIM)
add_custom_target(@TARGET@_package ALL DEPENDS ${manifest} ${sbom})

# Default CUDA architectures when the caller does not set them (override for
# your deployment GPU, e.g. -DCMAKE_CUDA_ARCHITECTURES=121 for GB10).
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES OR CMAKE_CUDA_ARCHITECTURES STREQUAL "")
  set(CMAKE_CUDA_ARCHITECTURES 89-real;90-real;121)
endif()

include(GNUInstallDirs)
install(TARGETS @TARGET@ LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/ocudu-dapps)
install(FILES ${manifest} ${sbom} DESTINATION ${CMAKE_INSTALL_LIBDIR}/ocudu-dapps)
"""

README_TEMPLATE = """<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# @GLOBAL_NAME@

This project is an independently buildable Class-A CUDA **@KIND@** dApp seeded
from the OCUDU reference implementation. It consumes only the installed
`OCUDUDAppSDK` package; it does not require the OCUDU source tree.

The stable module/lifecycle shell, contract validation, inherited-stream rules,
and per-worker ownership are included. Replace the implementation in
`reference/@FOLDER@/algorithm.cu`. The selected algorithm header exposes one
instance-owned state object shared by all lanes and one mutable state object per
worker/lane. Extend those types for prepared weights, immutable lookup tables,
lane-local scratch, or history; never use an invocation-global singleton. For
receiver model work, retain the generation, completion-event, and retirement
rules in `module_support.cpp`.

The replaceable algorithm entry may throw, but the C ABI callback must not.
Keep the generated `guard_algorithm_call` wrapper: it maps allocation failure
to `RESOURCE` and all other exceptions to `EXCEPTION`, which lets OCUDU record
the incident and apply its breaker policy without unwinding across the ABI or
terminating the DU.
The receiver shell records its model-slot completion fence even after a failed
or throwing algorithm call, because the implementation may have enqueued work
before failing. Preserve that rule when replacing its model state.

The single algorithm entry point is `@ALGORITHM_FUNCTION@`; its state references
are already resolved before the callback. Keep the query
trampoline pure: OCUDU queries it in a disposable preflight helper and again in
the DU. CUDA allocation, model upload, scratch, and threads belong in lifecycle
or per-worker state, never descriptor discovery or invocation. The equalizer
and receiver shells already own CUDA scratch independently for every admitted
PUSCH lane. The CE state types are intentionally empty in the reference so a
copied implementation can add only the resources it needs.

Build with:

```text
cmake -S . -B build -DOCUDUDAppSDK_DIR=/path/to/lib/cmake/OCUDUDAppSDK
cmake --build build
```

Then run `certify_class_a_cuda` from the OCUDU dApp reference SDK build against
the final artifact:

```text
certify_class_a_cuda build/@TARGET@.so @INTERFACE_ID@
```

It exercises two resident CUDA workers and streams at the
1-PRB/1-port/1-layer minimum, representative 25- and 106-PRB/two-port/two-layer
shapes, a four-symbol/two-DM-RS-symbol nonzero-origin allocation, and the exact
declared maximum. The multi-symbol references require joint processing to
resolve both layers. Every shape runs QPSK, 16-QAM, 64-QAM,
and 256-QAM under declared DM-RS Types 1 and 2, including a DC-overlap slice and
dense canonical compact outputs. Caller-owned outputs use nonzero byte offsets;
the certifier verifies descriptor identity, complete writes, and poisoned CUDA
guard regions before emitting a single-line JSON result. Keep that result with
the separately hash-verified manifest and digest-bound SPDX SBOM, then add every extra deployment-specific
shape and model artifact required for your release.
Use `--shape-set compact` for a practical two-worker, maximum-layer racecheck;
keep the default boundary matrix for certification and memcheck.

The build emits `@TARGET@.so`, its hash-bound read-only package manifest-v1
record, and its digest-bound read-only SPDX 2.3 JSON SBOM.
@MODEL_NOTE@
"""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=KINDS, required=True)
    parser.add_argument("--target", required=True, help="CMake target and artifact basename")
    parser.add_argument("--global-name", required=True, help="Stable reverse-DNS module identity")
    parser.add_argument("--package-id", required=True, help="Unique package UUID")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--vendor", default="DeepSig")
    parser.add_argument("--version", default="v1")
    return parser.parse_args()


def validate(args: argparse.Namespace) -> None:
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", args.target):
        raise SystemExit("--target must match [A-Za-z][A-Za-z0-9_]*")
    if not re.fullmatch(r"[A-Za-z0-9]+(?:[.-][A-Za-z0-9]+)+", args.global_name):
        raise SystemExit("--global-name must be a dot/hyphen-separated stable identity")
    try:
        uuid.UUID(args.package_id)
    except ValueError as error:
        raise SystemExit("--package-id must be a UUID") from error
    if not re.fullmatch(r"(?:v?1|[0-9]+\.[0-9]+\.[0-9]+)", args.version):
        raise SystemExit("--version must be 1, v1, or numeric MAJOR.MINOR.PATCH")
    if not args.vendor or any(character in args.vendor for character in '\\\"\r\n'):
        raise SystemExit("--vendor must be nonempty and cannot contain backslashes, quotes, or newlines")
    if args.output.exists():
        raise SystemExit(f"output already exists: {args.output}")


def copy_file(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def materialize(args: argparse.Namespace, root: Path, output: Path) -> None:
    folder, interface_id, old_global_name = KINDS[args.kind]

    for relative in (
        "LICENSE",
        "cmake/write_package_manifest.cmake",
        "include/ocudu_dapp_sdk/module_support.h",
        f"include/ocudu_dapp_sdk/{ALGORITHM_HEADERS[args.kind]}",
        # Full CUDA LS/ZF contract (not the in-tree shim under reference/common/).
        "include/ocudu_dapp_sdk/class_a_cuda_contract.cuh",
        "reference/common/module_support.cpp",
        f"reference/{folder}/algorithm.cu",
        f"reference/{folder}/module.cpp",
    ):
        copy_file(root / relative, output / relative)

    algorithm_sources = f"reference/{folder}/algorithm.cu"
    module_sources = f"reference/{folder}/module.cpp"
    support_sources = "reference/common/module_support.cpp"
    model_tool = ""
    model_note = ""
    if args.kind in ("ce", "equalizer", "receiver"):
        relative = f"reference/{folder}/module_support.cpp"
        copy_file(root / relative, output / relative)
        module_sources += f" reference/{folder}/module_support.cpp"
    if args.kind in ("equalizer", "receiver"):
        # algorithm.cu exports only ref_class_a::*; open ABI wrappers live here.
        copy_file(root / "include/ocudu_dapp_sdk/ref_class_a_cuda.h",
                  output / "include/ocudu_dapp_sdk/ref_class_a_cuda.h")
        api = f"reference/{folder}/reference_api.cpp"
        copy_file(root / api, output / api)
        algorithm_sources += f" {api}"
    if args.kind == "equalizer":
        for relative in (
            "include/ocudu_dapp_sdk/class_c_config.h",
            "include/ocudu_dapp_sdk/package_config.h",
            "reference/common/class_c_config.cpp",
        ):
            copy_file(root / relative, output / relative)
        support_sources += " reference/common/class_c_config.cpp"
    if args.kind == "receiver":
        copy_file(root / "include/ocudu_dapp_sdk/receiver_symbol_storage.h",
                  output / "include/ocudu_dapp_sdk/receiver_symbol_storage.h")
        copy_file(root / "tools/make_receiver_cuda_model.cpp", output / "tools/make_receiver_cuda_model.cpp")
        model_tool = """add_executable(make_receiver_cuda_model tools/make_receiver_cuda_model.cpp)
target_include_directories(make_receiver_cuda_model PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(make_receiver_cuda_model PRIVATE OCUDU::DAppSDK)"""
        model_note = f"""`make_receiver_cuda_model OUTPUT [SCALE]` emits the bounded example model artifact. Certify its lifecycle with:

```text
certify_class_a_cuda build/{args.target}.so {interface_id} \\
  --model-artifact build/receiver-model.bin \\
  --model-id baseline --candidate-model-id candidate \\
  --model-version 1 --model-generation 1
```"""

    for relative in (f"reference/{folder}/module.cpp", f"reference/{folder}/module_support.cpp"):
        path = output / relative
        if path.exists():
            path.write_text(path.read_text().replace(old_global_name, args.global_name))
    support = output / "reference/common/module_support.cpp"
    support_text = support.read_text()
    # Prefer identity-aware populate if present; else rewrite default vendor/version.
    if 'return populate_module(host, output, name, "OCUDU reference", "v1", interface);' in support_text:
        support_text = support_text.replace(
            'return populate_module(host, output, name, "OCUDU reference", "v1", interface);',
            f'return populate_module(host, output, name, "{args.vendor}", "{args.version}", interface);',
        )
    else:
        raise SystemExit(
            "scaffold: reference/common/module_support.cpp no longer matches the "
            "identity-population pattern; refusing to emit a package whose "
            "vendor/version would disagree with its manifest")
    support.write_text(support_text)

    replacements = {
        "@ALGORITHM_FUNCTION@": ALGORITHM_FUNCTIONS[args.kind],
        "@ALGORITHM_SOURCES@": algorithm_sources,
        "@FOLDER@": folder,
        "@GLOBAL_NAME@": args.global_name,
        "@INTERFACE_ID@": interface_id,
        "@KIND@": args.kind,
        "@MODEL_NOTE@": model_note,
        "@MODEL_TOOL@": model_tool,
        "@MODULE_SOURCES@": module_sources,
        "@PACKAGE_ID@": str(uuid.UUID(args.package_id)),
        "@SUPPORT_SOURCES@": support_sources,
        "@TARGET@": args.target,
        "@VENDOR@": args.vendor,
        "@VERSION@": args.version,
        "@CMAKE_VERSION@": args.version.removeprefix("v"),
    }
    cmake = CMAKE_TEMPLATE
    readme = README_TEMPLATE
    for token, value in replacements.items():
        cmake = cmake.replace(token, value)
        readme = readme.replace(token, value)
    (output / "CMakeLists.txt").write_text(cmake)
    (output / "README.md").write_text(readme)


def publish(args: argparse.Namespace, root: Path, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{output.name}.tmp-", dir=output.parent))
    try:
        materialize(args, root, staging)
        if output.exists():
            raise SystemExit(f"output already exists: {output}")
        staging.rename(output)
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def main() -> None:
    args = parse_args()
    validate(args)
    publish(args, Path(__file__).resolve().parents[1], args.output.resolve())


if __name__ == "__main__":
    main()
