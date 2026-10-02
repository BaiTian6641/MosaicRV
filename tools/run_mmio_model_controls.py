#!/usr/bin/env python3
"""Negative controls for the MMIO side-effect model (work package V-019).

    python3 tools/run_mmio_model_controls.py [--only <substring>]

Each control builds CASE=mmio.side_effect_model from a **deleted** build
directory with its own `-D` on the Verilator command line, runs it, and requires
the binary to differ from the shipping one and to fail in the way the defect
says. A control the case cannot observe is reported NOT MET by name rather than
quietly counted as passing.

The mutants are the existing RTL mutations this case's checks catch -- this lane
does not edit RTL -- plus two harness-side switches the driver implements:

  * `MOSAIC_CORE_MUTANT_DEV_SPECULATIVE` -- the non-speculation gate is removed.
    The wrong-path device read and store behind the ecall's trap are performed,
    so a cancelled access has a side effect: the card's central failure.
  * `MOSAIC_CORE_MUTANT_DEV_RETRY` -- the held device transaction is not
    released, so one access is performed twice: a duplicate side effect.
  * `MOSAIC_CORE_MUTANT_DEV_AS_RAM` -- the device attribute presented to the
    memory system is cleared, the coalescing attribute a cache would key on.
  * `MOSAIC_LSU_MUTANT_DEV_ERR_OK` -- a device's fault response is reported as a
    successful read of zero, so the illegal-width / unmodelled-register access
    never traps: an unmodelled device defaults to zero.
  * `MOSAIC_LSU_MUTANT_FAULT_AS_ZERO` -- any fault is reported as a read of zero,
    so an unmapped address reads as zero instead of trapping.
  * `MOSAIC_LSU_MUTANT_NO_MISALIGN_CHECK` -- a misaligned access reaches the
    device instead of trapping.
  * `MOSAIC_SQ_MUTANT_DRAIN_DUPLICATE` -- an accepted drain does not remove the
    entry, so a device store's side effect happens twice.
  * `MOSAIC_SQ_MUTANT_DRAIN_OUT_OF_ORDER` -- the drain takes the youngest ready
    entry, so device stores reach the device out of order.
  * `MOSAIC_CORE_MUTANT_STORE_SIZE_WRONG` -- every store's size becomes a word,
    so its byte enables are wrong.
  * `MOSAIC_CORE_MUTANT_STORE_PRECOMMIT` -- a store is authorised at allocation,
    so a wrong-path device store reaches the device before it retires.
  * `MOSAIC_MMIO_MODEL_MUTANT_STATUS_AS_SCRATCH` (driver) -- the observation
    presents a STATUS read at SCRATCH's address; the two registers hold the same
    word, so the read data and every published value are identical and only the
    address differs. The card's "identical MMIO read data must not mask a wrong
    address".
  * `MOSAIC_MMIO_MODEL_RD_ONLY` (driver) -- a weakening control. With it the
    driver drops the access-log, attribute and device-state comparisons, so the
    STATUS_AS_SCRATCH injection passes; that is what makes the address and
    attribute comparisons load-bearing rather than decorative.

The table with the real commands, binary hashes and exit codes is in
results/reports/V-019-mmio.md.
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
CASE = "mmio.side_effect_model"

# (defines, defect, text the first failure must contain, observable, expects_exit)
MUTANTS = [
    (
        ["MOSAIC_CORE_MUTANT_DEV_SPECULATIVE"],
        "the non-speculation gate is removed: a device load issues as soon as the "
        "load queue offers it, so the read-back of the scratch register is served "
        "before the store it must observe has drained -- and the wrong-path device "
        "accesses behind the ecall's trap are performed",
        "the retirement stream follows the reference",
        True,
        1,
    ),
    (
        ["MOSAIC_CORE_MUTANT_STORE_PRECOMMIT"],
        "a store is authorised at allocation, so the wrong-path device store reaches "
        "the device before the instruction retires",
        "follow the reference, in order",
        True,
        1,
    ),
    (
        ["MOSAIC_CORE_MUTANT_DEV_RETRY"],
        "the held device transaction is not released when the endpoint takes it, so "
        "the same access is offered -- and performed -- again",
        "distinct identity",
        True,
        1,
    ),
    (
        ["MOSAIC_SQ_MUTANT_DRAIN_DUPLICATE"],
        "an accepted drain does not remove the entry, so a device store drains twice "
        "and its side effect happens twice; the wedged queue is caught by the "
        "progress watchdog",
        "stalled",
        True,
        1,
    ),
    (
        ["MOSAIC_SQ_MUTANT_DRAIN_OUT_OF_ORDER"],
        "the drain takes the youngest ready authorised entry, so device stores reach "
        "the device out of order",
        "max-cycles exhausted",
        True,
        1,
    ),
    (
        ["MOSAIC_CORE_MUTANT_STORE_SIZE_WORD"],
        "every store's allocation size becomes a word, so its size and byte enables "
        "are wrong; the strobed write changes different bytes and the read-back "
        "diverges from the reference",
        "the retirement stream follows the reference",
        True,
        1,
    ),
    (
        ["MOSAIC_LSU_MUTANT_NO_STORE_SHIFT"],
        "a store's data is placed at lane zero instead of at the addressed lane, so a "
        "byte store writes the wrong byte",
        "the retirement stream follows the reference",
        True,
        1,
    ),
    (
        ["MOSAIC_CORE_MUTANT_DEV_AS_RAM"],
        "the attribute presented to the memory system is cleared: the access is still "
        "serialized, but it is presented as ordinary -- the coalescing attribute",
        "every access carries the device attribute",
        True,
        1,
    ),
    (
        ["MOSAIC_LSU_MUTANT_DEV_ERR_OK"],
        "a device's fault response is reported as a successful read of zero, so the "
        "illegal-width and unmodelled-register accesses retire instead of trapping",
        "retires instead of trapping",
        True,
        1,
    ),
    (
        ["MOSAIC_LSU_MUTANT_FAULT_AS_ZERO"],
        "a memory access fault is reported as a successful read of zero, so an "
        "unmapped address reads as zero instead of trapping",
        "retires instead of trapping",
        True,
        1,
    ),
    (
        ["MOSAIC_LSU_MUTANT_NO_MISALIGN_CHECK"],
        "the misalignment check never fires, so a misaligned device access reaches "
        "the device, is refused there with a different cause, and no longer traps "
        "with cause 4",
        "the retirement stream follows the reference",
        True,
        1,
    ),
    (
        ["MOSAIC_MMIO_MODEL_MUTANT_STATUS_AS_SCRATCH"],
        "the observation presents a STATUS read at SCRATCH's address: the read data "
        "and every published value are identical and only the address differs",
        "access 2 address expected",
        True,
        1,
    ),
    (
        ["MOSAIC_MMIO_MODEL_RD_ONLY"],
        "a weakening control, not a defect: the driver drops the access-log, attribute "
        "and device-state comparisons and keeps only the published-data comparison",
        "",
        True,
        0,
    ),
    (
        ["MOSAIC_MMIO_MODEL_RD_ONLY", "MOSAIC_MMIO_MODEL_MUTANT_STATUS_AS_SCRATCH"],
        "the data-only comparison passes on the wrong-address injection: this is what "
        "makes the address comparison load-bearing rather than decorative",
        "",
        True,
        0,
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
        # A driver-side control is offered to Verilator as a Verilator define and
        # to the C++ sources through CFLAGS, so a control that weakens the driver
        # and a control that breaks the RTL use the same switch.
        cmd += ["-D%s" % define]
        if define.startswith("MOSAIC_MMIO_MODEL_"):
            cmd += ["-CFLAGS", "-D%s" % define]
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

    root = os.path.join(REPO_ROOT, "build", "p0", "mmio_model_controls")
    shipping_dir = os.path.join(root, "shipping")
    print("building the shipping case from an empty directory...")
    try:
        build(entry, CASE, [], shipping_dir)
    except RuntimeError as error:
        print("BASELINE BUILD FAILED")
        print(error)
        return 1
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
    print("%-58s %-5s %-9s %s" % ("mutant", "exit", "binary", "result"))
    print("-" * 130)
    for defines, defect, expected, observable, expects_exit in MUTANTS:
        if args.only is not None and not any(args.only in d for d in defines):
            continue
        label = " + ".join(defines)
        mutant_dir = os.path.join(root, "_".join(defines))
        try:
            build(entry, CASE, defines, mutant_dir)
        except RuntimeError as error:
            print("%-58s BUILD FAILED" % label)
            print(str(error)[-2000:])
            failures += 1
            continue
        mutant = os.path.join(mutant_dir, CASE)
        differs = sha256(shipping) != sha256(mutant)
        result = run_case(mutant, CASE, os.path.join(root, "out-" + "_".join(defines)),
                          max_cycles)
        log = result.stdout.decode("utf-8", "replace")
        if expects_exit == 0:
            caught = result.returncode == 0 and "RESULT PASS" in log
        else:
            caught = result.returncode == expects_exit and expected in log
        status = "OK" if (differs and caught) else "MISS"
        if not differs:
            status += " (binary identical to shipping)"
        if result.returncode == 0 and expects_exit != 0:
            status += " (exit 0: unobservable or no check can fail)"
        elif result.returncode != expects_exit:
            status += " (exit %d, wanted %d)" % (result.returncode, expects_exit)
        if differs and result.returncode == expects_exit and not caught:
            status += " (fails, but not on %r)" % expected
        print("%-58s %-5d %-9s %s" % (label, result.returncode,
                                      "differs" if differs else "identical", status))
        print("%-58s       %s" % ("", first_result_line(log)))
        print("%-58s       %s" % ("", first_failure_line(log)))
        if differs and not observable:
            documented += 1
            print("%-58s       DOCUMENTED UNOBSERVABLE: %s" % ("", defect))
            continue
        if not (differs and caught):
            print("%-58s       defect: %s" % ("", defect))
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
