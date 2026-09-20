<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Optional receiver constellations

The CPU and CUDA reference receivers can populate the optional
`ocudu_dapp_receiver_metrics_v1.equalized_symbols` output. Extenders implementing
direct-bit receivers may leave it unfilled: valid LLRs do not require symbols.

Inspect this append-only field only when both metadata extent and `struct_size`
cover `OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_MIN_SIZE_V1`. A zero tensor
`struct_size` means absent. Older prefix-only requests remain supported.
The tensor is caller-owned CF32 `[data_re, layer]`, layout
`OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1`, version 1. Preserve the
selected RE/layer order, descriptor, and lease. Validate bounds, memory domain,
and nonoverlap before writing.

The reference implementations write the same unsliced symbols used in their
demapping operation and set `OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_VALID_V1`.
This bit is independent of `OCUDU_DAPP_RECEIVER_METRICS_VALID_V1`: exporting
symbols does not authorize new scheduler SINR estimates. CUDA exports are
fused into the existing receiver kernel, on the inherited stream; no extra
kernel, transfer, or allocation is required for the symbol output itself.

Tests in `reference_phy_algorithms_test.cpp` exercise legacy compatibility,
all-QAM LLR/noise invariance, unsliced outputs, canaries and malformed/aliased
requests. `reference_phy_algorithms_cuda_test.cu` adds all-QAM device output,
LLR invariance and optional/aliased-request checks. Running the CPU tests does
not qualify the device tests or OTA performance.

The public ABI compiler matrix checks the extension's flag values, minimum
extent and field offset with GCC and Clang in both C11 and C++17. On LP64 the
legacy prefix remains 336 bytes and the extended metadata is 632 bytes; all
existing field offsets and the parent receiver output size are unchanged.
Use matching updated OCUDU headers when building the SDK and running this gate.
