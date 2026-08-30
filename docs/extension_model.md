<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Extending the reference dApps

The reference packages are working implementations and authoring templates. They are deliberately split into a stable integration shell and a replaceable algorithm core so another team can improve performance without recreating the dangerous parts of the host boundary.

## Keep these parts stable

- the versioned C ABI query trampoline and descriptor;
- lifecycle, per-instance and per-worker ownership, quiescence, and teardown;
- tensor bounds, memory-domain, device, stream, completeness, and profile validation;
- caller-owned output and bounded scratch behavior in the invocation path;
- exception/status translation, fallback, circuit-breaker, and deadline metrics;
- manifest, SBOM, signature, package configuration, model-management, and E3 surfaces;
- scheduler host-authority validation and Class C queue/lease limits.

These pieces may evolve through reviewed SDK releases, but an algorithm project normally links or copies the documented wrapper rather than modifying it.

## Replace or extend these parts

| Reference | Primary algorithm extension points |
|---|---|
| Channel estimator | pilot processing, interpolation, denoising, learned estimation, signal/noise estimation |
| Equalizer | channel use, MMSE/IRC/MIMO detection, residual-noise and SINR estimation |
| Receiver | joint CE/EQ/demapping, learned receiver stages, model artifacts and persistent GPU state |
| Scheduler | ranking, OLLA, MCS, allocation, TPC, sensing-aware policy within host allow flags |
| Spectrum | detector, features, classifier, occupancy confidence, accelerator implementation |
| SRS/ISAC | channel/CIR estimation, clutter rejection, detection, tracking, fusion, learned inference |

An implementation may also extend off-hot-path configuration and model schemas using their version and size rules. It must not reinterpret an existing field or bypass host authority.

## Performance rules

Class A and latency-critical Class B algorithms should remain native C/C++/CUDA, operate on host-provided buffers, preserve GPU residency, enqueue on the inherited stream, and avoid allocation or synchronization during invocation. A selected CUDA path must not silently execute the algorithm on CPU. Measure the whole integration boundary, including completion, rather than an isolated kernel alone.

Class C may use a supervised process when isolation is valuable. Producers remain non-blocking and bounded; overload drops lower-authority sensing work before any UL burst or scheduler-critical work. Portable E3 is suitable for bounded control and result traffic, not real-time tensor transport.

## Recommended workflow

1. Select the closest working reference and first run it unchanged.
2. Copy the package scaffold or create a thin repository linked to `OCUDUDapp`.
3. Replace one algorithm layer while retaining the wrapper and contract tests.
4. Add golden-vector, malformed-input, concurrency, deadline, and fallback tests.
5. Compare against the unmodified reference over representative channel/load conditions.
6. Package, sign, load in shadow when applicable, then enable live with rollback ready.

The class-specific authoring guides contain the exact source and target names. The [validation ladder](validation_ladder.md) defines what evidence is needed before an improved algorithm can be called production-qualified.
