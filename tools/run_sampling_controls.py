#!/usr/bin/env python3
"""Negative controls for the sampling loop (work package V-010).

CASE=harness.sampling_calibration claims that one cycle is one rising edge of
h_clk, that the settled state is sampled exactly once per cycle, and that every
accept and every retire is recorded exactly once. A claim like that is evidence
only if the defects it excludes would be caught, so this tool rebuilds the case
with exactly one defect injected through a `-DMOSAIC_SAMPLING_MUTANT_*` define,
from an empty build directory, and runs it. Every mutant must:

  * build cleanly into its own directory, wiped first (a mutant must never reuse
    the shipping object files);
  * produce a binary that differs from the shipping one (`cmp`), so a stale
    build cannot masquerade as a mutant;
  * exit 1, with the named check the mutation breaks as the first failure.

A mutant that exits 0 is either an unobservable defect or a check that cannot
fail; this tool reports it as NOT MET rather than counting it.

Three mutants are C++ (the driver's sampling loop); one is SystemVerilog (the
DUT wrapper's handshake). Each define is passed to Verilator and to the C++
compiler, because only one of the two halves is affected by any given mutant and
the other half ignores it.

The shipping build is built and run first, from an empty directory: mutants mean
nothing if the case does not pass in the first place.

Usage: run_sampling_controls.py [--case harness.sampling_calibration] [--only NAME]
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_unit  # noqa: E402

REPO_ROOT = run_unit.REPO_ROOT

# Each mutant: the define, the defect it injects, and the text of the check that
# must catch it. The text is the exact label the driver uses, so a mutant that
# fails for the wrong reason is reported rather than counted.
MUTANTS = [
    (
        "MOSAIC_SAMPLING_MUTANT_DOUBLE_EVAL",
        "the high phase is evaluated twice and the sampler is re-entered, so eval() "
        "is used as if it were a clock and one architectural retire is recorded "
        "twice",
        "no cycle was sampled twice for one handshake",
    ),
    (
        "MOSAIC_SAMPLING_MUTANT_REEMIT_RECORD",
        "the record callback fires twice for the same architectural event and "
        "re-emits the identical (retire_seq, pc) identity",
        "no retire_seq is emitted twice",
    ),
    (
        "MOSAIC_SAMPLING_MUTANT_SKIP_SAMPLE",
        "the timeline is advanced without re-sampling: the rising edge is taken but "
        "the settled state is not looked at every other cycle, so retires are missed",
        "the tap recorded one event per instruction the reference says",
    ),
    (
        "MOSAIC_SAMPLING_MUTANT_STRETCH_ACK",
        "the memory model holds the fetch ack high for two cycles instead of one; the "
        "core ignores the second cycle, so nothing architectural changes and only an "
        "environment that counts cycles-high as accepts sees a phantom handshake",
        "the fetch ack is a one-cycle pulse",
    ),
]


def build(entry: dict, case_id: str, defines: list, build_dir: str) -> str:
    """Build one configuration from an empty directory; returns the log."""
    shutil.rmtree(build_dir, ignore_errors=True)
    os.makedirs(build_dir, exist_ok=True)

    rtl = entry.get("rtl", [])
    packages = [p for p in rtl if os.path.basename(p) == "mosaic_pkg.sv"]
    rest = [p for p in rtl if p not in packages]
    sources = [
        os.path.join(REPO_ROOT, p)
        for p in (packages + rest + entry.get("sv", []) + entry.get("cpp", [])
                  + run_unit.SHARED_CPP)
    ]
    cmd = ["verilator"] + run_unit.VERILATOR_FLAGS
    cmd += ["--top-module", entry["top"], "-Mdir", os.path.join(build_dir, "obj_dir")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "build", "p0", "sim")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "rtl", "core")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "rtl", "common")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "build", "p0", "rtl")]
    cmd += ["-CFLAGS", "-I%s" % run_unit.SIM_COMMON]
    cmd += ["-CFLAGS", "-I%s" % os.path.join(REPO_ROOT, "build", "p0", "sim")]
    for define in defines:
        # Both halves, deliberately: a driver mutant is a C++ define and a
        # wrapper mutant is a SystemVerilog one, and the half that does not know
        # the name ignores it.
        cmd += ["-D%s" % define]
        cmd += ["-CFLAGS", "-D%s" % define]
    cmd += ["-o", os.path.join(build_dir, case_id)]
    cmd += sources

    result = run_unit.run(cmd)
    log = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0:
        raise RuntimeError("build failed for %s\n%s" % (defines, log))
    return log


def run_case(binary: str, case_id: str, out_dir: str):
    os.makedirs(out_dir, exist_ok=True)
    return subprocess.run(
        [binary, "--case", case_id, "--out", out_dir, "--seed", "1",
         "--max-cycles", "4000000"],
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )


def first_result_line(log: str) -> str:
    for line in log.splitlines():
        if line.startswith("RESULT "):
            return line
    return "(no RESULT line)"


def first_failure(log: str) -> str:
    for line in log.splitlines():
        if line.startswith("CHECK FAILED:"):
            return line
    return "(no CHECK FAILED line)"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", default="harness.sampling_calibration")
    parser.add_argument("--only", default=None)
    args = parser.parse_args()

    registry = run_unit.load_registry()["cases"]
    if args.case not in registry:
        print("no such case: %s" % args.case)
        return 2
    entry = registry[args.case]

    root = os.path.join(REPO_ROOT, "build", "p0", "sampling_controls")
    shipping_dir = os.path.join(root, "shipping")
    print("building the shipping case from an empty directory...")
    build(entry, args.case, [], shipping_dir)
    shipping = os.path.join(shipping_dir, args.case)
    result = run_case(shipping, args.case, os.path.join(root, "out-shipping"))
    baseline_ok = result.returncode == 0 and b"RESULT PASS" in result.stdout
    print("  baseline exit=%d %s" % (result.returncode,
                                     first_result_line(result.stdout.decode("utf-8", "replace"))))
    if not baseline_ok:
        print("BASELINE FAILED: the shipping build must pass before mutants mean anything")
        return 1

    failures = 0
    unobservable = 0
    print()
    print("%-46s %-6s %s" % ("mutant", "exit", "result"))
    print("-" * 118)
    for define, defect, expected in MUTANTS:
        if args.only is not None and args.only not in define:
            continue
        mutant_dir = os.path.join(root, define)
        try:
            build(entry, args.case, [define], mutant_dir)
        except RuntimeError as error:
            print("%-46s BUILD FAILED" % define)
            print(error)
            failures += 1
            continue
        mutant = os.path.join(mutant_dir, args.case)
        differs = subprocess.run(["cmp", "-s", shipping, mutant]).returncode != 0
        result = run_case(mutant, args.case, os.path.join(root, "out-" + define))
        log = result.stdout.decode("utf-8", "replace")
        caught = result.returncode == 1 and expected in first_failure(log)
        status = "OK" if (differs and caught) else "MISS"
        if not differs:
            status += " (binary identical to shipping)"
        if result.returncode != 1:
            status += " (exit %d)" % result.returncode
            if result.returncode == 0:
                unobservable += 1
        print("%-46s %-6d %s" % (define, result.returncode, status))
        print("    injects: %s" % defect)
        print("    %s" % first_failure(log))
        print("    %s" % first_result_line(log))
        if not (differs and caught):
            failures += 1

    print("-" * 118)
    if failures:
        print("%d mutant(s) were not caught as required" % failures)
        return 1
    print("all %d mutants mutate the binary, exit 1 and name the check they break"
          % (len(MUTANTS) if args.only is None else 1))
    if unobservable:
        print("%d mutant(s) exited 0 and are unobservable" % unobservable)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
