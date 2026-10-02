#!/usr/bin/env python3
"""Negative controls for CASE=rvv.chaining_hazards (work package I-058).

Each mutant is one `-DMOSAIC_VEC_CHAIN_MUTANT_*` define that injects a single
defect into the vector chaining network.  The runner builds the shipping case
first, from an empty directory, and requires it to pass -- a mutant means nothing
if the baseline is already broken -- then builds each mutant from its own empty
directory with its define on the recorded command line, requires the binary to
differ from the shipping one, requires exit 1, and requires the first failure to
name the check the defect should break.

    python3 tools/run_vec_chain_controls.py [--profile p0] [--only NAME]
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
CASE_ID = "rvv.chaining_hazards"

# Each mutant: the define, the source file that must read it, the defect it
# injects, and the text the failing check must contain.
MUTANTS = [
    (
        "MOSAIC_VEC_CHAIN_MUTANT_MACRO_READY",
        "rtl/core/mosaic_vec_chain.sv",
        "readiness is a macro-level ready bit -- one element written makes the "
        "whole producing macro look ready, so a consumer reads an element its "
        "producer has not written (the card's first fail mode)",
        "order descending: an element was read before its producer wrote it",
    ),
    (
        "MOSAIC_VEC_CHAIN_MUTANT_STALE_PACKET",
        "rtl/core/mosaic_vec_chain.sv",
        "the packet identity check is dropped, so a packet from a cancelled "
        "generation is accepted by the new descriptor that took the slot (the "
        "card's second fail mode)",
        "cancel: a packet from the cancelled generation 30 was accepted",
    ),
    (
        "MOSAIC_VEC_CHAIN_MUTANT_WAR_RELEASE",
        "rtl/core/mosaic_vec_chain.sv",
        "a WAR release fires because some other reader is free, ignoring the "
        "reader whose element read is still outstanding",
        "war A: element 4 was released while the consumer had read only",
    ),
    (
        "MOSAIC_VEC_CHAIN_MUTANT_REORDER",
        "rtl/core/mosaic_vec_chain.sv",
        "the forwarding path returns the neighbouring element's data, so the "
        "element-order contract inside one macro is broken",
        "order ascending: element 0 read",
    ),
]


def build(entry: dict, profile: str, defines: list, build_dir: str) -> str:
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
    for define in defines:
        # The mutation is in the RTL, so the define goes to Verilator, not to
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
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def first_failure(log: str) -> str:
    for line in log.splitlines():
        if line.startswith("CHECK FAILED"):
            return line
    for line in log.splitlines():
        if line.startswith("RESULT"):
            return line
    return "(no CHECK FAILED or RESULT line)"


def first_result_line(log: str) -> str:
    for line in log.splitlines():
        if line.startswith("RESULT "):
            return line
    return "(no RESULT line)"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", default=None)
    parser.add_argument("--profile", default="p0")
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
    for define, source_rel, _defect, _expected in MUTANTS:
        source = os.path.join(REPO_ROOT, source_rel)
        if not os.path.exists(source):
            print("FAIL: %s does not exist" % source)
            return 1
        with open(source) as handle:
            if define not in handle.read():
                print("FAIL: %s does not occur in %s" % (define, source))
                return 1

    root = os.path.join(REPO_ROOT, "build", args.profile, "vec_chain_controls")
    shipping_dir = os.path.join(root, "shipping")
    print("building the shipping case from an empty directory (profile %s)..." % args.profile)
    build(entry, args.profile, [], shipping_dir)
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
    print("%-48s %-5s %s" % ("mutant", "exit", "result"))
    print("-" * 118)
    for define, _source_rel, defect, expected in MUTANTS:
        if args.only is not None and args.only not in define:
            continue
        mutant_dir = os.path.join(root, define)
        try:
            build(entry, args.profile, [define], mutant_dir)
        except RuntimeError as error:
            print("%-48s BUILD FAILED" % define)
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
        print("%-48s %-5d %s" % (define, result.returncode, status))
        print("    injects: %s" % defect)
        print("    sha256:  %s" % mutant_hash)
        print("    build:   %s" % os.path.relpath(
            os.path.join(mutant_dir, "build_command.txt"), REPO_ROOT))
        print("    %s" % first_failure(log))
        if not (differs and caught):
            failures += 1

    print("-" * 118)
    if failures:
        print("%d mutant(s) were not caught as required" % failures)
        return 1
    print("all %d mutants mutate the binary, exit 1 and name the check they break"
          % (len(MUTANTS) if args.only is None else 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
