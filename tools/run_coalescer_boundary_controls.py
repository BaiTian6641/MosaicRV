#!/usr/bin/env python3
"""Rebuild CASE=cache.coalescer_boundaries with one control at a time and run it.

This is the V-062 negative-control runner. The case shares the I-042/I-043 DUT
(`mosaic_cache` + `mosaic_mshr`) and driver (`sim/unit/tb_cache.cpp`), so the
controls are `-DMOSAIC_CACHE_MUTANT_*` switches the RTL already carries or that
this package added. Each is a fail mode the V-062 card names:

  MOSAIC_CACHE_MUTANT_COALESCE_LOW_BITS
      two different lines that share a set and an offset coalesce, so the merge
      key is the low address bits instead of the whole line -- "matching on low
      address bits alone". `cross-line-same-set` catches it (and then the value
      the second consumer receives is the first line's).
  MOSAIC_CACHE_MUTANT_STORE_MASK_DROP
      a coalesced store writes every byte lane whatever `wmask` says, so a
      partial store clobbers the bytes it did not select -- "losing byte
      enables". `store-byte-merge` catches it.
  MOSAIC_CACHE_MUTANT_WRONG_WAITER
      every waiter on a coalesced line is answered from the FIFO's first slot,
      so a request that joined an in-flight miss gets another requester's word
      -- "a coalesced response delivered to the wrong requester".
      `same-line-different-bytes` catches it.
  MOSAIC_CACHE_MUTANT_FAULT_VALID
      a faulted refill is installed as if it had succeeded, so the consumers
      that should each receive the line's fault instead receive data --
      "wrong consumers sharing a fault". `coalesced-fault-per-request` catches
      it, and `cross-line`/`same-line` value checks would catch the wrong-line
      form of the same defect.

Each control is built from a deleted build directory with its -D on the
Verilator command line (and in -CFLAGS for the C++ half), so the binary hash
differs from the shipping one, and the run must exit 1 with a named first
failure. The shipping baseline is printed first: a control is only evidence if
the shipping build passes.

The case's source list is held here rather than read from
`tests/unit/registry.json`, because the integration lead registers the entry --
this script must be runnable before that. The list must match the registered
entry (`top=mosaic_cache_tb`, the I-042/I-043 sources).

Usage: python3 tools/run_coalescer_boundary_controls.py [--profile p1]
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
CASE = "cache.coalescer_boundaries"

# The spec the integration lead should register. Kept in lockstep with the
# I-042/I-043 entries because the three cases share this top and these sources.
SPEC = {
    "task": "V-062",
    "profiles": ["p1"],
    "top": "mosaic_cache_tb",
    "rtl": [
        "rtl/core/mosaic_pkg.sv",
        "rtl/core/mosaic_uop_pkg.sv",
        "rtl/core/mosaic_cache.sv",
        "rtl/core/mosaic_mshr.sv",
        "rtl/core/mosaic_cache_line_bridge.sv",
        "rtl/core/mosaic_llb.sv",
        "rtl/core/mosaic_prefetch.sv",
        "rtl/core/mosaic_locality_path.sv",
        "rtl/core/mosaic_l1_cache_path.sv",
    ],
    "sv": ["sim/tb/mosaic_cache_tb.sv"],
    "cpp": ["sim/unit/tb_cache.cpp"],
    "max_cycles": 200000,
    "seed": 1,
}

CONTROLS = [
    ("shipping", None),
    ("COALESCE_LOW_BITS", "MOSAIC_CACHE_MUTANT_COALESCE_LOW_BITS"),
    ("STORE_MASK_DROP", "MOSAIC_CACHE_MUTANT_STORE_MASK_DROP"),
    ("WRONG_WAITER", "MOSAIC_CACHE_MUTANT_WRONG_WAITER"),
    ("FAULT_VALID", "MOSAIC_CACHE_MUTANT_FAULT_VALID"),
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
                        help="the profile the case is registered for (default p1)")
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
            if line.startswith("MISMATCH ") or line.startswith("CHECK FAILED:"):
                first = line.split(":", 1)[-1].strip() if line.startswith("CHECK FAILED:") \
                    else line[len("MISMATCH "):].strip()
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
