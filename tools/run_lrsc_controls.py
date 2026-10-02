#!/usr/bin/env python3
"""Negative controls for the LR/SC reservation path (I-040).

    python3 tools/run_lrsc_controls.py [--only <define>]

Each control builds CASE=lrsc.reservation_progress from a **deleted** build
directory with its own `-D` on the Verilator command line, runs it, and requires
the binary to differ from the shipping one, to exit 1, and to name the failure
the mutation is supposed to produce. A control the case cannot observe is
reported NOT MET by name.

The four mutations the work package names, and the defect each injects:

  * `MOSAIC_LRSC_MUTANT_SC_WRITES_ON_FAIL` -- the store-conditional's refusal is
    removed: an SC whose reservation is gone performs its write anyway and
    reports success. "An SC with no reservation does not write" is the first
    sentence of the card's Pass criterion, and this is the mutant that denies it.
  * `MOSAIC_LRSC_MUTANT_SC_DOUBLE_WRITE` -- a successful SC performs its write a
    second time. The final memory value is unchanged (the second write stores the
    same data), which is exactly why the case counts write beats per SC rather
    than comparing only the end state.
  * `MOSAIC_LRSC_MUTANT_NO_EXT_INVAL` -- another agent's write to the granule
    never invalidates the reservation, so a conflicting write is invisible and
    the SC succeeds on state the architecture says is gone.
  * `MOSAIC_LRSC_MUTANT_GRANULE_OVERINVALIDATE` -- any write clears the
    reservation, whatever its address. The ISA permits an SC to fail for any
    reason, but this implementation declares a granule, and over-invalidation
    makes an out-of-granule store observable as a spurious failure.

The table with the real commands, binary hashes and exit codes is in
results/reports/I-040-lrsc.md.
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
CASE = "lrsc.reservation_progress"

# (define, defect, text the first failure must contain, observable?)
MUTANTS = [
    (
        "MOSAIC_LRSC_MUTANT_SC_WRITES_ON_FAIL",
        "the endpoint's refusal of an SC whose reservation is gone is removed, so "
        "the store-conditional performs its write and reports success",
        "the ISA requires",
        True,
    ),
    (
        "MOSAIC_LRSC_MUTANT_SC_DOUBLE_WRITE",
        "a successful SC is sent round for a second write beat: one architectural "
        "write, two memory writes",
        "exactly one write beat per successful SC",
        True,
    ),
    (
        "MOSAIC_LRSC_MUTANT_NO_EXT_INVAL",
        "another agent's write to the granule never clears the reservation, so a "
        "conflicting write is invisible and the SC succeeds on state the "
        "architecture says is gone",
        "external writes must clear the reservation",
        True,
    ),
    (
        "MOSAIC_LRSC_MUTANT_GRANULE_OVERINVALIDATE",
        "any write clears the reservation whatever its address, so a store outside "
        "the declared granule produces a spurious SC failure",
        "the ISA requires",
        True,
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


def first_failure_line(log: str) -> str:
    for line in log.splitlines():
        if line.startswith("MISMATCH ") or line.startswith("CHECK FAILED"):
            return line
    return "(no failure line)"


def sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


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

    root = os.path.join(REPO_ROOT, "build", "p0", "lrsc_controls")
    shipping_dir = os.path.join(root, "shipping")
    print("building the shipping case from an empty directory...")
    build(entry, CASE, [], shipping_dir)
    shipping = os.path.join(shipping_dir, CASE)
    result = run_case(shipping, CASE, os.path.join(root, "out-shipping"), max_cycles)
    baseline_ok = result.returncode == 0 and b"RESULT PASS" in result.stdout
    print("  baseline exit=%d %s" % (
        result.returncode,
        first_result_line(result.stdout.decode("utf-8", "replace"))))
    print("  shipping sha256=%s" % sha256(shipping))
    if not baseline_ok:
        print("BASELINE FAILED: the shipping build must pass before mutants mean "
              "anything")
        return 1

    failures = 0
    documented = 0
    print()
    print("%-46s %-5s %-9s %s" % ("mutant", "exit", "binary", "result"))
    print("-" * 118)
    for define, defect, expected, observable in MUTANTS:
        if args.only is not None and args.only not in define:
            continue
        mutant_dir = os.path.join(root, define)
        try:
            build(entry, CASE, [define], mutant_dir)
        except RuntimeError as error:
            print("%-46s BUILD FAILED" % define)
            print(error)
            failures += 1
            continue
        mutant = os.path.join(mutant_dir, CASE)
        differs = sha256(shipping) != sha256(mutant)
        result = run_case(mutant, CASE, os.path.join(root, "out-" + define), max_cycles)
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
        print("%-46s %-5d %-9s %s" % (define, result.returncode,
                                      "differs" if differs else "identical", status))
        print("%-46s       %s" % ("", first_result_line(log)))
        print("%-46s       %s" % ("", first_failure_line(log)))
        if differs and not observable:
            documented += 1
            print("%-46s       DOCUMENTED UNOBSERVABLE: %s" % ("", defect))
            continue
        if not (differs and caught):
            print("%-46s       defect: %s" % ("", defect))
            failures += 1

    print()
    if documented:
        print("%d control(s) documented as unobservable in this case" % documented)
    if failures:
        print("%d control(s) NOT MET" % failures)
        return 1
    print("all observable controls OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
