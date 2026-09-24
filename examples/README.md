<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Examples

| Directory | Kind | What it shows |
|-----------|------|---------------|
| [`vendor_min/`](vendor_min/README.md) | Catalog package (Class-A CUDA MODULE) | Copy-me commercial vendor layout: `find_package(OCUDUDapp)`, thin algorithm, `package_config` |
| [`class_c_e3_python/`](class_c_e3_python/README.md) | Catalog packages + portable E3 client (Python) | Class-C out-of-process sensing in Python/PyTorch (research only) |
| [`e3_telemetry_python/`](e3_telemetry_python/README.md) | Independent E3AP clients (Python) | Six observer scripts: catalog, PUSCH quality, SRS watch, dApp health, full-rate HDF5/npz recording, PUSCH burst export |
| [`e3_spectrum_sensing/`](e3_spectrum_sensing/README.md) | Independent E3AP client (Python) | `emitter_detector.py`: emitter detection on the `classc.spectrum` uplink resource grid |
| [`e3_isac_sensing/`](e3_isac_sensing/README.md) | Independent E3AP client (Python) | `srs_isac_monitor.py`: SRS range-Doppler starting point (explicitly not a sensing capability) |
| [`e3_receiver_routing/`](e3_receiver_routing/README.md) | Independent E3AP client (Python) | `sinr_router.py`: routes each UE's PUSCH to one of two receivers (or the stock receiver) by its uplink SINR, through the gNB's per-UE route table |
| [`e3_srs_dataset/`](e3_srs_dataset/README.md) | Independent E3AP client (Python) | `capture_srs_dataset.py`: SRS channel-estimation datasets from a live cell — the SRS estimator dApp input with the gNB's answer as label — and their replay through the reference estimator |
| [`e3_mcp_server/`](e3_mcp_server/README.md) | Independent E3AP client (Python) | `ocudu_mcp_server.py`: MCP server exposing the gNB to an AI assistant over one E3 association |

The catalog packages (`vendor_min`, `class_c_e3_python` process workers) go
through staging, DU YAML and the E3 lifecycle; the `e3_*` directories are plain
processes that attach to the gNB's E3AP SCTP endpoint. Enable/run recipes for
everything: [`docs/run_examples.md`](../docs/run_examples.md) (Python E3
examples in §6).
