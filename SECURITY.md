<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Security policy

## Supported versions

Until `0.1.0` is tagged, only the current default branch receives security fixes. After release, supported versions will be listed here.

## Reporting a vulnerability

Report vulnerabilities through GitLab's private vulnerability reporting on the canonical repository (<https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/security/vulnerability_report>), or open a **confidential** issue at <https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/issues/new> if the security report form is unavailable to you. Include the affected commit, deployment class, reproduction steps, impact, and any proposed mitigation.

Maintainers acknowledge new reports within **5 business days** and aim to provide an assessment or fix plan within **30 days** of acknowledgement. Do not disclose the issue publicly until maintainers have coordinated a fix and advisory.

## Deployment boundary

The SDK is source code and reference tooling, not a security sandbox.

- Class A and B modules execute in the DU process and must be signed, operator-trusted native code. Admission, bounded inputs, watchdogs, circuit breakers, and fail-open conventional authority limit faults but do not make hostile native code safe.
- Class C modules may execute in-process when trusted or in a supervised process for stronger fault containment. The process boundary uses bounded shared-memory publications and authenticated control messages; it is not a multi-tenant hostile-code boundary.
- Portable E3 is for bounded, lower-rate control and result traffic. It is not a transport for hot-path tensors.

Operators retain responsibility for key custody, trust-policy rotation, least-privilege service accounts, package review, and host hardening. The complete model is in [docs/security_model.md](docs/security_model.md).
