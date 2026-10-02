#!/usr/bin/env python3
"""Rebuild CASE=locality.integrated_path with one control at a time and run it.

The controls are the fail modes the I-060 integration card names:

  MOSAIC_LOC_MUTANT_PERM_BYPASS
      the access-class rule is dropped, so a store's read-for-ownership may
      consult and populate the locality buffer just as a load's refill does --
      a copy taken under one access class reused by another.
  MOSAIC_LLB_MUTANT_STORE_NO_INVALIDATE
      a store does not invalidate the line it wrote, so a later load that misses
      the L1 is served the pre-store copy -- the structure's central failure.
  MOSAIC_PREFETCH_MUTANT_PERM_BYPASS
      the prefetch's read-safe group is dropped, so a prefetch read is presented
      for an address the demand path would have refused (the line past the top
      of RAM).
  MOSAIC_PREFETCH_MUTANT_ARCH_DATA
      the prefetch's fill data is corrupted, so enabling the structure changes
      an architectural result.

Each control is built from a deleted build directory with its -D on the Verilator
command line (and passed to the C++ half too), so the binary hash differs from
the shipping one, and the run is expected to exit 1 with a named first failure.
The shipping baseline is printed first, because a control is only evidence if the
shipping build passes.

Usage: python3 tools/run_locality_controls.py [--profile p1]
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
CASE = "locality.integrated_path"

CONTROLS = [
    ("shipping", None),
    ("PERM_BYPASS", "MOSAIC_LOC_MUTANT_PERM_BYPASS"),
    ("STORE_NO_INVALIDATE", "MOSAIC_LLB_MUTANT_STORE_NO_INVALIDATE"),
    ("PREFETCH_PERM_BYPASS", "MOSAIC_PREFETCH_MUTANT_PERM_BYPASS"),
    ("PREFETCH_ARCH_DATA", "MOSAIC_PREFETCH_MUTANT_ARCH_DATA"),
]


def sources(entry: dict) -> list:
    rtl = entry["rtl"]
    packages = [p for p in rtl if os.path.basename(p) == "mosaic_pkg.sv"]
    rest = [p for p in rtl if p not in packages]
    listed = packages + rest + entry.get("sv", []) + entry.get("cpp", []) + \
        ["sim/common/sim_common.cpp"]
    return [os.path.join(REPO_ROOT, p) for p in listed]


def build(profile: str, define: str | None, build_dir: str, entry: dict):
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
    print("%-28s %-12s %-6s %s" % ("control", "sha256", "exit", "first failure"))
    failures = 0
    for name, define in CONTROLS:
        build_dir = os.path.join(REPO_ROOT, "build", args.profile, "unit",
                                 CASE + "." + name)
        try:
            binary, _ = build(args.profile, define, build_dir, entry)
        except RuntimeError as exc:
            print("%-28s BUILD FAILED\n%s" % (name, exc))
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
            if line.startswith("MISMATCH ") or line.startswith("CHECK FAILED: "):
                first = line.strip()
                break
        print("%-28s %-12s %-6d %s" % (name, digest, run.returncode, first))
        if name == "shipping":
            if run.returncode != 0:
                failures += 1
        elif run.returncode == 0:
            failures += 1
    print("\n%d control(s) did not behave as the case requires" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
