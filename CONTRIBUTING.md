<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Contributing

Contributions should preserve the bounded, versioned host contract. Keep latency-critical Class A and B work native, allocation-free during invocation, and on the host-provided execution stream. Do not transport real-time tensors over E3.

Before opening a merge request:

1. Format changed C, C++, and CUDA files with **clang-format 18** (`git ls-files '*.h' '*.cpp' '*.cu' '*.cuh' | xargs clang-format-18 -i`). The tree is formatted with major 18 and CI enforces it; other clang-format majors disagree on alignment and parameter packing and will fail the gate.
2. Build and run the CPU suite with GCC and Clang where available.
3. Run the CUDA suite on supported hardware for accelerator changes.
4. Run ASan and UBSan for host/process changes.
5. Update public documentation, `CHANGELOG.md`, and compatibility notes when behavior changes.
6. Include test commands, platform details, and performance or OTA evidence appropriate to the changed dApp class.

Changes to installed headers, schemas, manifests, package configuration, security policy, or lifecycle behavior require maintainer review. New public fields must be append-only or introduced under a new versioned structure. See [API_STABILITY.md](API_STABILITY.md) and [docs/releasing.md](docs/releasing.md).

Report vulnerabilities through the private route described in [SECURITY.md](SECURITY.md), not a public issue.
