#!/usr/bin/env python3
"""Negative controls for the integrated core's control path (I-023).

CASE=core.corpus_branch claims that a resolved branch reaches the redirect
arbiter, that the arbiter's winner redirects the front end, purges the younger
work it discards, and drives rename's squash -- and that the machine retires the
corpus program's instructions and nothing else. A claim like that is evidence
only if the defect it excludes would be caught, so this tool rebuilds the case
with exactly one defect injected through a `-DMOSAIC_*_MUTANT_*` define, from an
empty build directory so a mutant can never reuse the shipping object files, and
runs it. Every mutant must:

  * build cleanly into its own directory;
  * produce a binary that differs from the shipping one (`cmp`), so a stale
    build -- or a `-D` that never reached the compiler -- cannot masquerade as a
    mutant;
  * exit 1, with the named check the mutation breaks as the first failure.

The shipping build is built and run first, from an empty directory: mutants mean
nothing if the case does not pass in the first place.

Usage: run_corpus_branch_controls.py [--only SUBSTRING]
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
CASE = "core.corpus_branch"

# Each mutant: the define, the defect it injects, the text of the first failure
# the mutation must produce, and whether this case is expected to observe it. A
# control that the case cannot observe is reported NOT MET by name and counted
# separately -- an unobservable defect is a coverage gap in the case, and the
# honest response is to say so rather than to drop the control or to count it.
MUTANTS = [
    (
        "MOSAIC_FETCH_MUTANT_OUT_REG_NO_REDIRECT_FLUSH",
        "a redirect does not purge the younger work it discards: the fetched "
        "instruction the output register holds survives the redirect, reaches "
        "the decoder and retires -- a squashed instruction retiring",
        "the retirement stream follows the reference",
        True,
    ),
    (
        "MOSAIC_CORE_MUTANT_REDIRECT_NEXT",
        "the front end resumes one instruction past the redirect target, so the "
        "first instruction of the resolved branch's path is skipped",
        "the front end fetches the redirect's target",
        True,
    ),
    (
        "MOSAIC_CLUSTER_MUTANT_IGNORE_TAKEN",
        "a branch resolution is raised as not taken whatever the comparator "
        "said, so the arbiter never redirects and the wrong path keeps running",
        "the arbiter issued 0 redirects",
        True,
    ),
    (
        "MOSAIC_CORE_MUTANT_NO_PURGE",
        "the redirect does not purge the decode buffer, so the fall-through "
        "instructions already decoded behind the branch are dispatched after "
        "the redirect and the machine leaves the program's path",
        "the arbiter issued 1 redirects",
        True,
    ),
    (
        "MOSAIC_CORE_MUTANT_EARLY_CKPT",
        "the rename checkpoint is taken in the cycle the arbiter acts, before "
        "the redirecting branch's own commit has landed, so the recovery point "
        "is not at a committed boundary and rename refuses the squash",
        "the divergent-recovery counters stay zero",
        True,
    ),
    (
        "MOSAIC_DISPATCH_MUTANT_BANK_CONFLICT_VALUE",
        "two source operands that share a register-file bank are read from one "
        "bank port, so the second operand silently becomes the first and every "
        "branch on such a pair resolves the wrong way",
        "the redirect the arbiter issues names the taken transfer's target",
        True,
    ),
    (
        "MOSAIC_CORE_MUTANT_EARLY_BARRIER_RELEASE",
        "the branch barrier is released when the branch resolves rather than "
        "when the arbiter acts, so younger work is allocated and dispatched while "
        "the redirect is still pending -- the state the barrier exists to make "
        "impossible",
        "an empty ROB is at a rename boundary",
        True,
    ),
    (
        "MOSAIC_FETCH_MUTANT_OUT_REG_DRAG",
        "a dropped (stale) response in the same cycle as the output register's "
        "drain suppresses the drain, so the instruction in the register is "
        "delivered again and again",
        "(this case does not observe the mutation; see I-023-branches.md)",
        False,
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
        # The mutation is in the RTL, so the define goes to Verilator.
        cmd += ["-D%s" % define]
    cmd += ["-o", os.path.join(build_dir, case_id)]
    cmd += sources

    result = run_unit.run(cmd)
    log = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0:
        raise RuntimeError("build failed for %s\n%s" % (defines, log))
    return log


def run_case(binary: str, case_id: str, out_dir: str, max_cycles: int):
    os.makedirs(out_dir, exist_ok=True)
    return subprocess.run(
        [binary, "--case", case_id, "--out", out_dir, "--seed", "1",
         "--max-cycles", str(max_cycles)],
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
    parser.add_argument("--only", default=None)
    args = parser.parse_args()

    registry = run_unit.load_registry()["cases"]
    if CASE not in registry:
        print("no such case: %s" % CASE)
        return 2
    entry = registry[CASE]
    max_cycles = entry.get("max_cycles", 200000)

    root = os.path.join(REPO_ROOT, "build", "p0", "corpus_branch_controls")
    shipping_dir = os.path.join(root, "shipping")
    print("building the shipping case from an empty directory...")
    build(entry, CASE, [], shipping_dir)
    shipping = os.path.join(shipping_dir, CASE)
    result = run_case(shipping, CASE, os.path.join(root, "out-shipping"), max_cycles)
    baseline_ok = result.returncode == 0 and b"RESULT PASS" in result.stdout
    print("  baseline exit=%d %s" % (
        result.returncode,
        first_result_line(result.stdout.decode("utf-8", "replace"))))
    if not baseline_ok:
        print("BASELINE FAILED: the shipping build must pass before mutants mean "
              "anything")
        return 1

    failures = 0
    documented = 0
    print()
    print("%-52s %-5s %s" % ("mutant", "exit", "result"))
    print("-" * 118)
    for define, defect, expected, observable in MUTANTS:
        if args.only is not None and args.only not in define:
            continue
        mutant_dir = os.path.join(root, define)
        try:
            build(entry, CASE, [define], mutant_dir)
        except RuntimeError as error:
            print("%-52s BUILD FAILED" % define)
            print(error)
            failures += 1
            continue
        mutant = os.path.join(mutant_dir, CASE)
        differs = subprocess.run(["cmp", "-s", shipping, mutant]).returncode != 0
        result = run_case(mutant, CASE, os.path.join(root, "out-" + define),
                          max_cycles)
        log = result.stdout.decode("utf-8", "replace")
        caught = result.returncode == 1 and expected in log
        status = "OK" if (differs and caught) else "MISS"
        if not differs:
            status += " (binary identical to shipping)"
        if result.returncode == 0:
            status += " (exit 0: unobservable or no check can fail)"
        elif result.returncode != 1:
            status += " (exit %d)" % result.returncode
        if differs and result.returncode == 1 and not caught:
            status += " (fails, but not on %r)" % expected
        print("%-52s %-5d %s" % (define, result.returncode, status))
        print("%-52s       %s" % ("", first_result_line(log)))
        if differs and not observable:
            # The mutation reaches the binary but this case cannot observe it:
            # recorded, not counted, and never presented as evidence.
            documented += 1
            print("%-52s       DOCUMENTED UNOBSERVABLE: %s" % ("", defect))
            continue
        if not (differs and caught):
            print("%-52s       defect: %s" % ("", defect))
            failures += 1

    print()
    if documented:
        print("%d control(s) documented as unobservable in this case (see "
              "results/reports/I-023-branches.md)" % documented)
    if failures:
        print("%d control(s) NOT MET" % failures)
        return 1
    print("all observable controls OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
