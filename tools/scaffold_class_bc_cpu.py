#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Create an independently buildable Class-B or Class-C CPU dApp from a reference module."""

from __future__ import annotations

import argparse
import re
import shutil
import tempfile
import uuid
from pathlib import Path


# kind -> (folder, interface id, reference global name, dApp class letter,
#          contract-test class number, placement mask, interface placement,
#          manifest capabilities)
KINDS = {
    "scheduler": ("scheduler", "0x00010006", "org.ocudu.reference.scheduler.cpu", "B", "2", "1", "in_process", "0"),
    "spectrum": ("spectrum", "0x00010001", "org.ocudu.reference.spectrum.cpu", "C", "3", "3", "both", "7"),
    "srs-isac": ("srs_isac", "0x00010002", "org.ocudu.reference.srs-isac.cpu", "C", "3", "3", "both", "0"),
}

# Headers each kind vendors beyond the always-present support set.
EXTRA_HEADERS = {
    "scheduler": (
        "include/ocudu_dapp_sdk/package_config.h",
        "include/ocudu_dapp_sdk/reference_scheduler_pipeline.h",
        "include/ocudu_dapp_sdk/reference_scheduler_olla.h",
        "include/ocudu_dapp_sdk/reference_scheduler_tables.h",
    ),
    "spectrum": (
        "include/ocudu_dapp_sdk/class_c_algorithms.h",
        "include/ocudu_dapp_sdk/reference_spectrum_policy.h",
    ),
    "srs-isac": ("include/ocudu_dapp_sdk/class_c_algorithms.h",),
}

# Algorithm sources each kind vendors (the replaceable core).
EXTRA_SOURCES = {
    "scheduler": (
        "reference/common/reference_scheduler_pipeline.cpp",
        "reference/common/reference_scheduler_tables.cpp",
        "reference/common/reference_scheduler_olla.cpp",
    ),
    "spectrum": ("reference/common/class_c_algorithms.cpp",),
    "srs-isac": ("reference/common/class_c_algorithms.cpp",),
}

CMAKE_TEMPLATE = """# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

cmake_minimum_required(VERSION 3.18)
project(@TARGET@ VERSION @CMAKE_VERSION@ LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Release recommended)" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
  message(STATUS "CMAKE_BUILD_TYPE defaulted to Release (-O3)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# Host ABI only. The replaceable reference sources are vendored into this tree
# so a vendor build does not need a sibling ocudu-dapp-sdk checkout.
find_package(OCUDUDAppSDK 1 CONFIG REQUIRED)

add_library(dapp_support STATIC @SUPPORT_SOURCES@)
set_target_properties(dapp_support PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(dapp_support PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dapp_support PUBLIC OCUDU::DAppSDK)

add_library(dapp_algorithm STATIC @ALGORITHM_SOURCES@)
set_target_properties(dapp_algorithm PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(dapp_algorithm PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dapp_algorithm PUBLIC OCUDU::DAppSDK)

add_library(@TARGET@ MODULE @MODULE_SOURCES@)
set_target_properties(@TARGET@ PROPERTIES PREFIX "")
target_link_libraries(@TARGET@ PRIVATE dapp_support dapp_algorithm)

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
    -DDAPP_CLASS=@DAPP_CLASS@
    -DINTERFACE_ID=@INTERFACE_ID@
    -DPACKAGE_PLACEMENT=in_process
    -DINTERFACE_PLACEMENT=@INTERFACE_PLACEMENT@
    -DCAPABILITIES=@CAPABILITIES@
    -DBACKEND=cpu
    -P ${PROJECT_SOURCE_DIR}/cmake/write_package_manifest.cmake
  DEPENDS @TARGET@ ${PROJECT_SOURCE_DIR}/cmake/write_package_manifest.cmake
  VERBATIM)
add_custom_target(@TARGET@_package ALL DEPENDS ${manifest} ${sbom})

include(GNUInstallDirs)
install(TARGETS @TARGET@ LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/ocudu-dapps)
install(FILES ${manifest} ${sbom} DESTINATION ${CMAKE_INSTALL_LIBDIR}/ocudu-dapps)
"""

README_COMMON = """<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# @GLOBAL_NAME@

This project is an independently buildable Class-@DAPP_CLASS@ CPU **@KIND@**
dApp seeded from the OCUDU reference implementation. It consumes only the
installed `OCUDUDAppSDK` package; it does not require the OCUDU source tree.

The stable module/lifecycle shell, contract validation, typed configuration
schema, and per-worker ownership rules are included. The replaceable algorithm
entry may throw, but the C ABI callback must not: keep the generated
`guard_algorithm_call` wrapper, which maps allocation failure to `RESOURCE`
and all other exceptions to `EXCEPTION` so OCUDU records the incident and
applies its breaker policy without unwinding across the ABI.

Keep the query trampoline in `reference/@FOLDER@/module.cpp` pure: OCUDU
queries it in a disposable preflight helper and again in the DU. Allocation,
tables, and history belong in the instance/worker state objects, never in
descriptor discovery or on the invocation path.

Build with:

```text
cmake -S . -B build -DOCUDUDAppSDK_DIR=/path/to/lib/cmake/OCUDUDAppSDK
cmake --build build
```

Validate the final artifact through the public ABI with the SDK's contract
tester (from an ocudu-dapp-sdk build tree):

```text
module_contract_test build/@TARGET@.so @INTERFACE_ID@ @CONTRACT_CLASS@ @PLACEMENT_MASK@ \\
  @GLOBAL_NAME@ "@VENDOR@" @VERSION@
```

The build emits `@TARGET@.so`, its hash-bound read-only package manifest
record, and its digest-bound read-only SPDX 2.3 JSON SBOM.

@CLASS_NOTES@
"""

