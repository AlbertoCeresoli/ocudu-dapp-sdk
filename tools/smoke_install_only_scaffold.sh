#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Smoke: scaffold a Class-A EQ package and build it against install prefixes only.
# Usage:
#   HOST_PREFIX=/opt/ocudu-host TOOLKIT_PREFIX=/opt/ocudu-toolkit \
#     ./tools/smoke_install_only_scaffold.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST_PREFIX="${HOST_PREFIX:-/tmp/ocudu-host-dapp-sdk}"
TOOLKIT_PREFIX="${TOOLKIT_PREFIX:-/tmp/ocudu-dapp-toolkit}"
OUT="${OUT:-/tmp/ocudu-scaffold-smoke}"
ARCHS="${CMAKE_CUDA_ARCHITECTURES:-native}"

if [[ ! -f "${HOST_PREFIX}/lib/cmake/OCUDUDAppSDK/OCUDUDAppSDKConfig.cmake" ]]; then
  echo "missing host ABI at ${HOST_PREFIX} (cmake --install host --component dapp_sdk)" >&2
  exit 1
fi

rm -rf "${OUT}"
python3 "${ROOT}/tools/scaffold_class_a_cuda.py" \
  --kind equalizer \
  --target smoke_eq_cuda \
  --global-name com.ocudu.smoke.equalizer.cuda \
  --package-id 00000000-0000-4000-8000-00000000a004 \
  --vendor "OCUDU smoke" \
  --version v1 \
  --output "${OUT}/eq"

cmake -S "${OUT}/eq" -B "${OUT}/eq/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="${HOST_PREFIX};${TOOLKIT_PREFIX}" \
  -DOCUDUDAppSDK_DIR="${HOST_PREFIX}/lib/cmake/OCUDUDAppSDK" \
  -DCMAKE_CUDA_ARCHITECTURES="${ARCHS}"
cmake --build "${OUT}/eq/build" --parallel

ARTIFACT="${OUT}/eq/build/smoke_eq_cuda.so"
test -f "${ARTIFACT}"

if [[ -x "${TOOLKIT_PREFIX}/bin/certify_class_a_cuda" ]]; then
  "${TOOLKIT_PREFIX}/bin/certify_class_a_cuda" "${ARTIFACT}" 0x00010004 --shape-set compact
elif [[ -x "${ROOT}/build-cuda/certify_class_a_cuda" ]]; then
  "${ROOT}/build-cuda/certify_class_a_cuda" "${ARTIFACT}" 0x00010004 --shape-set compact
else
  echo "warn: certifier not found; build-only smoke ok" >&2
fi

echo "PASS install-only scaffold smoke: ${ARTIFACT}"
