#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=ocudu-host.lock
source "${script_dir}/ocudu-host.lock"

: "${OCUDU_HOST_REPOSITORY_URL:=${OCUDU_HOST_LOCK_REPOSITORY_URL}}"
: "${OCUDU_HOST_REF:=${OCUDU_HOST_LOCK_REF}}"
: "${OCUDU_HOST_SOURCE_DIR:=${CI_PROJECT_DIR:-$PWD}/.ci/ocudu}"
: "${OCUDU_HOST_BUILD_DIR:=${CI_PROJECT_DIR:-$PWD}/.ci/ocudu-build}"
: "${OCUDU_HOST_INSTALL_PREFIX:=${CI_PROJECT_DIR:-$PWD}/.ci/ocudu-install}"
: "${OCUDU_HOST_ENABLE_CUDA:=OFF}"
: "${OCUDU_HOST_INTEROP:=OFF}"
: "${OCUDU_CI_JOBS:=2}"

if [[ ! -d "${OCUDU_HOST_SOURCE_DIR}/.git" ]]; then
  mkdir -p "$(dirname "${OCUDU_HOST_SOURCE_DIR}")"
  git init "${OCUDU_HOST_SOURCE_DIR}"
  git -C "${OCUDU_HOST_SOURCE_DIR}" remote add origin "${OCUDU_HOST_REPOSITORY_URL}"
  git -C "${OCUDU_HOST_SOURCE_DIR}" fetch --depth 1 origin "${OCUDU_HOST_REF}"
  git -C "${OCUDU_HOST_SOURCE_DIR}" checkout --detach FETCH_HEAD
  git -C "${OCUDU_HOST_SOURCE_DIR}" remote set-url origin disabled://ci-source
fi

actual_host_ref="$(git -C "${OCUDU_HOST_SOURCE_DIR}" rev-parse HEAD)"
expected_host_ref="$(git -C "${OCUDU_HOST_SOURCE_DIR}" rev-parse "${OCUDU_HOST_REF}^{commit}")"
if [[ "${actual_host_ref}" != "${expected_host_ref}" ]]; then
  printf 'host source mismatch: expected %s, found %s\n' "${expected_host_ref}" "${actual_host_ref}" >&2
  exit 1
fi

cuda_arguments=()
if [[ "${OCUDU_HOST_ENABLE_CUDA}" == "ON" ]]; then
  : "${OCUDU_CUDA_ARCHITECTURES:?CUDA host SDK build needs OCUDU_CUDA_ARCHITECTURES}"
  cuda_arguments+=(
    -DENABLE_CUDA=ON
    "-DCMAKE_CUDA_ARCHITECTURES=${OCUDU_CUDA_ARCHITECTURES}"
  )
fi

interop_arguments=(-DBUILD_TESTING=OFF)
if [[ "${OCUDU_HOST_INTEROP}" == "ON" ]]; then
  git -C "${OCUDU_HOST_SOURCE_DIR}" submodule update --init --recursive
  test -f "${OCUDU_HOST_SOURCE_DIR}/scripts/check_dapp_interop_gate.py" || {
    echo "Pinned host lacks the mandatory interop gate; set OCUDU_HOST_REF to the reviewed paired host commit." >&2
    exit 1
  }
  interop_arguments=(-DBUILD_TESTING=ON -DOCUDU_DAPP_LIBE3_INTEROP=ON)
fi

cmake -S "${OCUDU_HOST_SOURCE_DIR}" -B "${OCUDU_HOST_BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  "${interop_arguments[@]}" \
  -DENABLE_DAPP=ON \
  -DENABLE_E3=ON \
  -DENABLE_E3_ASN1_CODEC=ON \
  -DENABLE_UHD=OFF \
  -DENABLE_ZEROMQ=OFF \
  -DENABLE_DPDK=OFF \
  -DENABLE_MKL=OFF \
  -DENABLE_ARMPL=OFF \
  -DENABLE_PLUGINS=OFF \
  "${cuda_arguments[@]}"
cmake --build "${OCUDU_HOST_BUILD_DIR}" --parallel "${OCUDU_CI_JOBS}" --target \
  ocudu_dapp_management ocudu_dapp_e3sm_asn1 ocudu_dapp_e3_codec ocudu_dapp_e3_client ocudu_dapp_e3ap_codec
cmake --install "${OCUDU_HOST_BUILD_DIR}" \
  --prefix "${OCUDU_HOST_INSTALL_PREFIX}" --component dapp_sdk

if [[ "${OCUDU_HOST_INTEROP}" == "ON" ]]; then
  cmake --build "${OCUDU_HOST_BUILD_DIR}" --parallel "${OCUDU_CI_JOBS}" --target \
    dapp_runtime_test dapp_telemetry_allocation_test e3ap_test_server libe3_interop_dapp libe3_interop_vectors ocudu-dappctl
  python3 "${OCUDU_HOST_SOURCE_DIR}/scripts/check_dapp_interop_gate.py" --build "${OCUDU_HOST_BUILD_DIR}"
fi

# The lock records the public C ABI the SDK was validated against; refuse a host
# install whose package config advertises a different major version.
installed_abi_dir="${OCUDU_HOST_INSTALL_PREFIX}/lib/cmake/OCUDUDAppSDK"
if [[ -d "${installed_abi_dir}" ]] && ! grep -q "PACKAGE_VERSION \"${OCUDU_HOST_LOCK_DAPP_ABI%%.*}" "${installed_abi_dir}"/OCUDUDAppSDKConfigVersion.cmake; then
  printf 'installed dApp SDK ABI does not match the locked ABI %s\n' "${OCUDU_HOST_LOCK_DAPP_ABI}" >&2
  exit 1
fi

# Exported so the CI jobs that source this script can pass it to CMake; also
# printed for callers that capture stdout.
export OCUDU_HOST_INSTALL_PREFIX
printf '%s\n' "${OCUDU_HOST_INSTALL_PREFIX}"
