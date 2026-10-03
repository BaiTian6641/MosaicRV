#!/usr/bin/env python3
"""Negative controls for CASE=pmu.trace_accounting (work package I-076).

    python3 tools/run_pmu_controls.py --profile p1

Each control is the shipping case built from a *deleted* build directory with
exactly one `-D` in that build's command line. The script requires, for every
control:

  * a binary that differs from the shipping binary (`cmp -s` + sha256),
  * exit status exactly 1,
  * the expected first-failure text in the run log,

and prints a table, exactly as tools/run_perf_controls.py does for I-084. The
two controls are the card's own failure modes: double-counting a squashed uop as
retired, and under-counting the classification so the parts no longer sum to the
whole.

`MOSAIC_PMU_MUTANT_DOUBLE_COMMIT` is the defect the case's own fix removed from
the CSR counter's wiring (a single retire lane): it re-injects a counting error
in the retire domain. `MOSAIC_PMU_MUTANT_SKIP_CLASS` is a *driver* mutation of
the classification, the under-count the card names by example.
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
CASE_ID = "pmu.trace_accounting"

# (define, kind, source, defect, expected first-failure text)
MUTANTS = [
    (
        "MOSAIC_PMU_MUTANT_DOUBLE_COMMIT",
        "rtl",
        "rtl/core/mosaic_core.sv",
        "every redirect adds one to the committed-instruction counter, as if a "
        "squashed or replayed uop had retired: the retire-domain check must catch "
        "the double count",
        "retired counter",
    ),
    (
        "MOSAIC_PMU_MUTANT_SKIP_CLASS",
        "driver",
        "sim/unit/tb_pmu.cpp",
        "the classification drops the CTRL class, so the per-class parts no "
        "longer sum to the whole: the classification identity must catch the "
        "under count",
        "class sum",
    ),
]


def build(entry: dict, defines: list, profile: str, build_dir: str) -> str:
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
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "build", profile, "sim")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "rtl", "core")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "rtl", "common")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "build", profile, "rtl")]
    cmd += ["-CFLAGS", "-I%s" % run_unit.SIM_COMMON]
    cmd += ["-CFLAGS", "-I%s" % os.path.join(REPO_ROOT, "build", profile, "sim")]
    for define, kind, _source, _defect, _expected in MUTANTS:
        if define not in defines:
            continue
        if kind == "driver":
            # A driver define reaches the C++ half through -CFLAGS; a bare -D
            # would only reach Verilator's RTL preprocessor.
            cmd += ["-CFLAGS", "-D%s" % define]
        else:
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
        cwd=REPO_ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )


def sha256_of(path: str) -> str:
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def first_result_line(log: str) -> str:
    for line in log.splitlines():
        if line.startswith("RESULT "):
            return line
    return "(no RESULT line)"


def first_failure_line(log: str) -> str:
    for line in log.splitlines():
        if line.startswith("MISMATCH ") or line.startswith("CHECK FAILED:"):
            return line
    return "(no failure line)"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="p1")
    parser.add_argument("--only", default=None)
    args = parser.parse_args()

    registry = run_unit.load_registry()["cases"]
    if CASE_ID not in registry:
        print("no such case: %s" % CASE_ID)
        return 2
    entry = registry[CASE_ID]
    max_cycles = entry.get("max_cycles", 400000)

    for define, _kind, source, _defect, _expected in MUTANTS:
        path = os.path.join(REPO_ROOT, source)
        with open(path) as handle:
            text = handle.read()
        if text.count(define) < 1:
            print("FAIL: %s does not occur in %s" % (define, source))
            return 1

    root = os.path.join(REPO_ROOT, "build", args.profile, "pmu_controls")
    shipping_dir = os.path.join(root, "shipping")
    print("building the shipping case from an empty directory...")
    build(entry, [], args.profile, shipping_dir)
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
    print("%-40s %-5s %s" % ("mutant", "exit", "result"))
    print("-" * 110)
    for define, kind, _source, defect, expected in MUTANTS:
        if args.only is not None and args.only not in define:
            continue
        mutant_dir = os.path.join(root, define)
        try:
            build(entry, [define], args.profile, mutant_dir)
        except RuntimeError as error:
            print("%-40s BUILD FAILED" % define)
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
        print("%-40s %-5d %s" % (define, result.returncode, status))
        print("    kind:   %s" % kind)
        print("    injects: %s" % defect)
        print("    sha256:  %s" % mutant_hash)
        print("    build:   %s" % os.path.relpath(
            os.path.join(mutant_dir, "build_command.txt"), REPO_ROOT))
        print("    %s" % first_failure_line(log))
        if not (differs and caught):
            failures += 1

    print("-" * 110)
    if failures:
        print("%d control(s) did not fail as required" % failures)
        return 1
    print("all %d control(s) failed as required" % len(MUTANTS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
