<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Class-A L1 validation suite (centralized in ocudu-dapp-sdk)

Offline GPU tests for **any** Class-A equalizer/receiver package loaded via
`dlopen` (open `ref_*_cuda` or vendor commercial packages). No live gNB.

Binary: `l1_validation_suite`

## Why it lives here

- **Single implementation** shared by the open reference SDK and vendor repos
- Vendors only point at their `.so` paths (and optional weights env)
- Does **not** link proprietary algorithm code — exercises packages through the
  public ABI (same isolation model as `certify_class_a_cuda`)

## Tiers

| Mode | What | Typical gate |
|------|------|----------------|
| **A `golden`** | Fixed-seed simulated PUSCH → EQ SER/MSE + FNV checksum | SER≈0, tight MSE @ high SNR |
| **B `chain`** | EQ package + RX package (CE+EQ+demap) LLR hard BER | QPSK BER=0 @ high SNR |
| **C `sensitivity`** | SNR SER sweep | QPSK SER@15dB &lt; 1e−2; 16QAM@20dB &lt; 5e−2 |
| **D `tb`** | Uncoded payload + CRC24A through EQ hard path | 100% CRC @ 30 dB |
| **`all`** | A+B+C+D | all pass |

## Usage

```bash
# Open reference packages (SDK ctest wires these paths)
./build-cuda/l1_validation_suite --mode all --quick \
  --eq-so build-cuda/ref_equalizer_cuda.so \
  --rx-so build-cuda/ref_receiver_cuda.so

# Vendor commercial packages (from vendor repo ctest). Export whatever
# environment your closed package documents (e.g. a weights path) first.
./build-cuda/l1_validation_suite --mode all --quick \
  --eq-so /path/to/acme_neural_eq_cuda.so \
  --rx-so /path/to/acme_neural_rx_cuda.so
```

Environment fallbacks:

| Variable | Purpose |
|----------|---------|
| `OCUDU_L1_EQ_SO` | Default `--eq-so` |
| `OCUDU_L1_RX_SO` | Default `--rx-so` |

## Relation to other gates

| Gate | Role |
|------|------|
| `certify_class_a_cuda` | ABI packaging + ideal-gain goldens (no noise SER curves) |
| **This suite** | Noisy RX sim, SER/MSE, sensitivity, TB-CRC via public ABI |
| Vendor-only benches | e.g. a proprietary neural receiver vs conventional, side by side |
| Dummy-RU / OTA | Live lifecycle under traffic |

## CTest (SDK)

```bash
ctest --test-dir build-cuda -R '^l1\.' --output-on-failure
```
