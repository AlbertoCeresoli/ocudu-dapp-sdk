# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Python Class-C sensing examples (supervised process + portable E3).

**Placement:** Python is supported only for **Class-C out-of-process** catalog
packages (and independent portable-E3 clients). Class-A/B and Class-C
**in-process** MODULEs remain C/C++/CUDA only.

**Audience:** research, education, and algorithm prototyping. Prefer native
``ref_spectrumd_*`` / ``ref_srs_isacd_*`` (or your own C++/CUDA packages) for
production RAN performance.

Public algorithms (PyTorch):

* ``radiometer`` / ``SensingResult`` — spectrum PRB occupancy demo
* ``estimate_cir`` / ``CirResult`` — SRS early-delay CIR placeholder (not full ISAC)
* ``select_device`` — CUDA if available, else CPU (override with explicit device)

Transport helpers live in ``torch_bridge`` and ``process_loop``; catalog
entrypoints are ``spectrum_worker`` and ``srs_isac_worker``.
"""

from .spectrum_sensing import SensingResult, radiometer, select_device, synthetic_spectrum_grid
from .srs_cir import CirResult, estimate_cir, synthetic_srs_channel

__all__ = [
    "CirResult",
    "SensingResult",
    "estimate_cir",
    "radiometer",
    "select_device",
    "synthetic_spectrum_grid",
    "synthetic_srs_channel",
]
