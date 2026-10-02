#!/usr/bin/env python3
"""Negative controls for CASE=irq.replay_timeline (work package V-015).

The card names three fail modes for the dynamic-interrupt timeline, and all three
are properties of *this case's own comparison* rather than of one RTL edit, so
all three are injected here with `-CFLAGS -D...` into the driver:

  1. a stimulus whose timing comes from host wall time rather than from the
     machine, so the same program and the same seed do not produce the same
     boundaries on the second run;
  2. an interrupt treated as an ordinary retiring instruction, so the checker
     expects the interrupted instruction to retire in the trap cycle;
  3. a pending interrupt lost at a boundary instead of staying pending, so the
     checker believes an interrupt asserted while masked is never taken.

Each mutant is built from a deleted build directory with its `-D` in that
build's own `build_command.txt`, its binary is required to differ from the
shipping one, and it must exit 1 with the named first failure below.

Usage: run_irq_controls.py [--only NAME]
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_unit  # noqa: E402

REPO_ROOT = run_unit.REPO_ROOT
CASE = "irq.replay_timeline"

# (define, kind, fail mode, defect, text the first failure must contain)
# kind: "rtl" -> -D to Verilator, "driver" -> -CFLAGS -D to the C++ half.
MUTANTS = [
    (
        "MOSAIC_IRQ_MUTANT_WALLCLOCK",
        "driver",
        "1. a stimulus timed by host wall time",
        "the stimulus is delayed by a host-clock-derived number of cycles and the "
        "host clock is mixed into the per-cycle trace, so the second run of the "
        "same program and seed reaches different boundaries.",
        "the same stimulus replays",
    ),
    (
        "MOSAIC_IRQ_MUTANT_ORDINARY_RETIRE",
        "driver",
        "2. an interrupt treated as an ordinary retiring instruction",
        "the checker requires the interrupted instruction to appear in the "
        "retirement stream in the interrupt's trap cycle, which is what a machine "
        "that let the interrupt leak into the retire stream would produce.",
        "an interrupt is not an instruction",
    ),
    (
        "MOSAIC_IRQ_MUTANT_LOST_PENDING",
        "driver",
        "3. a lost unaccepted interrupt",
        "the checker believes an interrupt asserted while masked is dropped at the "
        "boundary, so it requires that no acceptance follows the enable CSR; the "
        "machine takes it and the check fails.",
        "an unaccepted interrupt is not lost",
    ),
]


def build(entry: dict, case_id: str, mutants: list, build_dir: str) -> str:
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
    for define, kind, _mode, _defect, _expected in mutants:
        if kind == "rtl":
            cmd += ["-D%s" % define]
        else:
            cmd += ["-CFLAGS", "-D%s" % define]
    cmd += ["-o", os.path.join(build_dir, case_id)]
    cmd += sources

    with open(os.path.join(build_dir, "build_command.txt"), "w") as handle:
        handle.write(" ".join(cmd))

    result = run_unit.run(cmd)
    log = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0:
        raise RuntimeError("build failed for %s\n%s" % (mutants, log))
    return log


def run_case(binary: str, out_dir: str, max_cycles: int) -> subprocess.CompletedProcess:
    os.makedirs(out_dir, exist_ok=True)
    return subprocess.run(
        [binary, "--case", CASE, "--out", out_dir, "--seed", "1",
         "--max-cycles", str(max_cycles)],
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )


def sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def first_line(log: str, prefix: str) -> str:
    for line in log.splitlines():
        if line.startswith(prefix):
            return line
    return "(no %s line)" % prefix


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", default=None)
    args = parser.parse_args()

    registry = run_unit.load_registry()["cases"]
    if CASE not in registry:
        print("no such case: %s" % CASE)
        return 2
    entry = registry[CASE]
    max_cycles = entry.get("max_cycles", 4000000)

    root = os.path.join(REPO_ROOT, "build", "p0", "irq_controls")
    shipping_dir = os.path.join(root, "shipping")
    print("building the shipping case from an empty directory...")
    build(entry, CASE, [], shipping_dir)
    shipping = os.path.join(shipping_dir, CASE)
    result = run_case(shipping, os.path.join(root, "out-shipping"), max_cycles)
    baseline_ok = result.returncode == 0 and b"RESULT PASS" in result.stdout
    print("  baseline exit=%d %s" % (
        result.returncode, first_line(result.stdout.decode("utf-8", "replace"),
                                      "RESULT ")))
    if not baseline_ok:
        print("BASELINE FAILED: the shipping build must pass before mutants mean "
              "anything")
        return 1

    shipping_hash = sha256(shipping)
    print("shipping binary sha256: %s" % shipping_hash)
    print()
    print("%-40s %-7s %-5s %s" % ("mutant", "kind", "exit", "result"))
    print("-" * 110)
    failures = 0
    ran = 0
    for define, kind, mode, defect, expected in MUTANTS:
        if args.only is not None and args.only not in define:
            continue
        ran += 1
        mutant_dir = os.path.join(root, define)
        try:
            build(entry, CASE, [(define, kind, mode, defect, expected)], mutant_dir)
        except RuntimeError as error:
            print("%-40s %-7s BUILD FAILED" % (define, kind))
            print(error)
            failures += 1
            continue
        mutant = os.path.join(mutant_dir, CASE)
        differs = subprocess.run(["cmp", "-s", shipping, mutant]).returncode != 0
        result = run_case(mutant, os.path.join(root, "out-" + define), max_cycles)
        log = result.stdout.decode("utf-8", "replace")
        caught = result.returncode == 1 and expected in log
        status = "OK" if (differs and caught) else "MISS"
        if not differs:
            status += " (binary identical to shipping)"
        if result.returncode == 0:
            status += " (exit 0: unobservable or no check can fail)"
        elif result.returncode != 1:
            status += " (exit %d)" % result.returncode
        if result.returncode == 1 and not caught:
            status += " (expected %r)" % expected
        print("%-40s %-7s %-5d %s" % (define, kind, result.returncode, status))
        print("    fail mode: %s" % mode)
        print("    injects:   %s" % defect)
        print("    sha256:    %s" % sha256(mutant))
        print("    build:     %s" % os.path.relpath(
            os.path.join(mutant_dir, "build_command.txt"), REPO_ROOT))
        print("    %s" % first_line(log, "RESULT "))
        print("    first:     %s" % first_line(log, "MISMATCH "))
        if not (differs and caught):
            failures += 1

    print("-" * 110)
    if failures:
        print("%d of %d mutant(s) were not caught as required" % (failures, ran))
        return 1
    print("all %d mutants mutate the binary, exit 1 and name the check they break"
          % ran)
    return 0


if __name__ == "__main__":
    sys.exit(main())
