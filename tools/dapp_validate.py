#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Unified dApp validation entrypoint (packaging → algorithm → system).

Reads a lab profile YAML (see lab_profile.example.yaml) and runs the shared
ladder without requiring a long list of environment variables.

Usage:
  python3 tools/dapp_validate.py --profile lab.yaml
  python3 tools/dapp_validate.py --profile lab.yaml --tiers packaging,algorithm
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any


def _load_profile(path: Path) -> dict[str, Any]:
    text = path.read_text(encoding="utf-8")
    # Prefer JSON (checked in as lab_profile.example.json).
    if path.suffix.lower() == ".json" or text.lstrip().startswith("{"):
        data = json.loads(text)
    else:
        try:
            import yaml  # type: ignore

            data = yaml.safe_load(text)
        except Exception as exc:  # noqa: BLE001
            raise SystemExit(
                f"cannot parse profile {path}: install PyYAML or use the .json example ({exc})"
            ) from exc
    if not isinstance(data, dict):
        raise SystemExit(f"profile must be a mapping: {path}")
    return data


def _resolve(base: Path, value: str | None) -> Path | None:
    if not value:
        return None
    path = Path(value)
    if not path.is_absolute():
        path = (base / path).resolve()
    return path


def _run(cmd: list[str], env: dict[str, str] | None = None, cwd: Path | None = None) -> int:
    print("==>", " ".join(cmd), flush=True)
    completed = subprocess.run(cmd, env=env, cwd=str(cwd) if cwd else None)
    return completed.returncode


def _so(package_dir: Path, name: str) -> Path:
    path = Path(name)
    if path.is_absolute():
        return path
    return package_dir / name


def _vendor_env(profile: dict[str, Any], package_dir: Path) -> dict[str, str]:
    """Environment for vendor package child processes.

    Merges, lowest precedence first: the caller's environment, an optional
    ``vendor_env.json`` (flat string map) shipped in the package directory, and
    the profile's ``environment:`` mapping. ``${package_dir}`` in a value
    expands to the package directory, so closed packages can locate weights or
    feature flags without this tool naming them.
    """
    env = os.environ.copy()
    layers: list[dict[str, Any]] = []
    manifest = package_dir / "vendor_env.json"
    if manifest.is_file():
        try:
            data = json.loads(manifest.read_text(encoding="utf-8"))
        except ValueError:
            data = None
        if isinstance(data, dict):
            layers.append(data)
        else:
            print(f"warning: ignoring unparsable {manifest}", file=sys.stderr)
    profile_env = profile.get("environment")
    if isinstance(profile_env, dict):
        layers.append(profile_env)
    for layer in layers:
        for key, value in layer.items():
            env[str(key)] = str(value).replace("${package_dir}", str(package_dir))
    return env


def run_packaging(profile: dict[str, Any], sdk_build: Path, package_dir: Path) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    cert = sdk_build / "certify_class_a_cuda"
    packaging = profile.get("packaging") or {}
    shape = str(packaging.get("certify_shape_set") or "compact")
    for entry in packaging.get("certify") or []:
        if not isinstance(entry, (list, tuple)) or len(entry) < 2:
            results.append({"name": "certify.bad_entry", "passed": False, "detail": str(entry)})
            continue
        so_name, iface = entry[0], entry[1]
        so = _so(package_dir, str(so_name))
        name = f"certify.{so.name}"
        if not cert.is_file():
            results.append({"name": name, "passed": False, "detail": f"missing {cert}"})
            continue
        if not so.is_file():
            results.append({"name": name, "passed": False, "detail": f"missing {so}"})
            continue
        env = _vendor_env(profile, package_dir)
        rc = _run([str(cert), str(so), str(iface), "--shape-set", shape], env=env)
        results.append({"name": name, "passed": rc == 0, "returncode": rc})
    return results


