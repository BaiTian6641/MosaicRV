#!/usr/bin/env python3
"""Negative controls for CASE=prefetch.fault_and_pollution (work package I-063).

The case claims a harm bound with two halves: a wrong prediction may only add
latency or traffic (never a fault, never an irreversible device read, never an
architectural change), and the predictor is never the only source of
correctness. A claim like that is evidence only if the defect it excludes would
be caught, so this tool rebuilds the case with exactly one defect injected
through a `-DMOSAIC_PREFETCH_MUTANT_*` define -- from an empty build directory,
so a mutant can never reuse the shipping object files -- and runs it. Every
mutant must:

  * build cleanly into its own directory, with its `-D` on the recorded command
    line (`build_command.txt` next to the binary);
  * produce a binary that differs from the shipping one (`cmp`), so a stale
    build cannot masquerade as a mutant;
  * exit 1, with the named check the mutation breaks as the first failure.

A mutant that exits 0 is either an unobservable defect or a check that cannot
fail; this tool reports it as NOT MET rather than counting it.

Usage: python3 tools/run_prefetch_controls.py [--profile p1]
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
CASE = "prefetch.fault_and_pollution"
SOURCE = os.path.join("rtl", "core", "mosaic_prefetch.sv")

# Each mutant: the define, the defect it injects, and the text of the first
# failure the mutation must produce.
MUTANTS = [
    (
        "MOSAIC_PREFETCH_MUTANT_PERM_BYPASS",
        "the translation/permission gate is not applied, so a candidate prefetch reaches an "
        "address a demand would have refused and a faulting demand spawns a hint -- the "
        "card's central fail mode, 'a predicted path that bypasses the permission check'",
        "unmapped-prefetch:",
    ),
    (
        "MOSAIC_PREFETCH_MUTANT_DEVICE_READ",
        "the device-free fact is not applied, so a prefetch issues an irreversible read of "
        "the UART -- the card's second fail mode, 'a wrong prediction that performs a "
        "device read'",
        "device-read:",
    ),
    (
        "MOSAIC_PREFETCH_MUTANT_CANCEL_LANDS",
        "a cancelled prefetch's response is installed anyway, so a cancelled access leaves "
        "state behind -- the card's 'a cancelled access has no side effect'",
        "cancel:",
    ),
    (
        "MOSAIC_PREFETCH_MUTANT_ARCH_DATA",
        "the fill data is corrupted, so a later demand that hits the prefetched copy returns "
        "a value that differs from memory -- the card's 'the predictor becomes the only "
        "source of correctness'",
        "architectural-hit:",
    ),
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
    cmd += ["-I" + os.path.join(REPO_ROOT, "build", profile, "sim"),
            "-I" + os.path.join(REPO_ROOT, "rtl", "core"),
            "-I" + os.path.join(REPO_ROOT, "rtl", "common"),
            "-I" + os.path.join(REPO_ROOT, "build", profile, "rtl"),
            "-CFLAGS", "-I" + os.path.join(REPO_ROOT, "sim", "common"),
            "-CFLAGS", "-I" + os.path.join(REPO_ROOT, "build", profile, "sim"),
            "-o", binary]
    cmd += sources(entry)
    with open(os.path.join(build_dir, "build_command.txt"), "w") as handle:
        handle.write(" ".join(cmd))
    result = subprocess.run(cmd, cwd=REPO_ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    log = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0 or not os.path.exists(binary):
        raise RuntimeError("build failed for %s\n%s" % (define or "shipping", log[-4000:]))
    return binary, log


def sha256_of(path: str) -> str:
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def first_failure(log: str) -> str:
    for line in log.splitlines():
        if line.startswith("MISMATCH "):
            return line[len("MISMATCH "):].strip()
    return "(no MISMATCH line)"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="p1",
                        help="the profile whose platform map caches RAM (default p1)")
    parser.add_argument("--only", default=None)
    args = parser.parse_args()

    with open(REGISTRY) as handle:
        entry = json.load(handle)["cases"][CASE]

    # The mutation must exist in the source before it is claimed to have been
    # built: a `-D` that no `ifdef` reads would build a shipping binary under a
    # mutant's name.
    source = os.path.join(REPO_ROOT, SOURCE)
    with open(source) as handle:
        text = handle.read()
    for define, _defect, _expected in MUTANTS:
        if define not in text:
            print("FAIL: %s does not occur in %s" % (define, source))
            return 1

    root = os.path.join(REPO_ROOT, "build", args.profile, "unit", CASE + ".controls")
    print("profile %s, case %s" % (args.profile, CASE))
    print("building the shipping case from an empty directory...")
    shipping, _ = build(args.profile, None, os.path.join(root, "shipping"), entry)
    out = os.path.join(REPO_ROOT, "results", "unit", CASE + ".shipping")
    os.makedirs(out, exist_ok=True)
    run = subprocess.run(
        [shipping, "--case", CASE, "--out", out, "--seed", str(entry.get("seed", 1)),
         "--max-cycles", str(entry.get("max_cycles", 200000))],
        cwd=REPO_ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=1800)
    with open(os.path.join(out, "run.log"), "w") as handle:
        handle.write(run.stdout.decode("utf-8", "replace"))
    if run.returncode != 0:
        print("BASELINE FAILED: the shipping build must pass before mutants mean anything")
        print(run.stdout.decode("utf-8", "replace")[-2000:])
        return 1
    print("  shipping sha256: %s" % sha256_of(shipping))
    print()

    failures = 0
    for define, defect, expected in MUTANTS:
        if args.only is not None and args.only not in define:
            continue
        build_dir = os.path.join(root, define)
        try:
            mutant, _ = build(args.profile, define, build_dir, entry)
        except RuntimeError as exc:
            print("%-40s BUILD FAILED" % define)
            print(exc)
            failures += 1
            continue
        differs = subprocess.run(["cmp", "-s", shipping, mutant]).returncode != 0
        mout = os.path.join(REPO_ROOT, "results", "unit", CASE + "." + define)
        os.makedirs(mout, exist_ok=True)
        mrun = subprocess.run(
            [mutant, "--case", CASE, "--out", mout, "--seed", str(entry.get("seed", 1)),
             "--max-cycles", str(entry.get("max_cycles", 200000))],
            cwd=REPO_ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=1800)
        log = mrun.stdout.decode("utf-8", "replace")
        with open(os.path.join(mout, "run.log"), "w") as handle:
            handle.write(log)
        caught = mrun.returncode == 1 and expected in log
        status = "OK" if (differs and caught) else "MISS"
        if not differs:
            status += " (binary identical to shipping)"
        if not caught:
            status += " (exit %d, expected %r)" % (mrun.returncode, expected)
        print("%-40s %-5d %s" % (define, mrun.returncode, status))
        print("    injects: %s" % defect)
        print("    sha256:  %s" % sha256_of(mutant))
        print("    %s" % first_failure(log))
        if not (differs and caught):
            failures += 1

    if failures:
        print("%d mutant(s) were not caught as required" % failures)
        return 1
    print("all %d mutants mutate the binary, exit 1 and name the check they break"
          % (len(MUTANTS) if args.only is None else 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
