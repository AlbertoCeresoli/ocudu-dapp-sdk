<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Compatibility and qualification matrix

`0.1.0` is a source-only toolkit. Consumers build against a matching installed OCUDU host SDK and rebuild for each compiler, CUDA toolkit, and GPU target.

| Platform | Required release evidence | Status |
|---|---|---|
| NVIDIA GB10 / aarch64 | GCC and Clang ABI checks, CPU/CUDA suite, sanitizers, install consumer, performance qualification, live OTA load/unload | Primary local qualification platform; final report pending |
| NVIDIA GH200 / aarch64 | Native build, CPU/CUDA correctness, sanitizer and performance report; OTA when hardware is available | External evidence pending |
| NVIDIA RTX 4090 / x86-64 | Native build, CPU/CUDA correctness, sanitizer and performance report; OTA when hardware is available | External evidence pending |

The ABI v1 LP64 fingerprint checks GCC and Clang structure size, alignment, offsets, and constants. It does not establish C++ standard-library ABI compatibility, 32-bit support, driver compatibility, or performance equivalence. CUDA packages are compiled for explicit architectures and must not silently fall back to CPU when a CUDA-only profile is selected.

A platform is "build supported" only after its native report passes. "OTA qualified" additionally requires stable attach, sustained load, dApp enable/disable, rollback/unload, and performance evidence on that hardware. See [validation_ladder.md](validation_ladder.md).
