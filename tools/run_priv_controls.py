#!/usr/bin/env python3
"""Rebuild CASE=privilege.permission_matrix with one control at a time and run it.

The controls are the four fail modes docs/implementation-plan.md I-044 names:

  MOSAIC_PMP_MUTANT_LOCK_IGNORED      a locked entry's configuration byte can be
                                      overwritten (RTL switch in mosaic_pmp)
  MOSAIC_PMP_MUTANT_OVERLAP_INVERTED  the highest-numbered matching entry decides
                                      instead of the lowest (RTL switch)
  MOSAIC_PMP_MUTANT_M_MODE_ENFORCED   an unlocked entry's permissions are
                                      enforced against an M-mode access (RTL)
  MOSAIC_PRIV_MUTANT_FAULT_WRITES     "a permission fault still writes": the
                                      *checker* is mutated to expect the refused
                                      access to have taken effect (driver-side;
                                      the refusal is structural in the RTL and no
                                      RTL switch can express "it wrote anyway")

Each control is built from a deleted build directory with its -D on the Verilator
command line, so the binary hash differs from the shipping one, and the run is
expected to exit 1 with a named first failure. This script prints the shipping
baseline first, because a control is only evidence if the shipping build passes
-- and for this case under --profile p1 it does not (see
results/reports/I-044-privilege.md).

Usage: python3 tools/run_priv_controls.py [--profile p0|p1]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mosaic import config_check  # noqa: E402

REPO_ROOT = config_check.REPO_ROOT
REGISTRY = os.path.join(REPO_ROOT, "tests", "unit", "registry.json")
CASE = "privilege.permission_matrix"

CONTROLS = [
    ("shipping", None),
    ("LOCK_IGNORED", "MOSAIC_PMP_MUTANT_LOCK_IGNORED"),
    ("OVERLAP_INVERTED", "MOSAIC_PMP_MUTANT_OVERLAP_INVERTED"),
    ("M_MODE_ENFORCED", "MOSAIC_PMP_MUTANT_M_MODE_ENFORCED"),
    ("FAULT_WRITES", "MOSAIC_PRIV_MUTANT_FAULT_WRITES"),
]


def sources(entry: dict) -> list[str]:
    rtl = entry["rtl"]
    packages = [p for p in rtl if os.path.basename(p) == "mosaic_pkg.sv"]
    rest = [p for p in rtl if p not in packages]
    listed = packages + rest + entry.get("sv", []) + entry.get("cpp", []) + \
        ["sim/common/sim_common.cpp"]
    return [os.path.join(REPO_ROOT, p) for p in listed]


def build(profile: str, define: str | None, build_dir: str, entry: dict) -> tuple[str, str]:
    """Build the case; returns (binary path, log). Raises on a build failure."""
    shutil.rmtree(build_dir, ignore_errors=True)
    os.makedirs(build_dir, exist_ok=True)
    binary = os.path.join(build_dir, "case")
    cmd = ["verilator", "--cc", "--exe", "--build", "-j", "0", "-O2",
           "-CFLAGS", "-O2 -std=c++17 -Wall", "--x-assign", "unique",
           "--x-initial", "unique", "--top-module", entry["top"],
           "-Mdir", os.path.join(build_dir, "obj_dir")]
    if define:
        # The RTL switches are Verilog macros and the driver control is a C++
        # macro, so the define is offered to both halves: Verilator preprocesses
        # the SystemVerilog with it and -CFLAGS passes it to the C++ compile.
        # A macro neither half uses is inert, so this cannot hide a control.
        cmd.append("-D" + define)
        cmd += ["-CFLAGS", "-D" + define]
    cmd += ["-I" + os.path.join(REPO_ROOT, "build", profile, "sim"),
            "-I" + os.path.join(REPO_ROOT, "rtl", "core"),
            "-I" + os.path.join(REPO_ROOT, "rtl", "common"),
            "-I" + os.path.join(REPO_ROOT, "build", profile, "rtl"),
            "-CFLAGS", "-I" + os.path.join(REPO_ROOT, "sim", "common"),
            "-CFLAGS", "-I" + os.path.join(REPO_ROOT, "build", profile, "sim"),
            "-o", binary]
    cmd += sources(entry)
    result = subprocess.run(cmd, cwd=REPO_ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    log = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0 or not os.path.exists(binary):
        raise RuntimeError("build failed for %s\n%s" % (define or "shipping", log[-4000:]))
    return binary, log


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="p1")
    args = parser.parse_args()

    with open(REGISTRY) as handle:
        entry = json.load(handle)["cases"][CASE]

    print("profile %s, case %s" % (args.profile, CASE))
    print("%-18s %-10s %-6s %s" % ("control", "sha256", "exit", "first failure"))
    failures = 0
    for name, define in CONTROLS:
        build_dir = os.path.join(REPO_ROOT, "build", args.profile, "unit",
                                 CASE + "." + name)
        try:
            binary, _ = build(args.profile, define, build_dir, entry)
        except RuntimeError as exc:
            print("%-18s BUILD FAILED\n%s" % (name, exc))
            failures += 1
            continue
        with open(binary, "rb") as handle:
            digest = hashlib.sha256(handle.read()).hexdigest()[:12]
        out_dir = os.path.join(REPO_ROOT, "results", "unit",
                               CASE + "." + name)
        os.makedirs(out_dir, exist_ok=True)
        run = subprocess.run([binary, "--case", CASE, "--out", out_dir,
                              "--seed", str(entry.get("seed", 1)),
                              "--max-cycles", str(entry.get("max_cycles", 100000))],
                             cwd=REPO_ROOT, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=1800)
        log = run.stdout.decode("utf-8", "replace")
        with open(os.path.join(out_dir, "run.log"), "w") as handle:
            handle.write(log)
        first = ""
        for line in log.splitlines():
            if line.startswith("MISMATCH"):
                first = line.split(":", 1)[1].strip()
                break
        print("%-18s %-10s %-6d %s" % (name, digest, run.returncode, first))
        if name == "shipping":
            if run.returncode != 0:
                failures += 1
        elif run.returncode == 0:
            failures += 1
    print("\n%d control(s) did not behave as the case requires" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
