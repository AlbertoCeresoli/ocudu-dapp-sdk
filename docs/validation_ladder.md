<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# dApp validation ladder (single product story)

All open and commercial packages share one ladder. Prefer the unified runner
over ad-hoc environment variables.

```text
packaging  →  algorithm  →  system
   │              │            │
   │              │            └─ dummy-RU matrix / gNB lifecycle
   │              └─ L1 suite (SER/MSE, SNR, TB-CRC) via dlopen
   └─ certify_class_a / module contract
```

## Unified entrypoint

```bash
# 1) Copy and edit paths (JSON preferred; YAML needs PyYAML)
cp tools/lab_profile.example.json /tmp/lab.json
# edit sdk_build + package_dir

# 2) Run (packaging + algorithm by default)
python3 tools/dapp_validate.py --profile /tmp/lab.json

# 3) Selected tiers
python3 tools/dapp_validate.py --profile /tmp/lab.json --tiers packaging,algorithm
```

Report JSON: `<package_dir>/reports/dapp-validate.json`.

## What each tier proves

| Tier | Tool | Proves |
|------|------|--------|
| **packaging** | `certify_class_a_cuda` | ABI lifecycle, caller-owned outs, ideal goldens/canaries |
| **algorithm** | `l1_validation_suite` | Noisy simulated PUSCH SER/MSE, chain, SNR, uncoded CRC24A |
| **system** | dummy-RU matrix / gNB tests | Live load/activate/traffic metrics under `ru_dummy` |

### Class-A shapes and host feedback (what authors should exercise)

| Concern | Offline / packaging | System / live |
|---------|---------------------|---------------|
| Post-EQ noise quality | Goldens + L1 suite SER/MSE | Scheduler `pusch_snr_db` present under UL with equalizer active |
| DM-RS type 1/2 | Certificate matrix | Live grants |
| Multi-symbol DMRS (e.g. two or three DMRS symbols in one grant) | Extend offline matrix / side-by-side benches | Host may set `dmrs.reserved[0]` mask; packages may ignore |
| Concurrent workers | Cert multi-stream | Soft deadline vs breaker metrics under multi-lane UL |
| Soft wall budgets | Benchmark `-D` / host test overrides | Default ~2000 µs demod / ~300 µs CE on host; 8 consecutive recoverable misses to latch |

Vendor-only (not in open ladder): a proprietary neural receiver vs the
conventional receiver, side-by-side SER.

## Vendor repos

Commercial trees should **not** fork the ladder. Point `paths.package_dir` at
the vendor build and `paths.sdk_build` at this SDK build:

```yaml
paths:
  sdk_build: /path/to/ocudu-dapp-sdk/build-cuda
  package_dir: /path/to/your-vendor-repo/build
environment:
  ACME_NRX_ENABLE: "0"
  ACME_NRX_WEIGHTS: "${package_dir}/weights/default.bin"
packaging:
  certify:
    - [acme_neural_eq_cuda.so, "0x00010004"]
    - [acme_neural_rx_cuda.so, "0x00010005"]
algorithm:
  eq_so: acme_neural_eq_cuda.so
  rx_so: acme_neural_rx_cuda.so
```

The `environment:` mapping (and, equivalently, a `vendor_env.json` flat map
shipped in `package_dir`) is applied to every certifier / suite invocation;
`${package_dir}` in a value expands to the package directory. This is how a
closed package finds its weights without the open ladder naming them.

See also [`vendor_toolkit.md`](vendor_toolkit.md) and
[`tools/l1_validation/README.md`](../tools/l1_validation/README.md).
