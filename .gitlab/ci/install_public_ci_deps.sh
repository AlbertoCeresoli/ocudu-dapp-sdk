#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

set -euo pipefail

profile="${1:-build}"
export DEBIAN_FRONTEND=noninteractive
apt-get update

# clang-format is pinned to major 18: the repository is formatted with 18.x and
# other majors disagree on AlignConsecutiveDeclarations / BinPackParameters.
# Keep this pin in sync with CONTRIBUTING.md when reformatting the tree.
packages=(ca-certificates git python3 python3-pip python3-numpy python3-h5py python3-flatbuffers clang-format-18)
if [[ "${profile}" == "build" ]]; then
  packages+=(
    build-essential clang cmake ninja-build pkg-config autoconf automake libtool bison flex
    flatbuffers-compiler libflatbuffers-dev libgtest-dev
    libboost-program-options-dev libconfig++-dev libfftw3-dev
    libmbedtls-dev libsctp-dev libyaml-cpp-dev
    # Python Class-C pybind bindings compile in CI (tests still probe torch).
    python3-dev python3-pybind11
  )
fi

apt-get install --no-install-recommends -y "${packages[@]}"

# Expose the pinned clang-format major under the unsuffixed name used by CI and docs.
if command -v clang-format-18 >/dev/null && ! command -v clang-format >/dev/null; then
  update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-18 100
fi

# The E3AP Python observer examples and the portable-E3 consumer round trip
# hard-depend on asn1tools (no apt package in Ubuntu 24.04). Without it every
# Python example suite deregisters at configure time and CI reports success
# having executed none of it.
python3 -m pip install --quiet --break-system-packages 'asn1tools>=0.166,<1'
