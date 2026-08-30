# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Import Class-C process publications as PyTorch tensors.

Sensing in this example is always **torch** (complex64). Transport may deliver:

* **Host SHMEM** — often CBF16 from CUDA low-PHY; the C++ extension densifies to
  CF32, then we take a private ``torch`` clone and release the SHMEM lease.
* **CUDA-IPC** — optional zero-copy device view via DLPack (``kDLCUDA``).

Prefer ``copy=True`` (default) so the supervised ring can advance while you run
your model. Use ``copy=False`` only for short zero-copy peeks that keep the
lease alive until you call ``handle.release()``.

This bridge is for **Class-C out-of-process** workers only (see package README).
"""

from __future__ import annotations

from typing import Any

import torch
from torch.utils.dlpack import from_dlpack


def publication_to_torch(handle: Any, *, copy: bool = True) -> tuple[torch.Tensor, Any]:
    """Convert a process ``PublicationHandle`` to a torch complex tensor + meta.

    Parameters
    ----------
    handle:
        Live ``ocudu_dapp_class_c_process.PublicationHandle`` (lease held).
    copy:
        If True, return an owned tensor and release the lease. If False, the
        tensor may view SHMEM/CUDA-IPC memory — do not release until finished.

    Returns
    -------
    tensor, meta
        ``tensor`` is ``torch.complex64`` on CPU or CUDA. ``meta`` is the
        extension's ``PublicationMeta`` (kind, cell, sequence, radio time, …).
    """
    meta = handle.meta()
    memory = str(getattr(meta, "memory", "") or "")

    if memory.startswith("cuda_ipc"):
        tensor = from_dlpack(handle.to_dlpack())
        if not torch.is_complex(tensor):
            raise TypeError(f"expected complex DLPack tensor, got {tensor.dtype}")
        if copy:
            tensor = tensor.detach().clone()
            handle.release()
        return tensor, meta

    # Host SHM: C++ densifies (CBF16→CF32 when needed) into a contiguous buffer.
    host = handle.numpy_grid()
    tensor = torch.as_tensor(host).to(dtype=torch.complex64).contiguous().clone()
    if copy:
        handle.release()
    return tensor, meta
