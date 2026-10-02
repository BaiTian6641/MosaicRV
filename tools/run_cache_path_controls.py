#!/usr/bin/env python3
"""Negative controls for CASE=cache.integrated_path (work package I-042).

The case claims the L1 caches are *in the access path and correct*: the caches
change no architectural result, they are actually used, a FENCE.I makes patched
bytes visible to fetch, and a device access is never cached. Each of those claims
is a place a plausible implementation could be wrong, so each gets a mutant that
breaks exactly it. Every mutant must (a) produce a binary that differs from the
shipping one, (b) exit 1, and (c) name the check it breaks in the log.

The build is done here rather than by `tools/run_unit.py` because a mutant is a
`-D` on the *build command*, and run_unit.py has no way to pass one: it would
build the shipping configuration and run it, which is the configuration the
mutant is supposed to differ from. Each build is made from a directory that is
deleted first, so no object file from another configuration can survive into it,
and the command that produced each binary is written next to it.

Profile p1: the caches are only reachable where the platform map declares a
cacheable region, and p0 declares none (its RAM is `cacheable: false`), so this
case is a p1 case by construction.

    python3 tools/run_cache_path_controls.py
    python3 tools/run_cache_path_controls.py --only DEVICE
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
PROFILE = "p1"
CASE_ID = "cache.integrated_path"

# Each mutant: the define, the defect it injects, the source that carries it, and
# the text of the check the mutation must break.
MUTANTS = [
    (
        "MOSAIC_CACHE_MUTANT_FENCEI_NO_FLUSH",
        os.path.join("rtl", "core", "mosaic_core.sv"),
        "FENCE.I writes the data cache back but never invalidates the instruction "
        "cache, so the front end keeps executing the line it already holds -- the "
        "card's 'FENCE.I that does not invalidate the instruction cache'",
        "SMC: FENCE.I made the patched instruction visible (cache-on)",
    ),
    (
        "MOSAIC_CACHE_MUTANT_DEVICE_CACHED",
        os.path.join("rtl", "core", "mosaic_l1_cache_path.sv"),
        "the platform map's cacheability rule is ignored, so a UART access is read "
        "as part of a cache line and installed -- the card's 'a device access that "
        "was cached'",
        "the three UART accesses reach memory once with the cache on (on)",
    ),
    (
        "MOSAIC_CACHE_MUTANT_DIRTY_DROP",
        os.path.join("rtl", "core", "mosaic_cache.sv"),
        "a dirty line's eviction is issued with zeroed data, so the dirty bytes "
        "never reach memory -- the card's 'an eviction loses dirty bytes'",
        "cache-on and cache-off leave the same four signature words",
    ),
    (
        "MOSAIC_CACHE_MUTANT_FAULT_VALID",
        os.path.join("rtl", "core", "mosaic_cache.sv"),
        "a refill that faulted is still installed as valid, so the next access "
        "hits a line the memory never delivered -- the card's 'an error refill "
        "marked valid'",
        "the faulted refill did not mark the line valid",
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
        if line.startswith("MISMATCH"):
            return line
    for line in log.splitlines():
        if line.startswith("CHECK FAILED"):
            return line
    for line in log.splitlines():
        if line.startswith("RESULT"):
            return line
    return "(no MISMATCH, CHECK FAILED or RESULT line)"


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

    # A mutant whose define is not in the source is not a mutant, it is a typo
    # that would silently build the shipping configuration.
    for define, source_rel, _defect, _expected in MUTANTS:
        source = os.path.join(REPO_ROOT, source_rel)
        with open(source) as handle:
            text = handle.read()
        if define not in text:
            print("FAIL: %s does not occur in %s" % (define, source_rel))
            return 1

    root = os.path.join(REPO_ROOT, "build", PROFILE, "cache_path_controls")
    shipping_dir = os.path.join(root, "shipping")
    print("building the shipping case from an empty directory...")
    build(entry, [], shipping_dir)
    shipping = os.path.join(shipping_dir, CASE_ID)
    result = run_case(shipping, os.path.join(root, "out-shipping"), max_cycles)
    log = result.stdout.decode("utf-8", "replace")
    baseline_ok = result.returncode == 0 and "RESULT PASS" in log
    print("  baseline exit=%d %s" % (result.returncode, first_result_line(log)))
    if not baseline_ok:
        print("BASELINE FAILED: the shipping build must pass before mutants mean "
              "anything")
        return 1

    failures = 0
    print("shipping binary sha256: %s" % sha256_of(shipping))
    print()
    print("%-42s %-5s %s" % ("mutant", "exit", "result"))
    print("-" * 110)
    for define, source_rel, defect, expected in MUTANTS:
        if args.only is not None and args.only not in define:
            continue
        mutant_dir = os.path.join(root, define)
        try:
            build(entry, [define], mutant_dir)
        except RuntimeError as error:
            print("%-42s BUILD FAILED" % define)
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
        print("%-42s %-5d %s" % (define, result.returncode, status))
        print("    injects: %s" % defect)
        print("    source:  %s" % source_rel)
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
