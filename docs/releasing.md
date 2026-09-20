<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Release procedure

1. Pin the compatible OCUDU host commit and verify ABI/schema versions.
2. Run documentation, SPDX/REUSE, formatting (clang-format 18), GCC/Clang ABI, CPU, CUDA, ASan, UBSan, package-integrity, and clean-install consumer gates. The CUDA gate requires a configured CUDA runner (`OCUDU_DAPP_CUDA_RUNNER_TAG` / `OCUDU_CUDA_ARCHITECTURES` CI variables); the pipeline's `cuda release unavailable` placeholder failing-by-design marks the gap when it did not run — a tag with that placeholder red and no manual CUDA evidence is not releasable.
   The host repository's `docs/dapp/release_checklist.md` additionally gates on Doxygen warnings, schema generation, and parser mutation/fuzz targets; run those from the host checkout against the pinned commit — they cover shared E3/schema surface this SDK consumes.
3. Run Class A deadline/correctness qualification and Class B policy/timing qualification on the primary platform.
4. Run native correctness/performance reports on each supported secondary platform; record unavailable OTA environments explicitly.
5. Run live OTA attach, traffic, enable/disable, rollback, unload, process-death, and overload tests without degrading the conventional baseline.
6. Review security boundaries, known limitations, changelog, and migration notes.
7. Build only from a clean commit. This release publishes source and documentation, not prebuilt binaries.
8. Create a simple signed or protected `v0.1.0` tag on the default branch and attach immutable test/qualification reports to the release record.

Do not tag while a required gate is failing. A waived gate must identify the owner, rationale, risk, and follow-up issue.

The mandatory `cross repository interoperability` job also builds the paired
host with real libe3 interoperability enabled and uses its fail-closed test
inventory gate for both repositories. The paired host must contain
`scripts/check_dapp_interop_gate.py`; update `.gitlab/ci/ocudu-host.lock` to the
reviewed, published host commit, or provide `OCUDU_HOST_REF` explicitly. A missing
host implementation/pin is a failing prerequisite, not a skipped success.

The reference scheduler keeps fixed per-worker scratch for up to the public
8192-candidate bound and 275 PRBs; evaluation and sorting allocate/free nothing.
The CPU test instruments allocation at the maximum candidate count. This does
not replace Class-B deadline qualification under real scheduler load.