def run_algorithm(profile: dict[str, Any], sdk_build: Path, package_dir: Path) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    suite = sdk_build / "l1_validation_suite"
    algo = profile.get("algorithm") or {}
    eq = _so(package_dir, str(algo.get("eq_so") or "ref_equalizer_cuda.so"))
    rx_name = algo.get("rx_so")
    rx = _so(package_dir, str(rx_name)) if rx_name else None
    mode = str(algo.get("mode") or "all")
    quick = bool(algo.get("quick", True))
    name = "algorithm.l1"
    if not suite.is_file():
        return [{"name": name, "passed": False, "detail": f"missing {suite}"}]
    if not eq.is_file():
        return [{"name": name, "passed": False, "detail": f"missing {eq}"}]
    cmd = [str(suite), "--mode", mode, "--eq-so", str(eq)]
    if rx is not None:
        if not rx.is_file():
            return [{"name": name, "passed": False, "detail": f"missing {rx}"}]
        cmd += ["--rx-so", str(rx)]
    if quick:
        cmd.append("--quick")
    env = _vendor_env(profile, package_dir)
    rc = _run(cmd, env=env)
    results.append({"name": name, "passed": rc == 0, "returncode": rc})
    return results


def run_system(profile: dict[str, Any], profile_dir: Path, package_dir: Path) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    paths = profile.get("paths") or {}
    system = profile.get("system") or {}
    script_rel = system.get("matrix_script")
    name = "system.dummy_ru_matrix"
    if not script_rel:
        return [{"name": name, "passed": True, "detail": "skipped (no matrix_script)"}]
    script = _resolve(profile_dir, str(script_rel))
    if script is None or not script.is_file():
        # Also try package_dir parent tests/
        alt = package_dir.parent / "tests" / "run_dummy_ru_matrix.py"
        script = alt if alt.is_file() else script
    if script is None or not script.is_file():
        return [{"name": name, "passed": False, "detail": f"missing matrix script {script_rel}"}]

    env = os.environ.copy()
    for key, env_name in (
        ("odu", "OCUDU_ODU"),
        ("preflight", "OCUDU_DAPP_PREFLIGHT"),
        ("dappctl", "OCUDU_DAPPCTL_PY"),
        ("e3_schema", "OCUDU_DAPP_E3_SCHEMA"),
    ):
        value = paths.get(key)
        if value:
            env[env_name] = str(Path(value).expanduser())
    env["OCUDU_DAPP_SDK_BUILD"] = str(package_dir)
    cmd = [sys.executable, str(script), "--build-dir", str(package_dir)]
    rc = _run(cmd, env=env, cwd=script.parent.parent if (script.parent.name == "tests") else package_dir)
    results.append({"name": name, "passed": rc == 0, "returncode": rc})
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, required=True, help="lab profile YAML")
    parser.add_argument(
        "--tiers",
        default="",
        help="comma list: packaging,algorithm,system (default: profile tiers)",
    )
    parser.add_argument("--report", type=Path, default=None)
    args = parser.parse_args()

    profile_path = args.profile.resolve()
    profile = _load_profile(profile_path)
    paths = profile.get("paths") or {}
    profile_dir = profile_path.parent

    sdk_build = _resolve(profile_dir, str(paths.get("sdk_build") or ""))
    package_dir = _resolve(profile_dir, str(paths.get("package_dir") or ""))
    if sdk_build is None or package_dir is None:
        print("paths.sdk_build and paths.package_dir are required", file=sys.stderr)
        return 2

    tiers_cfg = profile.get("tiers") or {}
    if args.tiers.strip():
        wanted = {t.strip() for t in args.tiers.split(",") if t.strip()}
    else:
        wanted = {k for k, v in tiers_cfg.items() if v} or {"packaging", "algorithm"}

    results: list[dict[str, Any]] = []
    if "packaging" in wanted:
        results.extend(run_packaging(profile, sdk_build, package_dir))
    if "algorithm" in wanted:
        results.extend(run_algorithm(profile, sdk_build, package_dir))
    if "system" in wanted:
        results.extend(run_system(profile, profile_dir, package_dir))

    passed = all(r.get("passed") for r in results) if results else False
    report = {
        "schema": "ocudu.dapp.validate.report.v1",
        "status": "passed" if passed else "failed",
        "profile": str(profile_path),
        "tiers": sorted(wanted),
        "results": results,
    }
    out = args.report or (package_dir / "reports" / "dapp-validate.json")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"], "report": str(out)}, indent=2))
    for r in results:
        mark = "PASS" if r.get("passed") else "FAIL"
        print(f"  {mark}  {r.get('name')}  {r.get('detail', '')}".rstrip())
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
