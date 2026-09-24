<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Test data

## `srs_channel_estimator_vectors.bin`

Golden vectors for `OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1`. Each case is
an SRS occasion synthesised on an OCUDU resource grid (flat channel per port
pair, a common propagation delay, white noise) and passed through the gNB's
Class-A SRS adapter, whose target records the module input and declines; the
adapter then falls back to the gNB's host estimator (`srs_estimator_generic_impl`).
A case therefore pairs the exact tensors a module receives with the host's
channel matrix, noise variance, EPRE, RSRP and time alignment.

The 26 cases cover one, two and four transmit ports (shared and interleaved
pilot sets), one to eight receive ports, one, two and four symbols, combs 2 and
4, 15 and 30 kHz, sequence lengths from 12 to 1632 pilots, -5 to 45 dB SNR and
delays up to the edge of the search window.

Regenerate from an OCUDU build tree (tests enabled, `ENABLE_DAPP=ON`):

```bash
make srs_dapp_channel_estimator_vectors
./tests/unittests/phy/upper/srs_dapp_channel_estimator_vectors srs_channel_estimator_vectors.bin
```

Layout (little endian): `"OSRSCEV1"`, `u32` version (1), `u32` case count, then
per case a `u32` metadata size, the `ocudu_dapp_srs_channel_estimator_metadata_v1`
bytes, `rx_symbols` (CF32 `[pilot_set, rx_port, symbol, pilot]`),
`reference_symbols` (CF32 `[tx_port, pilot]`), the host channel matrix (CF32
`[rx_port, tx_port]`), the host measurements (F32, NaN when absent) and the host
time alignment in double precision (value, resolution, min, max).
`include/ocudu_dapp_sdk/srs_channel_estimator_vectors.h` reads it.
