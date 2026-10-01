#!/usr/bin/env python3
"""Negative controls for FENCE / FENCE.I (I-037).

    python3 tools/run_fence_controls.py [--only <define>]

Each control builds CASE=fence.code_and_data_order from a **deleted** build
directory with its own `-D` on the Verilator command line, runs it, and requires
the binary to differ from the shipping one, to exit 1, and to name the failure
the mutation is supposed to produce. A control the case cannot observe is
reported NOT MET by name.

The mutations, the defect each injects and the first failure it must produce are
the four the work package names:

  * FENCE completes without draining the memory path: it retires with an older
    store still queued (the card's second fail mode);
  * FENCE.I does not invalidate the instruction view the front end already
    delivered, so the stale bytes execute (the card's first fail mode);
  * FENCE.I resumes one instruction too far, so a legitimate instruction is lost
    -- a wrong retirement stream, not a hang;
  * a younger memory access is allowed past a FENCE, so the data-ordering
    program reads the device before the older store publishes to it.

The table with the real commands, binary hashes and exit codes is in
results/reports/I-037-fence.md.
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
CASE = "fence.code_and_data_order"

# (define, defect, text the first failure must contain, observable?)
MUTANTS = [
    (
        "MOSAIC_CORE_MUTANT_FENCE_EARLY",
        "FENCE completes as soon as it reaches the ROB head instead of when the "
        "memory path has drained, so it retires with an older store still queued or "
        "the endpoint still busy -- the card's second fail mode",
        "retires with the memory path drained",
        True,
    ),
    (
        "MOSAIC_CORE_MUTANT_FENCEI_NO_INVALIDATE",
        "FENCE.I completes like a plain fence and never redirects the front end, "
        "so the instruction the fetch unit delivered before the publishing store "
        "survives and the stale bytes execute -- the card's first fail mode",
        "value for x10 expected",
        True,
    ),
    (
        "MOSAIC_CORE_MUTANT_FENCEI_SKIP",
        "FENCE.I resumes one instruction past itself, so the first instruction "
        "after it is skipped -- a legitimate instruction is lost. The case names "
        "it at the redirect target (one too far) before the retirement stream can",
        "the first redirect is FENCE.I's",
        True,
    ),
    (
        "MOSAIC_CORE_MUTANT_FENCE_ACCESS_PAST",
        "a younger memory macro is not blocked while a FENCE is staged, so the "
        "younger load is in the load queue when the fence retires and would read the "
        "device one publish too early",
        "value for x30 expected",
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

    root = os.path.join(REPO_ROOT, "build", "p0", "fence_controls")
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
