<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Tutorial: standing up a new vendor dApp repository

This walks a vendor from an empty git repository to a signed, certified dApp
package that an operator can load on a live gNB — building **against the
installed SDK only**, never against the OCUDU source tree. The running example
is a fictional vendor, *AcmeRF*, shipping a Class-A CUDA equalizer and a
Class-C spectrum sensor from their own repo.

Related reading: [`extension_model.md`](extension_model.md) (the host
boundary), [`vendor_toolkit.md`](vendor_toolkit.md) (CMake/packaging),
[`class_a_cuda_authoring.md`](class_a_cuda_authoring.md),
[`reference_packages.md`](reference_packages.md), and the end-to-end
[`run_examples.md`](run_examples.md).

## 0. Prerequisites — the installed SDK

A vendor consumes the SDK as an **installed CMake package**. Someone (the
vendor, or the operator handing them a prefix) installs OCUDU with the dApp
runtime and then the SDK:

```bash
# Host, once (produces the OCUDUDAppSDK package the SDK builds against)
cmake -S ocudu -B ocudu/build -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_DAPP=ON -DENABLE_E3=ON -DENABLE_CUDA=ON \
  -DCMAKE_INSTALL_PREFIX=$HOME/ocudu-install
cmake --build ocudu/build -j && cmake --install ocudu/build

# SDK, once (installs the OCUDUDapp package + tools the vendor links)
cmake -S ocudu-dapp-sdk -B ocudu-dapp-sdk/build \
  -DCMAKE_PREFIX_PATH=$HOME/ocudu-install \
  -DCMAKE_INSTALL_PREFIX=$HOME/ocudu-install
cmake --build ocudu-dapp-sdk/build -j
cmake --install ocudu-dapp-sdk/build
```

From here the vendor never needs the OCUDU or SDK source again — only
`$HOME/ocudu-install` (or wherever the operator publishes the toolkit).

## 1. Scaffold the first package

The fastest correct start is the scaffold generator, which emits a
**self-contained external project** for any of the three Class-A CUDA depths
(`ce`, `equalizer`, `receiver`) — already wired to build, certify, and
package:

```bash
mkdir acmerf-dapps && cd acmerf-dapps && git init
# ocudu-dapp-scaffold-class-a-cuda is installed to <prefix>/bin
ocudu-dapp-scaffold-class-a-cuda \
  --kind equalizer \
  --target acme_eq \
  --global-name com.acmerf.equalizer \
  --package-id "$(uuidgen)" \
  --vendor "AcmeRF, Inc." \
  --version 1.0.0 \
  --output packages/acme_eq
```

That produces a package directory holding exactly the four files a dApp is
made of (see [`extension_model.md`](extension_model.md)):

| File | Role — **what you edit vs. keep** |
|------|-----------------------------------|
| `module.cpp` | Pure `ocudu_dapp_query_v1` descriptor trampoline. **Keep** — it holds no state (the host queries it in a disposable preflight process). |
| `module_support.cpp` | Instantiates `stateful_module_context<Shared,Worker>` and the ABI-guarded `invoke`. **Keep** the glue; it routes your algorithm through `guard_algorithm_call` so exceptions never cross the C ABI. |
| `management.cpp` | Your typed configuration schema (product knobs). **Edit** to declare your keys. |
| `algorithm.cu` | Your product IP — the kernels behind the reference's admission profile. **Replace** this. |

