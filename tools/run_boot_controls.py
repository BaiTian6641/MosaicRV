#!/usr/bin/env python3
"""Rebuild CASE=boot.p1_contract with one control at a time and run it.

The controls are the fail modes docs/implementation-plan.md I-048 names:

  MOSAIC_CSR_MUTANT_NO_DELEGATION    delegation is dropped in the CSR file, so an
                                     exception the profile delegates to S-mode is
                                     taken in M-mode instead.
  MOSAIC_CSR_MUTANT_SRET_NO_RESTORE  `sret` returns but restores nothing, so the
                                     supervisor state the case reads after it is
                                     still the trapped state.
  MOSAIC_BOOT_MUTANT_BAD_PAGETABLE   the firmware builds the test subtree wrongly,
                                     so the first S-mode access faults where the
                                     handler does not expect it (driver-side).
  MOSAIC_BOOT_MUTANT_BANNER_ACCEPTS  the *checker* accepts a Linux banner as the
                                     pass criterion instead of the self-check
                                     signature; it must fail the real check
                                     (driver-side).

Each control is built from a deleted build directory with its -D on the Verilator
command line (and on the C++ half), so the binary hash differs from the shipping
one, and the run is expected to exit 1 with a named first failure. The shipping
baseline is printed first, because a control is only evidence if the shipping
build passes.

Usage: python3 tools/run_boot_controls.py [--profile p1]
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
CASE = "boot.p1_contract"

CONTROLS = [
    ("shipping", None),
    ("NO_DELEGATION", "MOSAIC_CSR_MUTANT_NO_DELEGATION"),
    ("SRET_NO_RESTORE", "MOSAIC_CSR_MUTANT_SRET_NO_RESTORE"),
    ("BAD_PAGETABLE", "MOSAIC_BOOT_MUTANT_BAD_PAGETABLE"),
    ("BANNER_ACCEPTS", "MOSAIC_BOOT_MUTANT_BANNER_ACCEPTS"),
]


def sources(entry: dict) -> list:
    rtl = entry["rtl"]
    packages = [p for p in rtl if os.path.basename(p) == "mosaic_pkg.sv"]
    rest = [p for p in rtl if p not in packages]
    listed = packages + rest + entry.get("sv", []) + entry.get("cpp", []) + \
        ["sim/common/sim_common.cpp"]
    return [os.path.join(REPO_ROOT, p) for p in listed]


def build(profile: str, define: str | None, build_dir: str, entry: dict):
    """Build the case; returns (binary path, log). Raises on a build failure."""
    shutil.rmtree(build_dir, ignore_errors=True)
    os.makedirs(build_dir, exist_ok=True)
    binary = os.path.join(build_dir, "case")
    cmd = ["verilator", "--cc", "--exe", "--build", "-j", "0", "-O2",
           "-CFLAGS", "-O2 -std=c++17 -Wall", "--x-assign", "unique",
           "--x-initial", "unique", "--top-module", entry["top"],
           "-Mdir", os.path.join(build_dir, "obj_dir")]
    if define:
        cmd.append("-D" + define)
        cmd += ["-CFLAGS", "-D" + define]
    cmd += ["-I" + os.path.join(REPO_ROOT, "build", profile, "sim"),
            "-I" + os.path.join(REPO_ROOT, "rtl", "core"),
            "-I" + os.path.join(REPO_ROOT, "rtl", "common"),
            "-I" + os.path.join(REPO_ROOT, "build", profile, "rtl"),
            "-CFLAGS", "-I" + os.path.join(REPO_ROOT, "sim", "common"),
            "-CFLAGS", "-I" + os.path.join(REPO_ROOT, "build", profile, "sim"),
            "-o", binary]
    # The registry lists the core RTL explicitly; the walker and the cache are
    # pulled in by the top-level wrapper, so they must be added here.
    cmd += sources(entry)
    result = subprocess.run(cmd, cwd=REPO_ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    log = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0 or not os.path.exists(binary):
        raise RuntimeError("build failed for %s\n%s" % (define or "shipping", log[-4000:]))
    return binary, log


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="p1",
                        help="the profile whose supervisor mode the case needs (default p1)")
    args = parser.parse_args()

    with open(REGISTRY) as handle:
        entry = json.load(handle)["cases"][CASE]

    print("profile %s, case %s" % (args.profile, CASE))
    print("%-22s %-12s %-6s %s" % ("control", "sha256", "exit", "first failure"))
    failures = 0
    for name, define in CONTROLS:
        build_dir = os.path.join(REPO_ROOT, "build", args.profile, "unit",
                                 CASE + "." + name)
        try:
            binary, _ = build(args.profile, define, build_dir, entry)
        except RuntimeError as exc:
            print("%-22s BUILD FAILED\n%s" % (name, exc))
            failures += 1
            continue
        with open(binary, "rb") as handle:
            digest = hashlib.sha256(handle.read()).hexdigest()[:12]
        out_dir = os.path.join(REPO_ROOT, "results", "unit", CASE + "." + name)
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
        print("%-22s %-12s %-6d %s" % (name, digest, run.returncode, first))
        if name == "shipping":
            if run.returncode != 0:
                failures += 1
        elif run.returncode == 0:
            failures += 1
    print("\n%d control(s) did not behave as the case requires" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
