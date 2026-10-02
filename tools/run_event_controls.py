#!/usr/bin/env python3
"""Negative controls for CASE=core.event_payload (work package I-017).

The case claims that the frozen architectural event carries a store's and a CSR
instruction's own identity through the integrated core, and that a trap the
system unit resolves publishes a lane-0 record. Those claims are only evidence
if a machine that got one of them wrong would be caught, so this tool rebuilds
the case with exactly one defect injected -- the three defects V-013 reported,
plus two narrower variants of the payload one -- from an empty build directory,
and runs it. Every mutant must:

  * build into its own directory, deleted first, with its `-D` in that build's
    own command line (written next to the binary as `build_command.txt`);
  * produce a binary whose SHA-256 differs from the shipping build's, so a stale
    object file cannot masquerade as a mutant;
  * exit 1, with the named check the mutation breaks as the first failure.

The LWU control is the decoder's own: `LWU` is decoded by mosaic_decoder.sv, so
the case that owns "which encodings are reserved" (CASE=decode.rv64im_reserved)
is the one that must catch it, and its expectation is the independent C++
reference decoder written from the ISA text.

Usage: run_event_controls.py [--only NAME]
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

EVENT_CASE = "core.event_payload"
DECODE_CASE = "decode.rv64im_reserved"

# (case, define, kind, defect, text the first failure must contain)
# kind: "rtl" -> -D to Verilator, "driver" -> -CFLAGS -D to the C++ half.
MUTANTS = [
    (
        EVENT_CASE,
        "MOSAIC_CORE_MUTANT_NO_STORE_PAYLOAD",
        "rtl",
        "the store payload is tied off again -- the wiring this package shipped "
        "with, and the F-4 gap V-013 reported (its report section 6.2). The "
        "store still retires and still reaches memory; the event just cannot say "
        "which store it was.",
        "the store payload is present exactly for a store instruction",
    ),
    (
        EVENT_CASE,
        "MOSAIC_CORE_MUTANT_STORE_SIZE_WRONG",
        "rtl",
        "every store is reported as a word whatever the instruction asked for. "
        "The valid bit, the address and the data are all right, so only a check "
        "that reads `mem_size` field by field can see it.",
        "carries mem_size",
    ),
    (
        EVENT_CASE,
        "MOSAIC_CORE_MUTANT_NO_CSR_PAYLOAD",
        "rtl",
        "the CSR payload is tied off again. The CSR write still happens and is "
        "still visible in the architectural state; the event stream stops "
        "naming the address and the value it wrote.",
        "the CSR payload is present exactly for a CSR instruction that writes",
    ),
    (
        EVENT_CASE,
        "MOSAIC_RETIRE_MUTANT_SYS_TRAP_NO_EVENT",
        "rtl",
        "a trap the system unit resolved publishes no event at all -- the defect "
        "V-013 reported in its section 6.3. The trap is still taken, mepc and "
        "mcause are still written and the handler still runs; the stream simply "
        "shows a PC discontinuity with nothing to explain it.",
        "exactly one trap event was published",
    ),
    (
        EVENT_CASE,
        "MOSAIC_DECODER_MUTANT_LWU_ILLEGAL",
        "rtl",
        "LWU is refused as a reserved encoding again, seen from the integrated "
        "core: the program contains an `lwu` and the machine stops on it, which "
        "is the observation V-013 recorded (its section 6.1: "
        "`unsupported=1 illegal=1 stopped=1`).",
        "the machine never stopped on a refused macro",
    ),
    (
        DECODE_CASE,
        "MOSAIC_DECODER_MUTANT_LWU_ILLEGAL",
        "rtl",
        "LWU (LOAD funct3 110) is refused as a reserved encoding again -- the "
        "defect V-013 reported in its section 6.1. RV64I requires the "
        "instruction and the project's own reference model implements it, so the "
        "machine stops on a legal instruction.",
        "LWU",
    ),
]


def build(case_id: str, entry: dict, mutants: list, build_dir: str) -> str:
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
    for define, kind, _defect, _expected in mutants:
        if kind == "rtl":
            cmd += ["-D%s" % define]
        else:
            cmd += ["-CFLAGS", "-D%s" % define]
    cmd += ["-o", os.path.join(build_dir, case_id)]
    cmd += sources

    # The directory was deleted above, so this build cannot reuse a shipping
    # object; the command is written next to the binary so the mutant's `-D` is
    # on the record for whoever reads the evidence later.
    with open(os.path.join(build_dir, "build_command.txt"), "w") as handle:
        handle.write(" ".join(cmd))

    result = run_unit.run(cmd)
    log = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0:
        raise RuntimeError("build failed for %s\n%s" % (mutants, log))
    return log


def run_case(case_id: str, binary: str, out_dir: str, max_cycles: int):
    os.makedirs(out_dir, exist_ok=True)
    return subprocess.run(
        [binary, "--case", case_id, "--out", out_dir, "--seed", "1",
         "--max-cycles", str(max_cycles)],
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )


def sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def first_line(log: str, prefix: str) -> str:
    for line in log.splitlines():
        if line.startswith(prefix):
            return line
    return "(no %s line)" % prefix


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", default=None)
    args = parser.parse_args()

    registry = run_unit.load_registry()["cases"]
    for case_id in (EVENT_CASE, DECODE_CASE):
        if case_id not in registry:
            print("no such case: %s" % case_id)
            return 2

    root = os.path.join(REPO_ROOT, "build", "p0", "event_controls")

    # ------------------------------------------------------------- baseline
    shipping = {}
    for case_id in (EVENT_CASE, DECODE_CASE):
        entry = registry[case_id]
        shipping_dir = os.path.join(root, "shipping", case_id)
        print("building the shipping %s from an empty directory..." % case_id)
        build(case_id, entry, [], shipping_dir)
        binary = os.path.join(shipping_dir, case_id)
        result = run_case(case_id, binary, os.path.join(root, "out-shipping", case_id),
                          entry.get("max_cycles", 4000000))
        baseline_ok = result.returncode == 0 and b"RESULT PASS" in result.stdout
        print("  baseline exit=%d %s" % (
            result.returncode, first_line(result.stdout.decode("utf-8", "replace"),
                                          "RESULT ")))
        if not baseline_ok:
            print("BASELINE FAILED: the shipping build must pass before mutants "
                  "mean anything")
            return 1
        shipping[case_id] = binary
        print("  shipping binary sha256: %s" % sha256(binary))

    print()
    print("%-44s %-7s %-5s %s" % ("mutant", "kind", "exit", "result"))
    print("-" * 118)
    failures = 0
    for case_id, define, kind, defect, expected in MUTANTS:
        if args.only is not None and args.only not in define:
            continue
        entry = registry[case_id]
        # The same define is used by two cases (the LWU control is built for
        # both the decoder case and the integrated core), so the case id is part
        # of the directory name: one mutant's evidence cannot overwrite the
        # other's.
        mutant_dir = os.path.join(root, "%s__%s" % (case_id, define))
        try:
            build(case_id, entry, [(define, kind, defect, expected)], mutant_dir)
        except RuntimeError as error:
            print("%-44s %-7s BUILD FAILED" % (define, kind))
            print(error)
            failures += 1
            continue
        mutant = os.path.join(mutant_dir, case_id)
        differs = subprocess.run(["cmp", "-s", shipping[case_id], mutant]).returncode != 0
        result = run_case(case_id, mutant,
                          os.path.join(root, "out-%s__%s" % (case_id, define)),
                          entry.get("max_cycles", 4000000))
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
        print("%-44s %-7s %-5d %s" % (define, kind, result.returncode, status))
        print("    case:    %s" % case_id)
        print("    injects: %s" % defect)
        print("    sha256:  %s" % sha256(mutant))
        print("    build:   %s" % os.path.relpath(
            os.path.join(mutant_dir, "build_command.txt"), REPO_ROOT))
        print("    %s" % first_line(log, "RESULT "))
        print("    %s" % first_line(log, "MISMATCH "))
        if not (differs and caught):
            failures += 1

    print("-" * 118)
    if failures:
        print("%d mutant(s) were not caught as required" % failures)
        return 1
    print("all %d mutants mutate the binary, exit 1 and name the check they break"
          % len([m for m in MUTANTS if args.only is None or args.only in m[1]]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
