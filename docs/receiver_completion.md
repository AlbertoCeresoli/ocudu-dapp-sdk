<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Optional asynchronous full-receiver completion

Use `ocudu_dapp_sdk/receiver_completion.h` when a resident receiver needs to
validate asynchronous device output before the host accepts it. Ordinary
reference receivers do not automatically opt into this extension.

The C contract lives in the host SDK's
`ocudu/dapp/use_cases/v1/receiver_completion.h`. It adds a separate exported
query; it does not change existing receiver tables or use their reserved slots.

## Authoring checklist

- In `ocudu_dapp_query_v1`, call `host_supports_receiver_completion(host)` and
  reject unsupported hosts **before** returning a completion-required module.
- Advertise the receiver completion and resident-accelerator capabilities on
  the full-receiver descriptor. Keep the original invoke-only entry present
  but return UNSUPPORTED without submitting device work.
- Export `ocudu_dapp_query_receiver_completion_v1`, using
  `populate_receiver_completion` to validate/copy the separate table.
- Use the same worker context for invoke, inspect, release and retire. Keep
  receipt storage bounded; do not put pointers to host C++ objects in it.
- Invoke publishes a nonzero opaque receipt on success. A partial submission
  is DEVICE_ERROR, never BYPASS/RESOURCE/INVALID_INPUT/UNSUPPORTED.
- Inspect is nonblocking and non-acknowledging. Only OK/ready/valid authorizes
  the result. Report actual failure; do not use learned confidence as proof of
  execution or as a physical SINR/noise measurement.
- Release acknowledges only after the host has finished/fenced all consumers.
  RESOURCE retains ownership for retry. Retire is control-only, handles partial
  submissions too, and permanently closes a successfully retired worker.

Host-supplied receipt/result objects must be valid, disjoint host storage.
Private receipts do not authenticate global tensor loans. The host must retain
the original worker, module, tensors, metadata and device resources through the
last consumer, even during deactivation. The extension is in-process only;
neither callbacks nor real-time tensors are serialized over E3.

## Compatibility and qualification

The ABI fingerprint includes the new types and offsets while preserving all
earlier entries. The existing GCC/Clang C11/C++17 matrix checks those layouts.
A vendor-side guarded CPU qualification additionally exercises the real native
loader/worker with independent C-table fixture DSOs and the actual receiver
completion implementation. This is not a substitute for target-GPU execution,
production pool ownership, deadline qualification or OTA comparison.

The production host currently does not advertise completion support. A module
query rejection is therefore expected, not a reason to bypass capability
checks. See the host's `docs/dapp/receiver_completion.md` for native ownership
and the remaining PHY integration requirements.
