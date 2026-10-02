#!/usr/bin/env python3
"""Rebuild CASE=llb.freshness_and_ownership with one control at a time and run it.

This is the V-061 companion to `tools/run_llb_controls.py` (I-060). The case
shares the I-060 DUT (`mosaic_llb`) and driver (`sim/unit/tb_llb.cpp`), so the
mutants are the same five switches the RTL already carries; each is a fail mode
the V-061 card names:

  MOSAIC_LLB_MUTANT_STORE_NO_INVALIDATE
      a store does not invalidate the line it wrote, so the LLB treats a clean
      copy as needing no coherence -- `local-writer-freshness` is the first
      phase to catch it.
  MOSAIC_LLB_MUTANT_MMIO_HIT
      the generated platform map is ignored, so a device line is filled and can
      be hit -- `pma-non-cacheable` catches it.
  MOSAIC_LLB_MUTANT_RACE_STALE_HIT
      the combinational refusal is removed, so a speculative hit in the
      invalidating cycle returns the pre-write value --
      `misprediction-race/hit-store` catches it.
  MOSAIC_LLB_MUTANT_ASID_SWITCH_NO_INVALIDATE
      a context change leaves entries reachable -- `ownership-context-change`
      catches it.
  MOSAIC_LLB_MUTANT_VA_KEYED
      the key includes the virtual page number, so the same physical line through
      a second virtual alias misses -- `same-address-aliasing` catches it.

Each control is built from a deleted build directory with its -D on the Verilator
command line (and passed to the C++ half too), so the binary hash differs from
the shipping one, and the run is expected to exit 1 with a named first failure.
The shipping baseline is printed first: a control is only evidence if the
shipping build passes.

The case's source list is held here rather than read from
`tests/unit/registry.json`, because the integration lead registers the entry --
this script must be runnable before that. The list must match the registered
entry (`top=mosaic_llb_tb`, the I-060 sources).

Usage: python3 tools/run_llb_freshness_controls.py [--profile p1]
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
CASE = "llb.freshness_and_ownership"

# The spec the integration lead should register. Kept in lockstep with the
# I-060 entry because the two cases share this top and these sources.
SPEC = {
    "task": "V-061",
    "profiles": ["p1"],
    "top": "mosaic_llb_tb",
    "rtl": [
        "rtl/core/mosaic_pkg.sv",
        "rtl/core/mosaic_cache.sv",
        "rtl/core/mosaic_llb.sv",
    ],
    "sv": ["sim/tb/mosaic_llb_tb.sv"],
    "cpp": ["sim/unit/tb_llb.cpp"],
    "max_cycles": 200000,
    "seed": 1,
}

CONTROLS = [
    ("shipping", None),
    ("STORE_NO_INVALIDATE", "MOSAIC_LLB_MUTANT_STORE_NO_INVALIDATE"),
    ("MMIO_HIT", "MOSAIC_LLB_MUTANT_MMIO_HIT"),
    ("RACE_STALE_HIT", "MOSAIC_LLB_MUTANT_RACE_STALE_HIT"),
    ("ASID_SWITCH_NO_INVALIDATE", "MOSAIC_LLB_MUTANT_ASID_SWITCH_NO_INVALIDATE"),
    ("VA_KEYED", "MOSAIC_LLB_MUTANT_VA_KEYED"),
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
    cmd += sources(entry)
    result = subprocess.run(cmd, cwd=REPO_ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    log = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0 or not os.path.exists(binary):
        raise RuntimeError("build failed for %s\n%s"
                           % (define or "shipping", log[-4000:]))
    return binary, log


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="p1",
                        help="the profile whose platform map caches RAM (default p1)")
    args = parser.parse_args()

    # If the integration lead has registered the entry, prefer it; it is the
    # source of truth once it exists. Otherwise use the spec above.
    entry = SPEC
    try:
        with open(REGISTRY) as handle:
            registered = json.load(handle)["cases"].get(CASE)
        if registered:
            entry = registered
    except (OSError, KeyError, ValueError):
        pass

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
            if line.startswith("MISMATCH "):
                first = line[len("MISMATCH "):].strip()
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
