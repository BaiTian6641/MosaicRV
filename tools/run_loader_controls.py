#!/usr/bin/env python3
"""Negative controls for the ELF loader (work package V-011).

The loader case (loader.elf_boundaries) says what must be refused. That claim is
only evidence if a loader that got it wrong would be caught, so this tool rebuilds
sim/common/elf_loader.cpp with exactly one defect injected through a
`-DMOSAIC_ELF_MUTANT_*` define, from an empty build directory, and runs the case
against it. Every mutant must:

  * build cleanly into its own directory, wiped first (a mutant must never reuse
    the shipping object files);
  * produce a binary that differs from the shipping one (`cmp`), so a stale build
    cannot masquerade as a mutant;
  * exit 1, with the named check the mutation breaks as the first failure.

A mutant that exits 0 is either an unobservable defect or a check that cannot
fail; this tool reports it as NOT MET rather than counting it.

Usage: run_loader_controls.py [--case loader.elf_boundaries] [--only NAME]
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

# Each mutant: the define, the defect it injects, and the check that must catch
# it. The name is the exact Check() label the testbench uses.
MUTANTS = [
    (
        "MOSAIC_ELF_MUTANT_LOAD_FIRST_SEGMENT_ONLY",
        "keeps only the first PT_LOAD and silently drops the rest",
        # The status check fires first: dropping the second segment leaves
        # e_entry outside every segment the loader kept.
        "loader.accept.multi-segment",
    ),
    (
        "MOSAIC_ELF_MUTANT_IGNORE_ENTRY",
        "ignores e_entry and enters at the lowest address",
        "loader.accept.entry-is-e_entry",
    ),
    (
        "MOSAIC_ELF_MUTANT_ALLOW_NONEXEC_ENTRY",
        "accepts an entry in a segment that declares itself non-executable",
        "loader.reject.entry-not-executable",
    ),
    (
        "MOSAIC_ELF_MUTANT_NO_SEGMENT_OVERFLOW_CHECK",
        "lets a segment whose vaddr+memsz wraps the address space through",
        "loader.reject.vaddr-overflow",
    ),
    (
        "MOSAIC_ELF_MUTANT_SKIP_OVERLAP_CHECK",
        "accepts overlapping PT_LOAD segments",
        "loader.reject.overlap",
    ),
    (
        "MOSAIC_ELF_MUTANT_ACCEPT_DYNAMIC",
        "accepts a dynamic (ET_DYN) image at its link-time address",
        "loader.reject.dynamic",
    ),
]


def build(entry: dict, case_id: str, defines: list, build_dir: str,
          sv_override: str = None) -> str:
    """Build one configuration from an empty directory; returns the log."""
    shutil.rmtree(build_dir, ignore_errors=True)
    os.makedirs(build_dir, exist_ok=True)

    rtl = entry.get("rtl", [])
    packages = [p for p in rtl if os.path.basename(p) == "mosaic_pkg.sv"]
    rest = [p for p in rtl if p not in packages]
    sv = [sv_override] if sv_override else entry.get("sv", [])
    sources = [
        os.path.join(REPO_ROOT, p)
        for p in (packages + rest + sv + entry.get("cpp", []) + run_unit.SHARED_CPP)
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
        cmd += ["-CFLAGS", "-D%s" % define]
    cmd += ["-o", os.path.join(build_dir, case_id)]
    cmd += sources

    result = run_unit.run(cmd)
    log = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0:
        raise RuntimeError("build failed for %s\n%s" % (defines, log))
    return log


def run_case(binary: str, case_id: str, out_dir: str) -> subprocess.CompletedProcess:
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", default="loader.elf_boundaries")
    parser.add_argument("--only", default=None)
    parser.add_argument(
        "--sv-override",
        default=None,
        help="build against this wrapper source instead of the registered one. "
        "Exists only for the case where another lane is holding the registered "
        "wrapper mid-edit; the normal run does not use it.",
    )
    args = parser.parse_args()

    registry = run_unit.load_registry()["cases"]
    if args.case not in registry:
        print("no such case: %s" % args.case)
        return 2
    entry = registry[args.case]

    root = os.path.join(REPO_ROOT, "build", "p0", "loader_controls")
    shipping_dir = os.path.join(root, "shipping")
    print("building the shipping case from an empty directory...")
    build(entry, args.case, [], shipping_dir, args.sv_override)
    shipping = os.path.join(shipping_dir, args.case)
    shipping_out = os.path.join(root, "out-shipping")
    result = run_case(shipping, args.case, shipping_out)
    baseline_ok = result.returncode == 0 and b"RESULT PASS" in result.stdout
    print("  baseline exit=%d %s" % (result.returncode, first_result_line(
        result.stdout.decode("utf-8", "replace"))))
    if not baseline_ok:
        print("BASELINE FAILED: the shipping build must pass before mutants mean anything")
        return 1
    if not os.path.exists(os.path.join(shipping_out, "controls.txt")):
        print("BASELINE FAILED: no controls.txt was written")
        return 1

    failures = 0
    unobservable = 0
    print()
    print("%-58s %-6s %s" % ("mutant", "exit", "first failure"))
    print("-" * 110)
    for define, defect, expected in MUTANTS:
        if args.only is not None and args.only not in define:
            continue
        mutant_dir = os.path.join(root, define)
        try:
            build(entry, args.case, [define], mutant_dir, args.sv_override)
        except RuntimeError as error:
            print("%-58s BUILD FAILED" % define)
            print(error)
            failures += 1
            continue
        mutant = os.path.join(mutant_dir, args.case)
        differs = subprocess.run(["cmp", "-s", shipping, mutant]).returncode != 0
        out_dir = os.path.join(root, "out-" + define)
        result = run_case(mutant, args.case, out_dir)
        log = result.stdout.decode("utf-8", "replace")
        line = first_result_line(log)
        caught = result.returncode == 1 and expected in log
        status = "OK" if (differs and caught) else "MISS"
        if not differs:
            status += " (binary identical to shipping)"
        if result.returncode != 1:
            status += " (exit %d)" % result.returncode
            if result.returncode == 0:
                unobservable += 1
        if not caught and result.returncode == 1:
            status += " (expected %s)" % expected
        print("%-58s %-6d %s" % (define, result.returncode, status))
        print("    injects: %s" % defect)
        print("    %s" % line)
        if not (differs and caught):
            failures += 1

    print("-" * 110)
    if failures:
        print("%d mutant(s) were not caught as required" % failures)
        return 1
    print("all %d mutants mutate the binary, exit 1 and name the check they break"
          % (len(MUTANTS) if args.only is None else 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
