<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Security and failure-containment model

OCUDU dApps are admitted packages operating at different latency and authority levels. Security controls must match the class instead of pretending every module is untrusted bytecode.

## Trust and execution

| Class | Intended work | Execution | Required trust posture |
|---|---|---|---|
| A | PUSCH CE, EQ, receiver replacement | In-process CPU or CUDA | Signed and operator-trusted native package; strict profile admission; conventional fallback and circuit breaker |
| B | Scheduler intent and bounded policy | In-process native; host records with optional CUDA internals | Signed and operator-trusted native package; host remains final authority and validates every intent |
| C | Spectrum, SRS/ISAC, analytics | Trusted in-process or supervised process | Signed package; bounded queues/publications; process mode preferred when fault isolation outweighs copy/control cost |

An in-process signature establishes provenance and operator approval; it does not sandbox native code. A malicious or memory-unsafe Class A/B module can compromise the DU. Review source or vendor provenance, pin hashes, protect signing keys, and deploy only approved packages.

## Data and control boundaries

- Hot tensors remain in native memory or GPU-resident buffers. Their lifetime, memory domain, device, dimensions, and stream are host-owned and validated before dispatch.
- Producers use bounded, non-blocking admission. Consumers cannot create unbounded work or backpressure in a real-time producer.
- The Class C process interface uses fixed bounds, leases, epochs, sequence checks, parent-lifetime binding, restricted syscalls, and authenticated management/control records.
- E3 carries management, inventory, bounded observations, and results. It is not an acceptable Class A/B tensor transport.
- The scheduler dApp emits bounded intent. The host validates permissions, radio limits, freshness, and feasibility before applying it.

## Failure behavior

Class A/B failures return conventional authority to the host, increment attributable metrics, and may open a per-worker circuit breaker. Late CUDA completion is distinct from immediate fallback and is recorded separately. Class C overload drops or replaces lower-authority sensing work before it can affect UL grants or real-time processing.

## Operator checklist

1. Admit only hash-bound, signed packages under a monotonic trust policy.
2. Use separate non-root service identities and restrict filesystem/device access.
3. Keep core dumps controlled and protected because they may contain radio and subscriber data.
4. Monitor rejection, timeout, exception, completion-deadline, queue-drop, and worker-restart metrics.
5. Exercise unload, rollback, process death, malformed input, and overload before production.
