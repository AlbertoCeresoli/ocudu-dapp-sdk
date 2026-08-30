<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Python Class-C dApps (supervised process + portable E3)

This example shows how to write **Class-C sensing dApps in Python** against the
OCUDU dApp SDK. It is intended as a **clear, copyable reference** for research
and education — not as a production RAN performance path.

Two **catalog** packages share one pybind transport and use **PyTorch** for
sensing math:

| dApp | Interface | Catalog binary | Algorithm |
|------|-----------|----------------|-----------|
| Spectrum sensing | `0x00010001` | `class_c_python_spectrumd` | PRB radiometer (`sensing/spectrum_sensing.py`) |
| SRS / UE-TRP CIR | `0x00010002` | `class_c_python_srs_isacd` | Early-delay CIR preview (`sensing/srs_cir.py`) — **not** full ISAC |

A separate **portable E3** CLI (`sensing/app.py --mode live`) consumes low-rate
host snapshots without joining the catalog process transport.

---

## Hard constraints (read first)

### 1. Python is only for Class-C **out-of-process**

| Placement | Python? | Why |
|-----------|---------|-----|
| Class **A** / **B** in-process MODULE | **No** | Resident L1 / RT control; MODULE ABI is C/C++/CUDA only |
| Class **C** in-process MODULE | **No** | Same native MODULE contract |
| Class **C** **out_of_process** (supervised process) | **Yes** | Separate OS process; catalog ELF may exec a Python worker |
| Portable E3 independent client | **Yes** | Not a catalog MODULE; off the hot path |

There is **no** supported path for embedding CPython inside the DU as a Class-A
or Class-B MODULE. If you need production L1 or scheduler logic, implement a
native package.

### 2. Research / education only — not production performance

Python + PyTorch process dApps are excellent for:

* Prototyping Class-C data paths and result ABI
* Offline algorithm development with unit tests
* Lab demos and ISAC / spectrum research notebooks

They are **not** appropriate for production RAN performance:

* Interpreter + GIL overhead on every publication
* Host densify (e.g. CBF16 → CF32) and copies into torch
* Heartbeat latency coupled to Python work between `wait()` calls
* Memory and startup cost of a full scientific stack

**Production Class-C sensing** should use the native C++/CUDA packages:

* `ref_spectrumd_cpu` / `ref_spectrumd_cuda`
* `ref_srs_isacd_cpu` / `ref_srs_isacd_cuda`

(or your own native packages built the same way). Keep this example as the
algorithm sandbox; ship native artifacts to the catalog.

---

## Architecture

```text
DU catalog (placement = out_of_process)
  │  posix_spawn(class_c_python_*d)  with stripped env (PATH/LANG)
  │  --control-fd=N
  ▼
class_c_python_launcher.cpp
  │  PR_SET_NO_NEW_PRIVS
  │  execve(absolute python, worker.py, baked PYTHONPATH / LD_LIBRARY_PATH)
  ▼
spectrum_worker.py  or  srs_isac_worker.py
  │  ProcessWorker(control_fd, interface_id)   # route admission + heartbeats
  │  wait() → try_acquire() → publication_to_torch() → radiometer / estimate_cir
  │  optional send_*_result(route_index, ...)
  ▼
ocudu_dapp_class_c_process.so  (pybind)
  ├─ CPU SHMEM ring  → densify CF32 → torch complex64
  └─ CUDA-IPC pool   → DLPack kDLCUDA → torch CUDA (optional)
```

Portable E3 path (not supervised SHMEM):

```text
DU E3 socket  →  ocudu_dapp_class_c_e3.so  →  app.py --mode live  →  radiometer
```

---

## Layout

```text
examples/class_c_e3_python/
  CMakeLists.txt                 # extensions + launchers + manifests + tests
  requirements.txt               # torch, numpy, pybind11
  bindings/
    process_module.cpp           # ProcessWorker, DLPack, densify, result send
    module.cpp                   # portable E3 consumer
    dlpack.h
  launcher/
    class_c_python_launcher.cpp  # shared catalog ELF (spectrum + SRS targets)
  sensing/
    spectrum_sensing.py          # pure torch radiometer
    srs_cir.py                   # pure torch CIR preview
    torch_bridge.py              # publication → torch tensor
    process_loop.py              # shared supervised loop (heartbeats, results)
    spectrum_worker.py           # catalog entry: spectrum
    srs_isac_worker.py           # catalog entry: SRS CIR
    app.py                       # offline + portable E3 CLI
  tests/
    test_spectrum_sensing.py
    test_srs_cir.py
```

---

## Build

Requires: installed host `OCUDUDAppSDK` with process client (and optional CUDA
process client), Python 3 development headers, `pybind11`, and PyTorch.

```bash
# venv with torch + pybind11 (see requirements.txt)
python3 -m venv .venv && . .venv/bin/activate
pip install -r examples/class_c_e3_python/requirements.txt

cmake -S . -B build-cuda \
  -DCMAKE_PREFIX_PATH=/path/to/host-dapp-sdk \
  -DOCUDU_DAPP_BUILD_PYTHON_CLASS_C=ON \
  -DCMAKE_CUDA_ARCHITECTURES=native \
  -DPython3_EXECUTABLE=$PWD/.venv/bin/python

cmake --build build-cuda -j --target \
  ocudu_dapp_class_c_process \
  class_c_python_spectrumd \
  class_c_python_srs_isacd \
  ocudu_dapp_class_c_e3
```

Artifacts land in the build tree:

