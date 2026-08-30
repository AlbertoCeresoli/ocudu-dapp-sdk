# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Shared helper: locate the ``ocudu_e3`` client and open an E3AP association.

The client ships with the OCUDU host tree (``python/ocudu_e3``) and with the
installed ``dapp_sdk`` component (``<prefix>/share/ocudu/dapp/python``). Set
``OCUDU_E3_PYTHON_DIR`` to point at either location, or install the package on
your ``PYTHONPATH``. The ASN.1 modules are found through ``OCUDU_E3_SCHEMA_DIR``
or the same install prefix.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path


def _add_client_to_path() -> None:
    candidates = []
    env = os.environ.get("OCUDU_E3_PYTHON_DIR")
    if env:
        candidates.append(Path(env))
    here = Path(__file__).resolve()
    # Sibling checkout layout: <workspace>/ocudu-dapp-sdk/examples/... next to <workspace>/ocudu
    candidates.append(here.parents[3] / "ocudu" / "python")
    for prefix in (Path(sys.prefix), Path("/usr/local"), Path("/usr"), Path("/opt/ocudu")):
        candidates.append(prefix / "share" / "ocudu" / "dapp" / "python")
    for candidate in candidates:
        if (candidate / "ocudu_e3").is_dir():
            if str(candidate) not in sys.path:
                sys.path.insert(0, str(candidate))
            return


_add_client_to_path()

try:
    import ocudu_e3  # noqa: E402
except ImportError as error:  # pragma: no cover - environment dependent
    raise SystemExit(
        "ocudu_e3 not found: set OCUDU_E3_PYTHON_DIR to <ocudu>/python or an installed "
        "share/ocudu/dapp/python directory, and install asn1tools"
    ) from error


def connect(host: str, port: int, name: str, timeout: float = 5.0) -> "ocudu_e3.E3Client":
    """Open the SCTP association; the caller runs ``setup()``."""
    return ocudu_e3.E3Client(host, port, dapp_name=name, dapp_version="0.1.0", vendor="example", timeout=timeout)
