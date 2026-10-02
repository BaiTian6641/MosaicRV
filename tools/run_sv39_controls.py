#!/usr/bin/env python3
"""Rebuild CASE=sv39.walk_and_faults with one control at a time and run it.

The controls are the fail modes docs/implementation-plan.md I-045 names, plus
the negative controls for the A/D policy:

  MOSAIC_PTW_MUTANT_ALLOW_NONCANONICAL
      a non-canonical virtual address is translated anyway, so it reaches the
      page table -- the card's first failure mode ("a non-canonical address
      still reaching physical RAM").
  MOSAIC_PTW_MUTANT_PTE_ERROR_OK
      an invalid or reserved PTE is accepted as a mapping to physical zero --
      the card's second failure mode ("a PTE error treated as an ordinary
      cache miss").
  MOSAIC_PTW_MUTANT_SUPERPAGE_UNALIGNED
      a misaligned superpage is served instead of faulting, taking its low bits
      from the virtual address.
  MOSAIC_PTW_MUTANT_SUM_IGNORED
      the SUM rule is dropped: S-mode reads a U page with SUM=0.
  MOSAIC_PTW_MUTANT_AD_INVERTED
      the A/D policy is inverted: the leaf that needs the update is the one that
      does not get it, and a leaf that needs nothing is written.

Each control is built from a deleted build directory with its -D on the
Verilator command line (and passed to the C++ half too, so a control can also be
expressed in the driver in future), so the binary hash differs from the shipping
one, and the run is expected to exit 1 with a named first failure. The script
prints the shipping baseline first, because a control is only evidence if the
shipping build passes.

Usage: python3 tools/run_sv39_controls.py [--profile p1]
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
CASE = "sv39.walk_and_faults"

CONTROLS = [
    ("shipping", None),
    ("ALLOW_NONCANONICAL", "MOSAIC_PTW_MUTANT_ALLOW_NONCANONICAL"),
    ("PTE_ERROR_OK", "MOSAIC_PTW_MUTANT_PTE_ERROR_OK"),
    ("SUPERPAGE_UNALIGNED", "MOSAIC_PTW_MUTANT_SUPERPAGE_UNALIGNED"),
    ("SUM_IGNORED", "MOSAIC_PTW_MUTANT_SUM_IGNORED"),
    ("AD_INVERTED", "MOSAIC_PTW_MUTANT_AD_INVERTED"),
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
    parser.add_argument("--profile", default="p1",
                        help="the profile whose supervisor mode the case needs (default p1)")
    args = parser.parse_args()

    with open(REGISTRY) as handle:
        entry = json.load(handle)["cases"][CASE]

    print("profile %s, case %s" % (args.profile, CASE))
    print("%-20s %-10s %-6s %s" % ("control", "sha256", "exit", "first failure"))
    failures = 0
    for name, define in CONTROLS:
        build_dir = os.path.join(REPO_ROOT, "build", args.profile, "unit",
                                 CASE + "." + name)
        try:
            binary, _ = build(args.profile, define, build_dir, entry)
        except RuntimeError as exc:
            print("%-20s BUILD FAILED\n%s" % (name, exc))
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
        print("%-20s %-10s %-6d %s" % (name, digest, run.returncode, first))
        if name == "shipping":
            if run.returncode != 0:
                failures += 1
        elif run.returncode == 0:
            failures += 1
    print("\n%d control(s) did not behave as the case requires" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