CLASS_NOTES = {
    "scheduler": """## Class-B specifics

Your algorithm advises the MAC scheduler inside a ~100 microsecond complete
boundary: finish fast, emit intents, and let the host validate and commit.
The vendored five-stage pipeline (rank / OLLA / MCS / pack / TPC in
`reference/common/reference_scheduler_*.cpp`) ships with **every stage off** -
host-equivalent behavior until a stage is enabled over E3 through the typed
config schema declared in `reference/scheduler/module.cpp`. Replace stages
(or the whole pipeline behind `run_scheduler_pipeline`) with your policy;
honor `allowed_output_flags`, never emit an intent class the host did not
grant, and keep per-UE history in the lane-local worker state.

Introduce authority evaluation-first: run under
`scheduler_evaluation_only: true` and judge
`class_b.policy.{invocations,accepted,fallbacks,validation_rejections,deadline_misses,dapp_time_ns}`
before granting any allow flag. See the SDK's
`docs/class_b_scheduler_baseline.md` and the platform's Class-B tutorial.""",
    "spectrum": """## Class-C specifics

Your algorithm observes the uplink spectrum snapshot and must never block the
producer: validate the snapshot, compute, and publish bounded results through
`host_api.publish_prepared` (interference maps, quiet-period intents). Every
publication is advice until the host authority accepts and clamps it. The
vendored radiometer in `reference/common/class_c_algorithms.cpp` is the
replaceable core; the policy knobs live in
`include/ocudu_dapp_sdk/reference_spectrum_policy.h`. This package also admits
a supervised out-of-process placement (`both`), where the same module runs
behind the host's process supervisor. See `docs/class_c_cuda_process_authoring.md`
and the standalone Class-C tutorial for the full placement spectrum.""",
    "srs-isac": """## Class-C specifics

Your algorithm consumes SRS channel-estimate snapshots for ISAC-style
processing and must never block the producer: validate, compute, and publish
bounded previews/results through `host_api.publish_prepared`. The vendored
CIR sketch in `reference/common/class_c_algorithms.cpp` is a starting point,
not a sensing capability - replace it with your estimator. This package also
admits a supervised out-of-process placement (`both`). See
`docs/class_c_cuda_process_authoring.md` and the standalone Class-C tutorial.""",
}


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
    if not args.vendor or any(character in args.vendor for character in '\\"\r\n'):
        raise SystemExit("--vendor must be nonempty and cannot contain backslashes, quotes, or newlines")
    if args.output.exists():
        raise SystemExit(f"output already exists: {args.output}")


def copy_file(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def materialize(args: argparse.Namespace, root: Path, output: Path) -> None:
    folder, interface_id, old_global_name, dapp_class, contract_class, placement_mask, interface_placement, caps = (
        KINDS[args.kind]
    )

    vendored = [
        "LICENSE",
        "cmake/write_package_manifest.cmake",
        "include/ocudu_dapp_sdk/module_support.h",
        "include/ocudu_dapp_sdk/class_c_config.h",
        "reference/common/module_support.cpp",
        "reference/common/class_c_config.cpp",
        f"reference/{folder}/module.cpp",
    ]
    vendored.extend(EXTRA_HEADERS[args.kind])
    vendored.extend(EXTRA_SOURCES[args.kind])
    for relative in vendored:
        copy_file(root / relative, output / relative)

    module_sources = f"reference/{folder}/module.cpp"
    support_sources = "reference/common/module_support.cpp reference/common/class_c_config.cpp"
    algorithm_sources = " ".join(EXTRA_SOURCES[args.kind])

    module = output / f"reference/{folder}/module.cpp"
    module.write_text(module.read_text().replace(old_global_name, args.global_name))

    support = output / "reference/common/module_support.cpp"
    support_text = support.read_text()
    if 'return populate_module(host, output, name, "OCUDU reference", "v1", interface);' in support_text:
        support_text = support_text.replace(
            'return populate_module(host, output, name, "OCUDU reference", "v1", interface);',
            f'return populate_module(host, output, name, "{args.vendor}", "{args.version}", interface);',
        )
    else:
        raise SystemExit(
            "scaffold: reference/common/module_support.cpp no longer matches the "
            "identity-population pattern; refusing to emit a package whose "
            "vendor/version would disagree with its manifest"
        )
    support.write_text(support_text)

    replacements = {
        "@ALGORITHM_SOURCES@": algorithm_sources,
        "@CAPABILITIES@": caps,
        "@CLASS_NOTES@": CLASS_NOTES[args.kind],
        "@CMAKE_VERSION@": args.version.removeprefix("v"),
        "@CONTRACT_CLASS@": contract_class,
        "@DAPP_CLASS@": dapp_class,
        "@FOLDER@": folder,
        "@GLOBAL_NAME@": args.global_name,
        "@INTERFACE_ID@": interface_id,
        "@INTERFACE_PLACEMENT@": interface_placement,
        "@KIND@": args.kind,
        "@MODULE_SOURCES@": module_sources,
        "@PACKAGE_ID@": str(uuid.UUID(args.package_id)),
        "@PLACEMENT_MASK@": placement_mask,
        "@SUPPORT_SOURCES@": support_sources,
        "@TARGET@": args.target,
        "@VENDOR@": args.vendor,
        "@VERSION@": args.version,
    }
    cmake = CMAKE_TEMPLATE
    readme = README_COMMON
    for token, value in replacements.items():
        cmake = cmake.replace(token, value)
        readme = readme.replace(token, value)
    for name, text in (("CMakeLists.txt", cmake), ("README.md", readme)):
        if "@" in text.replace(args.vendor, "").replace(args.global_name, ""):
            raise SystemExit(f"scaffold: unresolved template token in generated {name}")
        (output / name).write_text(text)


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