| Artifact | Role |
|----------|------|
| `ocudu_dapp_class_c_process*.so` | pybind process transport |
| `class_c_python_spectrumd` + `.manifest.json` | spectrum catalog package |
| `class_c_python_srs_isacd` + `.manifest.json` | SRS CIR catalog package |
| `ocudu_dapp_class_c_e3*.so` | portable E3 (if E3 consumer target exists) |

The launcher embeds absolute `Python3_EXECUTABLE`, `PYTHONPATH` (build dir),
`LD_LIBRARY_PATH` (torch / CUDA libs), worker script path, and a default log
file path (build-tree `runtime_logs/` by default). Lab trees can bake absolute
log paths at configure time:

```bash
cmake ... \
  -DOCUDU_DAPP_PYTHON_SPECTRUM_LOG=/path/runtime_logs/python_spectrum.jsonl \
  -DOCUDU_DAPP_PYTHON_SRS_LOG=/path/runtime_logs/python_srs_isac.jsonl
```

Override at runtime (when the spawn environment is not stripped) with:

| Environment | Purpose |
|-------------|---------|
| `OCUDU_DAPP_PYTHON` | Absolute Python interpreter |
| `OCUDU_DAPP_PYTHONPATH` | Import path for the pybind module |
| `OCUDU_DAPP_PYTHON_PROCESS_WORKER` | Worker script path |
| `OCUDU_DAPP_LOG_FILE` | JSONL event log (publications) |
| `LD_LIBRARY_PATH` | Shared libs for torch / CUDA |

---

## Offline algorithms (no DU)

```bash
cd examples/class_c_e3_python
python3 sensing/app.py --mode offline --device cpu
python3 -m unittest discover -s tests -v
```

Algorithms are pure PyTorch. NumPy is used only for:

* optional offline grid coercion in demos, and
* packing result buffers for the C++ process ABI (`send_*_result`).

---

## Supervised catalog (live DU)

Full cookbook (YAML + E3 + package UUIDs for **all** SDK examples):
[`docs/run_examples.md`](../../docs/run_examples.md) §5.

| Package | UUID | load |
|---------|------|------|
| `class_c_python_spectrumd` | `8f0d0001-4f9c-4e31-8c00-00000000e101` | out-of-process / cpu |
| `class_c_python_srs_isacd` | `8f0d0001-4f9c-4e31-8c00-00000000e102` | out-of-process / cpu |

1. Stage each binary **plus** matching `.manifest.json` / `.spdx.json` into the
   operator catalog.
2. List them under DU YAML `package_manifests` (Class-C **out_of_process**).
3. Ensure the baked Python/venv can import `ocudu_dapp_class_c_process` and
   `torch` (launcher embeds paths at build time).
4. Enable over E3:

```bash
DAPPCTL=(python3 /path/to/ocudu/apps/tools/dappctl/ocudu_dappctl.py
  --socket /run/ocudu/dapp-management.sock
  --schema /path/to/ocudu/schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1)

# Spectrum
"${DAPPCTL[@]}" load --package 8f0d0001-4f9c-4e31-8c00-00000000e101 \
  --placement out-of-process --backend cpu
# note instance + generation, then:
for action in prepare arm shadow activate; do
  "${DAPPCTL[@]}" lifecycle "$action" --instance "$INSTANCE" \
    --expected-generation "$GEN" --timeout-us 5000000
done

# SRS CIR (same pattern with …e102)
```

Or use `initial_instances` with `target: active` (see run_examples.md).

### Operational rules (learn these)

1. **Construct `ProcessWorker` first** — host admission waits for
   `PR_SET_NO_NEW_PRIVS` and route accept.
2. **Heartbeats only run inside `ProcessWorker.wait()`** — keep torch work short;
   drain a small batch then return to `wait()`.
3. **Never flood stdout** with per-publication JSON. Default is log-file only;
   lifecycle lines may go to stdout. A non-drained pipe blocks the worker →
   heartbeat SIGKILL.
4. **Results must use `handle.route_index()`** — always the publication's route,
   never a hard-coded `0` when multiple routes exist. Consecutive host rejects
   can kill the worker.
5. **Spectrum defaults to observer-only** (sense + JSONL). Set
   `OCUDU_DAPP_SPECTRUM_SEND_RESULTS=1` only when you intentionally want host
   map results and understand authority / rejection policy.
6. Prefer **`copy=True`** in `publication_to_torch` so the SHMEM ring advances
   during model execution.

Worker CLI flags: `--control-fd` (required), `--device`, `--observer-only`,
`--log-file`, `--json-log`, `--wait-ms`, `--threshold` (spectrum).

---

## Algorithms (intentionally simple)

**Spectrum** — PRB-average \(|z|^2\) occupancy (demo radiometer). Replace
`radiometer()`; keep the complex grid contract `[rx, symbols, subcarriers]`.

**SRS CIR** — IDFT-style early-delay power along frequency, peak bin, noise
correction. Placeholder for UE-TRP style sensing — **not** tracking,
association, or full ISAC. Replace `estimate_cir()`; keep
`send_srs_cir_result` shape if you return results to the host.

---

## Related documentation

| Doc | Content |
|-----|---------|
| [`docs/run_examples.md`](../../docs/run_examples.md) | **Enable & run every SDK example dApp** |
| [`docs/class_c_cuda_process_authoring.md`](../../docs/class_c_cuda_process_authoring.md) | Native + process Class-C authoring |
| [`docs/reference_packages.md`](../../docs/reference_packages.md) | Full package inventory |
| [`docs/observer_authority.md`](../../docs/observer_authority.md) | Spectrum observer vs authority |
| [`docs/tools.md`](../../docs/tools.md) | Example index + tools |
| Native process references | `reference/process/` (`ref_spectrumd_*`, `ref_srs_isacd_*`) |
