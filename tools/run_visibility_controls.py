#!/usr/bin/env python3
"""Negative controls for the memory-visibility provenance case (V-018).

    python3 tools/run_visibility_controls.py [--only <substring>]

Each control builds CASE=mem.visibility_provenance from a **deleted** build
directory with its own `-D` on the Verilator command line, runs it, and requires
the binary to differ from the shipping one and to exit 1 naming the failure the
mutation is supposed to produce. A control the case cannot observe is reported
NOT MET by name rather than quietly counted as passing.

The four controls the card names, and the defect each injects:

  * `MOSAIC_CORE_MUTANT_MEM_BEHIND_BRANCH + MOSAIC_SQ_MUTANT_VISIBLE_BEFORE_COMMIT`
    -- the branch barrier stops holding memory macros and the store queue's
    drain stops needing authorisation, so a store on a wrong path is dispatched
    and its bytes reach memory before it ever commits. This is the card's central
    failure: "one that makes a wrong-path store's bytes visible".
  * `MOSAIC_SQ_MUTANT_DRAIN_DUPLICATE` -- an accepted drain does not remove the
    entry, so one store's bytes become visible twice: "one that duplicates a
    drain (a byte written twice by two producers)".
  * `MOSAIC_LQ_MUTANT_MEMORY_OVER_STORE` -- a load that must be served by the
    store queue reads memory instead, so it returns stale bytes no legal source
    produced: "one that makes a load return a value no legal source could have
    produced".
  * `MOSAIC_VISIBILITY_RD_ONLY + MOSAIC_CORE_MUTANT_STORE_SIZE_WRONG` -- a
    *weakening* control rather than a defect: the driver drops every
    memory/provenance comparison and keeps only the rd comparison, and it must
    then PASS on a defect the real comparison catches. That is what makes the
    byte comparison load-bearing rather than decorative.

The remaining rows are extra mutants that exercise the same checks from other
directions (early visibility, an out-of-order drain, a corrupt store payload, a
missing sign extension, the barrier alone). A row the case cannot observe is
printed DOCUMENTED UNOBSERVABLE with the reason, not silently dropped.

The table with the real commands, binary hashes and exit codes is in
results/reports/V-018-visibility.md.
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
CASE = "mem.visibility_provenance"

# (defines, defect, text the first failure must contain, observable, expects_exit)
MUTANTS = [
    (
        ["MOSAIC_CORE_MUTANT_MEM_BEHIND_BRANCH", "MOSAIC_SQ_MUTANT_VISIBLE_BEFORE_COMMIT"],
        "the branch barrier stops holding memory macros and the drain stops needing "
        "authorisation, so a wrong-path store's bytes reach memory before it commits",
        "every visible byte has a legal, committed producer",
        True,
        1,
    ),
    (
        ["MOSAIC_SQ_MUTANT_DRAIN_DUPLICATE"],
        "an accepted drain does not remove the entry: one store's bytes become visible "
        "twice",
        "a byte written twice by two producers",
        True,
        1,
    ),
    (
        ["MOSAIC_LQ_MUTANT_MEMORY_OVER_STORE"],
        "a load that must forward from the store queue reads memory instead: stale "
        "bytes no legal source produced",
        "a load's returned value matches an allowed source",
        True,
        1,
    ),
    (
        ["MOSAIC_VISIBILITY_RD_ONLY", "MOSAIC_CORE_MUTANT_STORE_SIZE_WRONG"],
        "the driver drops every memory comparison and keeps only rd, so a corrupt store "
        "payload the real comparison catches is not caught",
        "(weakening control: expected to pass)",
        True,
        0,
    ),
    (
        ["MOSAIC_SQ_MUTANT_VISIBLE_BEFORE_COMMIT"],
        "the drain offer ignores the authorisation watermark: a store's bytes become "
        "visible before it commits",
        "no byte becomes visible before its commit",
        True,
        1,
    ),
    (
        ["MOSAIC_SQ_MUTANT_DRAIN_OUT_OF_ORDER"],
        "the drain takes any ready entry instead of position 0: a store becomes visible "
        "while an older committed store has not",
        "oldest committed store not yet drained",
        True,
        1,
    ),
    (
        ["MOSAIC_CORE_MUTANT_STORE_SIZE_WRONG"],
        "the retiring store's payload reports every size as a word, so the event stream "
        "cannot say which bytes the store owned",
        "the drain's byte mask is the store's own size",
        True,
        1,
    ),
    (
        ["MOSAIC_LSU_MUTANT_NO_SIGN_EXTEND"],
        "a signed load is not sign-extended: the value it returns is not what any source "
        "holds",
        "a load's returned value matches an allowed source",
        True,
        1,
    ),
    (
        ["MOSAIC_CORE_MUTANT_MEM_BEHIND_BRANCH"],
        "the branch barrier stops holding memory macros: a wrong-path store is "
        "dispatched, allocated and then squashed before it commits",
        "(no byte is expected to leak without the drain mutant too)",
        False,
        1,
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
        if define.startswith("MOSAIC_VISIBILITY_"):
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

    root = os.path.join(REPO_ROOT, "build", "p0", "visibility_controls")
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
    print("%-64s %-5s %-9s %s" % ("mutant", "exit", "binary", "result"))
    print("-" * 132)
    for defines, defect, expected, observable, want_exit in MUTANTS:
        label = "+".join(d.replace("MOSAIC_", "") for d in defines)
        if args.only is not None and args.only not in label:
            continue
        mutant_dir = os.path.join(root, label)
        try:
            build(entry, CASE, defines, mutant_dir)
        except RuntimeError as error:
            print("%-64s BUILD FAILED" % label)
            print(error)
            failures += 1
            continue
        mutant = os.path.join(mutant_dir, CASE)
        differs = sha256(shipping) != sha256(mutant)
        result = run_case(mutant, CASE, os.path.join(root, "out-" + label), max_cycles)
        log = result.stdout.decode("utf-8", "replace")
        if want_exit == 0:
            caught = result.returncode == 0
        else:
            caught = result.returncode == 1 and expected in log
        status = "OK" if (differs and caught) else "MISS"
        if not differs:
            status += " (binary identical to shipping)"
        if result.returncode != want_exit:
            status += " (exit %d, wanted %d)" % (result.returncode, want_exit)
        elif want_exit == 1 and expected not in log:
            status += " (fails, but not on %r)" % expected
        print("%-64s %-5d %-9s %s" % (label, result.returncode,
                                      "differs" if differs else "identical", status))
        print("%-64s       %s" % ("", first_result_line(log)))
        print("%-64s       %s" % ("", first_failure_line(log)))
        if differs and not observable:
            documented += 1
            print("%-64s       DOCUMENTED UNOBSERVABLE: %s" % ("", defect))
            continue
        if not (differs and caught):
            print("%-64s       defect: %s" % ("", defect))
            failures += 1

    print()
    if documented:
        print("%d control(s) documented as unobservable in this case" % documented)
    if failures:
        print("%d control(s) NOT MET" % failures)
        return 1
    print("all controls met")
    return 0


if __name__ == "__main__":
    sys.exit(main())
