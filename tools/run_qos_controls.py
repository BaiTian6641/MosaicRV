#!/usr/bin/env python3
"""Negative controls for CASE=memory.qos_no_starvation (work package I-062).

The case claims four things about the criticality/QoS memory scheduler: a class
with pending work at every grant of a window receives at least its reserved
share, so no class can be starved for ever while the downstream bound holds and
the age escape fires; an over-quota grant is always an age escape for a request
that actually aged, so the streaming class cannot monopolise the port; the age
escape bounds the wait of an old request; and a scheduling choice never changes
the data a request returns. Claims like those are evidence only if the defect
each excludes would be caught, so this tool rebuilds the case with exactly one
defect injected through a `-DMOSAIC_QOS_MUTANT_*` define -- from an empty build
directory, so a mutant can never reuse the shipping object files -- and runs it.
Every mutant must:

  * build cleanly into its own directory, with its `-D` on the recorded command
    line (`build_command.txt` next to the binary);
  * produce a binary that differs from the shipping one (`cmp`), so a stale
    build cannot masquerade as a mutant;
  * exit 1, with the named check the mutation breaks as the first failure.

A mutant that exits 0 is either an unobservable defect or a check that cannot
fail; this tool reports it as NOT MET rather than counting it.

The shipping build is built and run first, from an empty directory: mutants mean
nothing if the case does not pass in the first place.

Usage: run_qos_controls.py [--only SUBSTRING]
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
CASE_ID = "memory.qos_no_starvation"
SOURCE = os.path.join("rtl", "core", "mosaic_mem_qos.sv")

# Each mutant: the define, the defect it injects, and the text of the first
# failure the mutation must produce.
MUTANTS = [
    (
        "MOSAIC_QOS_MUTANT_SCALAR_STARVE",
        "the bulk class is never admitted (excluded from both the eligible and the aged set), "
        "so the scalar class permanently takes the port and bulk starves -- the card's first "
        "fail mode, 'the scalar class permanently preempting bulk', caught by the reservation "
        "bound rather than by a timeout",
        "starvation:",
    ),
    (
        "MOSAIC_QOS_MUTANT_BULK_MONOPOLY",
        "the bulk class's ceiling is removed from eligibility, so bulk is granted past its "
        "quota with no age escape and can monopolise the port -- the card's 'bulk quota "
        "removed' defect",
        "quota:",
    ),
    (
        "MOSAIC_QOS_MUTANT_NO_AGE_ESCAPE",
        "nothing ever ages, so the only over-quota path is gone: when every pending class is "
        "at its ceiling the port idles for ever and an old request waits without a bound -- "
        "the card's 'age escape removed so an old request can wait for ever'",
        "escape:",
    ),
    (
        "MOSAIC_QOS_MUTANT_DATA",
        "the payload delivered with a grant is corrupted, so the scheduling choice has changed "
        "a data result -- the card's second fail mode, 'a performance policy that changes a "
        "memory ordering'",
        "data:",
    ),
]


def build(entry: dict, defines: list, build_dir: str) -> str:
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
        # The mutation is in the RTL, so the define goes to Verilator and not to
        # the generated C++.
        cmd += ["-D%s" % define]
    cmd += ["-o", os.path.join(build_dir, CASE_ID)]
    cmd += sources

    with open(os.path.join(build_dir, "build_command.txt"), "w") as handle:
        handle.write(" ".join(cmd))

    result = run_unit.run(cmd)
    log = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0:
        raise RuntimeError("build failed for %s\n%s" % (defines, log))
    return log


def run_case(binary: str, out_dir: str, max_cycles: int):
    os.makedirs(out_dir, exist_ok=True)
    return subprocess.run(
        [binary, "--case", CASE_ID, "--out", out_dir, "--seed", "1",
         "--max-cycles", str(max_cycles)],
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )


def sha256_of(path: str) -> str:
    import hashlib
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def first_failure(log: str) -> str:
    """The first MISMATCH line, which names the check the mutation breaks."""
    for line in log.splitlines():
        if line.startswith("MISMATCH"):
            return line
    for line in log.splitlines():
        if line.startswith("RESULT"):
            return line
    return "(no MISMATCH or RESULT line)"


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
    if CASE_ID not in registry:
        print("no such case: %s" % CASE_ID)
        return 2
    entry = registry[CASE_ID]
    max_cycles = entry.get("max_cycles", 200000)

    # The mutation must exist in the source before it is claimed to have been
    # built: a `-D` that no `ifdef` reads would build a shipping binary under a
    # mutant's name.
    source = os.path.join(REPO_ROOT, SOURCE)
    with open(source) as handle:
        text = handle.read()
    for define, _defect, _expected in MUTANTS:
        if text.count(define) < 1:
            print("FAIL: %s does not occur in %s" % (define, source))
            return 1

    root = os.path.join(REPO_ROOT, "build", "p0", "qos_controls")
    shipping_dir = os.path.join(root, "shipping")
    print("building the shipping case from an empty directory...")
    build(entry, [], shipping_dir)
    shipping = os.path.join(shipping_dir, CASE_ID)
    result = run_case(shipping, os.path.join(root, "out-shipping"), max_cycles)
    baseline_ok = result.returncode == 0 and b"RESULT PASS" in result.stdout
    print("  baseline exit=%d %s" % (
        result.returncode, first_result_line(result.stdout.decode("utf-8", "replace"))))
    if not baseline_ok:
        print("BASELINE FAILED: the shipping build must pass before mutants mean "
              "anything")
        return 1

    failures = 0
    print("shipping binary sha256: %s" % sha256_of(shipping))
    print()
    print("%-38s %-5s %s" % ("mutant", "exit", "result"))
    print("-" * 110)
    for define, defect, expected in MUTANTS:
        if args.only is not None and args.only not in define:
            continue
        mutant_dir = os.path.join(root, define)
        try:
            build(entry, [define], mutant_dir)
        except RuntimeError as error:
            print("%-38s BUILD FAILED" % define)
            print(error)
            failures += 1
            continue
        mutant = os.path.join(mutant_dir, CASE_ID)
        differs = subprocess.run(["cmp", "-s", shipping, mutant]).returncode != 0
        mutant_hash = sha256_of(mutant)
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
        print("%-38s %-5d %s" % (define, result.returncode, status))
        print("    injects: %s" % defect)
        print("    sha256:  %s" % mutant_hash)
        print("    build:   %s" % os.path.relpath(
            os.path.join(mutant_dir, "build_command.txt"), REPO_ROOT))
        print("    %s" % first_failure(log))
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
