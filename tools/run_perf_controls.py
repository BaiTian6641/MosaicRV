#!/usr/bin/env python3
"""Negative controls for CASE=perf.equal_resource_compare (work package I-084).

The shipping build is built and run first, from an empty directory: a mutant
means nothing if the case does not pass in the first place. Each control is then
built from its own empty directory with one `-D` in the build command, and must

  * produce a binary whose sha256 differs from the shipping binary,
  * exit 1, and
  * name the check it breaks in the first failure.

The three controls exercise the three checks the package is *for*:

  MOSAIC_PERF_MUTANT_RESOURCE_DRIFT  the resource bundle the harness reads is
      drifted for every configuration after the first, as if the "dynamic" build
      had secretly been given a bigger ROB and a wider lane budget. The
      equal-resource assertion must catch it. It is a *driver* define (the one
      place a real geometry change could enter the comparison), so it is passed
      through `-CFLAGS` rather than to Verilator's RTL preprocessor.

  MOSAIC_FAB_MUTANT_DYN_SWAP_SRC  an RTL control that already exists (I-090):
      the dynamic route's two operand-value wires cross, so a non-commutative
      ALU op computes a different result in the dynamic configuration only. The
      architectural-identity check must catch it. `alu_chain` is built with a
      `sub` precisely so this control has something to break.

  MOSAIC_FAB_MUTANT_NO_DELTA  an RTL control that already exists (I-090): the
      strategy the core sees is held low, so the "+steering" configuration *is*
      the baseline. The engagement check must catch it -- a "configuration" whose
      feature did no work is not a configuration.

Usage: run_perf_controls.py [--profile p1] [--only SUBSTRING]
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
CASE_ID = "perf.equal_resource_compare"

# (define, kind, source, defect, expected first-failure text)
MUTANTS = [
    (
        "MOSAIC_PERF_MUTANT_RESOURCE_DRIFT",
        "driver",
        "sim/unit/tb_core_perf.cpp",
        "every configuration after the first is reported with a bigger ROB (64 -> "
        "80) and a wider lane budget (8 -> 12), as if the dynamic configuration "
        "had secretly been given more resources: the equal-resource assertion must "
        "catch it",
        "same resources",
    ),
    (
        "MOSAIC_FAB_MUTANT_DYN_SWAP_SRC",
        "rtl",
        "rtl/core/mosaic_dispatch.sv",
        "the dynamic route's two operand-value wires cross, so a non-commutative "
        "ALU op (`sub` in alu_chain) computes a different result in the dynamic "
        "configuration only: the architectural-identity check must catch it",
        "same architecture",
    ),
    (
        "MOSAIC_FAB_MUTANT_NO_DELTA",
        "rtl",
        "rtl/core/mosaic_core.sv",
        "the strategy the fabric sees is held low, so the '+steering' configuration "
        "is really the baseline: the engagement check must catch a configuration "
        "whose feature did no work",
        "actually engaged",
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
            # would only reach Verilator's RTL preprocessor and the mutant would
            # build the shipping binary under a mutant's name.
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
    max_cycles = entry.get("max_cycles", 4000000)

    for define, _kind, source, _defect, _expected in MUTANTS:
        path = os.path.join(REPO_ROOT, source)
        with open(path) as handle:
            text = handle.read()
        if text.count(define) < 1:
            print("FAIL: %s does not occur in %s" % (define, source))
            return 1

    root = os.path.join(REPO_ROOT, "build", args.profile, "perf_controls")
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
        print("%d mutant(s) were not caught as required" % failures)
        return 1
    print("all %d mutants mutate the binary, exit 1 and name the check they break"
          % (len(MUTANTS) if args.only is None else 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