The scaffold's CMake uses one `find_package(OCUDUDAppSDK)` — the host's installed ABI package
(distinct from the SDK's own `OCUDUDapp` toolkit package); there is no OCUDU
source dependency and no fork.

> Building a **Class-B scheduler** or **Class-C sensing** package instead?
> The CPU scaffold works the same way, with no CUDA toolchain:
> `ocudu-dapp-scaffold-class-bc-cpu --kind scheduler|spectrum|srs-isac ...`

## 2. Replace the algorithm, keep the contract

Open `algorithm.cu` and replace the reference kernel with yours. The contract
you must honor (all enforced by certification in step 4):

- **Validate shape against your admission profile, then bypass** anything you
  don't handle — return `OCUDU_DAPP_BYPASS_V1`, never crash. An unhandled
  shape becoming a bypass (host falls back to conventional) instead of a fault
  is the whole safety model.
- **Enqueue on the host's inherited CUDA stream and return** — allocate
  nothing, synchronize nothing on the invocation path.
- **Write only caller-owned outputs** at the given offsets; honor the
  completeness flags (partial data-RE selection / single-layer pilot plane
  must not be silently treated as complete).

For persistent state (weights, tables, per-worker scratch/history), use the
`SharedState` / `WorkerState` types the scaffold already declares — instance
state is visible to every worker; worker state is lane-local and needs no
lock. If your algorithm carries a neural model, the receiver reference
demonstrates the full model-slot machinery (stage / validate / warm /
activate / rollback); copy that pattern.

Add a second package the same way — e.g. a Class-C spectrum sensor by copying
`examples/vendor_min` or a `ref_spectrum*` reference — so the repo ships two
capabilities:

```
acmerf-dapps/
├── CMakeLists.txt            # top-level: find_package(OCUDUDapp); add_subdirectory each package
├── packages/
│   ├── acme_eq/              # Class A CUDA equalizer
│   └── acme_spectrum/        # Class C spectrum sensor
├── .gitlab-ci.yml            # build + certify + reuse/spdx gates (copy the SDK's shape)
└── README.md
```

## 3. Build in your own repo

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=$HOME/ocudu-install \
  -DCMAKE_CUDA_ARCHITECTURES=121   # GB10; set your target arch
cmake --build build -j
```

The build emits, per package, the shared object plus a `*.manifest.json` and
`*.spdx.json` (the SBOM). Your release cadence is now **fully decoupled** from
the host's — the frozen, fingerprint-verified C ABI is the only coupling
point, and `API_STABILITY.md` states what that guarantees.

## 4. Certify offline — before anyone else sees it

Certification is the vendor's proof-to-ship. It loads the final DSO through
the public ABI and drives deterministic vectors (off-origin placements,
rank-deficient two-layer pilots, poisoned output canaries, the full model
lifecycle) — converting "did you read the docs" into "did you pass the
vectors":

```bash
# certify_class_a_cuda ships with the SDK build (it is not part of your repo)
<sdk-build>/certify_class_a_cuda build/packages/acme_eq/acme_eq.so 0x00010004
```

Wire this into your CI exactly as the SDK does (ABI compiler-matrix
fingerprint, sanitizers, REUSE/SPDX, clean-prefix install-consumer). A
scaffolded project starts CI-green, so a red pipeline means *you* changed
something.

## 5. Sign and hand off

> **Just trying it out? Skip this section.** Signing and trust policies are a
> **production** concern, not a prerequisite for a first run. In a lab the
> operator sets `package_signature_mode: allow_unsigned` and loads your
> `.so` + `.manifest.json` + `.spdx.json` directly — **no keys at all**. Build
> (step 3), certify (step 4), hand the three files over, and jump to the
> [install tutorial's lab quickstart](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/tutorial_install_and_yaml.md#0-lab-quickstart-no-keys).
> Come back here when you're ready to ship for real.

Distribution is separate from admission (see the
[install tutorial](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/tutorial_install_and_yaml.md)). For
production, the vendor produces an immutable, hash-bound bundle; the
**operator** owns the trust policy and does the admission. Using the host
tool:

```bash
# Vendor generates a signing key once, keeps the private key offline
ocudu-dapp-package keygen --private-key acme.key --public-key acme.pub

# Sign each package (signs a digest over manifest + artifact-hash + SBOM-hash)
ocudu-dapp-package sign \
  --catalog-root build/packages/acme_eq \
  --manifest acme_eq.manifest.json \
  --private-key acme.key \
  --package-sequence 1
```

Ship the operator the bundle (`.so` + `.manifest.json` + `.spdx.json` +
`.sig.json`) and your **public key** (`acme.pub`) out of band. The operator
adds your key to their trust policy with a per-package sequence floor, drops
the bundle in their catalog, and loads it — no source, no rebuild, no shared
CI.

> The full production trust workflow — what the signature covers, the SBOM,
> trust policies, rotation/revocation, and key custody — is its own guide:
> **[Signing, SBOMs, and key management](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/tutorial_signing_and_trust.md)**.

## Where each responsibility lives

| Vendor (this repo) | Operator (the gNB) |
|--------------------|--------------------|
| Author, build, certify, sign | Hold trust policy, admit, load |
| Own release cadence & CI | Own catalog & YAML |
| Publish public key + bundle | Pin key + sequence floor, verify SBOM |

Next: [installing and loading over YAML](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/tutorial_install_and_yaml.md),
then [operating with `dappctl`](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/tutorial_dappctl_operations.md).
