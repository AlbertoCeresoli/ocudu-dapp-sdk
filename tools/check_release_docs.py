#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Check public-release metadata, relative Markdown links, and source hygiene."""

from __future__ import annotations

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SKIP_DIRS = {".git", ".ci", "artifacts", "__pycache__"}
REQUIRED = {
    "README.md",
    "LICENSE",
    "CHANGELOG.md",
    "CONTRIBUTING.md",
    "SECURITY.md",
    "CODE_OF_CONDUCT.md",
    "SUPPORT.md",
    "API_STABILITY.md",
    "REUSE.toml",
    "LICENSES/BSD-3-Clause-Clear.txt",
    "docs/extension_model.md",
    "docs/security_model.md",
    "docs/compatibility.md",
    "docs/known_limitations.md",
    "docs/releasing.md",
    ".gitlab/ci/ocudu-host.lock",
}
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cu", ".cuh", ".h", ".hpp", ".py", ".sh", ".cmake", ".in", ".md", ".dox", ".yml", ".yaml"}
FORBIDDEN = {
    "/home/" + "deepsig": "developer-local absolute path",
    "nvcuda_accel_02_" + "e3DApp": "private feature branch",
    "nvcuda_accel_02_" + "e3dApp": "private feature branch",
    "gitlab-ci-" + "token:": "embedded private CI credential route",
    "registry.gitlab.com/" + "deepsig": "private builder image",
    # Split so this file does not match itself.
    "find_package(OCUDUDapp " + "1 ": "impossible package version (the package is 0.1.x)",
}
LINK_PATTERN = re.compile(r"(?<!!)\[[^]]*\]\(([^)]+)\)")


def project_files() -> list[pathlib.Path]:
    result = []
    for path in ROOT.rglob("*"):
        relative = path.relative_to(ROOT)
        if any(part in SKIP_DIRS or part.startswith("build") for part in relative.parts):
            continue
        if path.is_file():
            result.append(path)
    return result


# The path-discovery helper is intentionally copied into each standalone E3AP
# example directory; this guard keeps the copies from drifting apart.
COMMON_COPIES = [
    "examples/e3_telemetry_python/_common.py",
    "examples/e3_spectrum_sensing/_common.py",
    "examples/e3_isac_sensing/_common.py",
    "examples/e3_mcp_server/_common.py",
]


def main() -> int:
    errors: list[str] = []
    for relative in sorted(REQUIRED):
        if not (ROOT / relative).is_file():
            errors.append(f"missing release file: {relative}")

    common_texts = {relative: (ROOT / relative).read_bytes() for relative in COMMON_COPIES if (ROOT / relative).is_file()}
    if len(common_texts) != len(COMMON_COPIES):
        errors.append("missing example _common.py copy: " + ", ".join(sorted(set(COMMON_COPIES) - set(common_texts))))
    elif len(set(common_texts.values())) != 1:
        errors.append("example _common.py copies have drifted apart; make them byte-identical: " + ", ".join(COMMON_COPIES))

    for path in project_files():
        relative = path.relative_to(ROOT)
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue

        # REUSE-IgnoreStart
        if (path.suffix in SOURCE_SUFFIXES or path.name == "CMakeLists.txt") and "SPDX-License-Identifier:" not in text:
            errors.append(f"missing SPDX license identifier: {relative}")
        # REUSE-IgnoreEnd
        for token, description in FORBIDDEN.items():
            if token in text:
                errors.append(f"{relative}: contains {description}: {token}")

        if path.suffix != ".md":
            continue
        for match in LINK_PATTERN.finditer(text):
            target = match.group(1).strip().split(maxsplit=1)[0].strip("<>")
            if not target or target.startswith(("#", "http://", "https://", "mailto:")):
                continue
            target_path = target.split("#", 1)[0].split("?", 1)[0]
            if not target_path:
                continue
            resolved = (path.parent / target_path).resolve()
            if ROOT != resolved and ROOT not in resolved.parents:
                # Workspace-sibling link (e.g. ../ocudu/docs/...): checkable
                # only when that repo sits next to this one; verified then,
                # skipped in a standalone checkout.
                try:
                    sibling = ROOT.parent / resolved.relative_to(ROOT.parent).parts[0]
                except ValueError:
                    continue
                if sibling.is_dir() and not resolved.exists():
                    errors.append(f"broken sibling link: {relative} -> {target}")
                continue
            if not resolved.exists():
                errors.append(f"broken relative link: {relative} -> {target}")

    if errors:
        print("\n".join(sorted(set(errors))), file=sys.stderr)
        return 1
    print(f"release metadata and documentation checks passed ({len(project_files())} files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
