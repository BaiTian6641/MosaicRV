#!/usr/bin/env python3
"""Negative controls for CASE=multihart.isolation (work package I-064).

The case claims two architectural domains share one physical machine without
corrupting each other: each hart's concurrent retirement stream equals its own
solo reference, one hart's fault and redirect do not disturb the other, no
response is confused by a hart ID, and both harts really use the one shared
memory service. A claim like that is evidence only if the defect it excludes
would be caught, so this tool rebuilds the case from an empty directory with
exactly one defect injected and runs it. Every mutant must:

  * build cleanly into its own directory, with its `-D` on the recorded command
    line (`build_command.txt` next to the binary);
  * produce a binary that differs from the shipping one (`cmp`), so a stale
    build cannot masquerade as a mutant;
  * exit 1, with the named check the mutation breaks present in the output.

A mutant that exits 0 is either an unobservable defect or a check that cannot
fail; this tool reports it as NOT MET rather than counting it.

The shipping build is built and run first, from an empty directory: mutants mean
nothing if the case does not pass in the first place.

Usage: run_multihart_controls.py [--only SUBSTRING]
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
CASE_ID = "multihart.isolation"
PROFILE = "p3"

# Each RTL mutant: the define, the file that carries it, the defect it injects,
# and the text of the failure the mutation must produce.
MUTANTS = [
    (
        "MOSAIC_MH_MUTANT_IGNORE_HART",
        "rtl/core/mosaic_multihart.sv",
        "the response matcher ignores the hart and routes every response to hart "
        "0's slot of that source, so a hart-1 response can be delivered to hart 1 "
        "only if hart 0 was not expecting one -- the card's 'hart-ID reuse "
        "confuses a response' fail mode, injected at the matcher itself",
        "equals its own reference",
    ),
    (
        "MOSAIC_MH_MUTANT_REUSE_TAG",
        "rtl/core/mosaic_multihart.sv",
        "every request is tagged hart 0, so a response for hart 1's request comes "
        "back naming hart 0's slot and is either dropped as an ownership mismatch "
        "or delivered to the wrong hart: hart 1 never completes",
        "retired its program and parked",
    ),
    (
        "MOSAIC_MH_MUTANT_GLOBAL_REDIRECT",
        "rtl/core/mosaic_multihart.sv",
        "hart 0's redirect resets hart 1 -- a global redirect clearing the other "
        "hart, the card's first named fail mode",
        "equals its own reference",
    ),
    (
        "MOSAIC_MH_MUTANT_ONE_HART_BUS",
        "rtl/core/mosaic_multihart.sv",
        "the arbiter never grants hart 1, so the second hart's requests never "
        "reach the shared service: the sharing claim is false and hart 1 stops "
        "making progress",
        "both harts used the one shared memory service",
    ),
]

# The one driver-side control: compare each hart against the other hart's
# reference. The two programs differ, so the comparison must fail -- the
# falsifiability control on the per-hart reference check.
DRIVER_CONTROLS = [
    ("--control-swap-ref", "each hart is compared against the other hart's reference",
     "equals its own reference"),
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
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "build", PROFILE, "sim")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "rtl", "core")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "rtl", "common")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "build", PROFILE, "rtl")]
    cmd += ["-CFLAGS", "-I%s" % run_unit.SIM_COMMON]
    cmd += ["-CFLAGS", "-I%s" % os.path.join(REPO_ROOT, "build", PROFILE, "sim")]
    for define in defines:
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


def run_case(binary: str, out_dir: str, max_cycles: int, extra: list = None):
    os.makedirs(out_dir, exist_ok=True)
    cmd = [binary, "--case", CASE_ID, "--out", out_dir, "--seed", "1",
           "--max-cycles", str(max_cycles)]
    if extra:
        cmd += extra
    return subprocess.run(cmd, cwd=REPO_ROOT, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)


def sha256_of(path: str) -> str:
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


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
    for define, source, _defect, _expected in MUTANTS:
        path = os.path.join(REPO_ROOT, source)
        with open(path) as handle:
            text = handle.read()
        if text.count(define) < 1:
            print("FAIL: %s does not occur in %s" % (define, source))
            return 1

    root = os.path.join(REPO_ROOT, "build", PROFILE, "multihart_controls")
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
    print("%-44s %-5s %s" % ("mutant", "exit", "result"))
    print("-" * 110)
    for define, _source, defect, expected in MUTANTS:
        if args.only is not None and args.only not in define:
            continue
        mutant_dir = os.path.join(root, define)
        try:
            build(entry, [define], mutant_dir)
        except RuntimeError as error:
            print("%-44s BUILD FAILED" % define)
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
        print("%-44s %-5d %s" % (define, result.returncode, status))
        print("    injects: %s" % defect)
        print("    sha256:  %s" % mutant_hash)
        print("    build:   %s" % os.path.relpath(
            os.path.join(mutant_dir, "build_command.txt"), REPO_ROOT))
        print("    %s" % first_result_line(log))
        if not (differs and caught):
            failures += 1

    # The driver-side control runs the *shipping* binary with one extra flag.
    for flag, defect, expected in DRIVER_CONTROLS:
        if args.only is not None and args.only not in flag:
            continue
        result = run_case(shipping, os.path.join(root, "out" + flag), max_cycles,
                          extra=[flag])
        log = result.stdout.decode("utf-8", "replace")
        caught = result.returncode == 1 and expected in log
        status = "OK" if caught else "MISS"
        if result.returncode == 0:
            status += " (exit 0: unobservable or no check can fail)"
        elif result.returncode != 1:
            status += " (exit %d)" % result.returncode
        if result.returncode == 1 and not caught:
            status += " (expected %r)" % expected
        print("%-44s %-5d %s" % (flag, result.returncode, status))
        print("    injects: %s" % defect)
        print("    binary:  the shipping binary (a driver-side control)")
        print("    %s" % first_result_line(log))
        if not caught:
            failures += 1

    print("-" * 110)
    if failures:
        print("%d control(s) were not caught as required" % failures)
        return 1
    print("all controls mutate the binary or the reference, exit 1 and name the "
          "check they break")
    return 0


if __name__ == "__main__":
    sys.exit(main())
