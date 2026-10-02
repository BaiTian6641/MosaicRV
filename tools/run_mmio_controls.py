#!/usr/bin/env python3
"""Negative controls for the device path (I-038).

    python3 tools/run_mmio_controls.py [--only <define>]

Each control builds CASE=mmio.exactly_once from a **deleted** build directory
with its own `-D` on the Verilator command line, runs it, and requires the binary
to differ from the shipping one, to exit 1, and to name the failure the mutation
is supposed to produce. A control the case cannot observe is reported NOT MET by
name.

The mutations, the defect each injects and the first failure it must produce are
the four the work package names:

  * a wrong-path device access reaching the device: the serializer's
    non-speculation gate is removed, so the FIFO read that the ecall's trap
    discards is performed anyway -- the device pops twice for a program that pops
    once;
  * a replayed transaction duplicating a side effect: the serializer does not
    release the device transaction it handed to the endpoint, so the endpoint
    takes it a second time -- one identity, two side effects;
  * an error response not trapping: a fault response to a device access is
    reported as a successful read of zero, so the trap never happens and the
    fall-through's marker is published instead;
  * a device access coalesced with RAM: the access is still serialized
    correctly, but the device attribute the memory system is told is cleared, so
    a device access is presented as an ordinary one -- the attribute a coalescer
    keys on when it refuses to merge MMIO with RAM.

A fifth control is *not* claimed: a combinational ready loop between the
serializer's gate and the queues. The serializer's upstream `ready` is a function
of the serializer's own register and the endpoint's ready, and the endpoint's
ready is a function of its own state -- not of `req_valid_i` -- so no path exists
from the offered request back to its own acceptance. The argument is in
results/reports/I-038-mmio.md; a mutant cannot exhibit a loop that the structure
does not contain.

The table with the real commands, binary hashes and exit codes is in
results/reports/I-038-mmio.md.
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
CASE = "mmio.exactly_once"

# (define, defect, text the first failure must contain, observable?)
MUTANTS = [
    (
        "MOSAIC_CORE_MUTANT_DEV_SPECULATIVE",
        "the serializer's non-speculation gate is removed, so a device load is "
        "issued as soon as the load queue offers it. The FIFO read the ecall's "
        "redirect discards is performed for an instruction that never retires, "
        "and the device pops twice for a program that pops once",
        "the device performed exactly the retired FIFO pops",
        True,
    ),
    (
        "MOSAIC_CORE_MUTANT_DEV_RETRY",
        "the serializer keeps the device transaction after the endpoint has taken "
        "it, so the same access is offered -- and performed -- again: one "
        "identity, two side effects",
        "distinct identity",
        True,
    ),
    (
        "MOSAIC_LSU_MUTANT_DEV_ERR_OK",
        "a fault response to a device access is reported as a successful read of "
        "zero, so the error device's response never traps and the program "
        "publishes its no-trap marker",
        "did not trap",
        True,
    ),
    (
        "MOSAIC_CORE_MUTANT_DEV_AS_RAM",
        "the device attribute presented to the memory system is cleared: the "
        "access is serialized correctly, but it is presented as an ordinary "
        "access -- the coalescing attribute",
        "every access carries the device attribute",
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
        if line.startswith("MISMATCH "):
            return line
    return "(no MISMATCH line)"


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

    root = os.path.join(REPO_ROOT, "build", "p0", "mmio_controls")
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
