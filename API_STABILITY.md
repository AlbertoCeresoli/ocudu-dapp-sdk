<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# API and compatibility policy

The toolkit release, public host ABI, and serialized schemas have independent versions:

| Surface | Current version | Compatibility rule |
|---|---:|---|
| `OCUDUDapp` toolkit package | 0.1.0 | Semantic Versioning; pre-1.0 source APIs may evolve with changelog and migration notes |
| OCUDU dApp C ABI | 1.0 | Existing v1 layout and meanings remain compatible; extensions use size/version checks or a new ABI major |
| manifest, management, process, and E3 schemas | v1 | Readers reject unknown incompatible majors and validate bounds before use |

Installed C++ reference helpers are a source-authoring convenience, not a cross-toolchain binary ABI. Rebuild dApps when changing compiler, standard library, CUDA toolkit, GPU architecture, or SDK release. The stable module boundary is the versioned C ABI supplied by the matching OCUDU host package.

Breaking changes require a new major or a parallel versioned entry point, a migration guide, updated ABI fingerprints, and a previous-release compatibility fixture. Deprecations must appear in the changelog for at least one release before removal unless required for security, and must be marked at the declaration with `OCUDU_DAPP_DEPRECATED("use <replacement>")` from [`include/ocudu_dapp_sdk/deprecated.h`](include/ocudu_dapp_sdk/deprecated.h) so consumers get a compiler diagnostic, not only prose.

See [docs/compatibility.md](docs/compatibility.md) for the supported platform matrix and qualification meaning.
