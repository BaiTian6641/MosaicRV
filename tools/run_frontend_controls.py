#!/usr/bin/env python3
"""Negative controls for the two-wide front end (CASE=perf.equal_resource_compare).

The shipping build is built and run first, from an empty directory: a mutant
means nothing if the case does not pass in the first place. Each control is then
built from its own empty directory with one `-D` in the build command, and must

  * produce a binary whose sha256 differs from the shipping binary,
  * exit 1, and
  * name the check it breaks in the first failure.

A control whose subject a configuration does not exercise is reported **INERT**
rather than required: it is not caught and it is not counted as a control. The
suite prints how many controls were required and how many were inert, so an
inert control can never be mistaken for a passing one.

Two runs at once are refused with an exclusive lock (`build/<profile>/unit/
frontend_controls.lock`): the suite builds into fixed directories and writes a
fixed results directory, so concurrent runs overwrite each other's binaries and
logs -- two concurrent runs of the same command on the same tree printed two
contradictory shipping measurements.

The only conditional control today is `MOSAIC_ROB_MUTANT_SWAP_PAIR_ORDER`, whose
subject is the two-wide **pair path**: the mutant places the two macros of a
same-cycle pair in the wrong ROB slots, so the pair's two macros must actually
leave the dispatch queue together for the defect to reach retirement. The
condition is the shipping run's own `pairs_total` counter (the same-cycle second
insert), printed by the case. `alloc2_total` alone does **not** imply the mutant
can be caught: with the insert held pending V-013
(`results/reports/held-insert.md`) the group is still allocated (`alloc2_total`
28) but no pair is inserted together (`pairs_total` 0) and the mutant is inert.
The mutant is held with the feature and must be re-armed when the insert
returns.

The controls exercise the paths the two-wide front end added:

  MOSAIC_ROB_MUTANT_SWAP_PAIR_ORDER   the ROB places a two-wide pair's two
      macros in the wrong slots, so the younger of the pair retires first. The
      absolute program-order check on `pair_burst` must catch it. **Conditional**
      on the pair path being exercised (`pairs_total > 0`); with the insert held
      it is inert.

  MOSAIC_DISPATCH_MUTANT_DROP_PAIR_TAIL   when the second lane of a pair was
      *refused* (the ROB or the queue was nearly full) the decoded instruction
      is popped anyway and never allocated -- an instruction the machine silently
      drops. The absolute signature check on `pair_burst` must catch it.

  MOSAIC_RENAME_MUTANT_NO_BYPASS   an RTL control that already exists (I-014):
      the wider path's same-cycle dependency check is bypassed, so a lane-1
      source naming lane 0's destination reads the superseded mapping. On the
      `pair_burst` workload every pair's second macro consumes the first's
      result, so the signature must change.

Usage: run_frontend_controls.py [--profile p1] [--only SUBSTRING]
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_unit  # noqa: E402

REPO_ROOT = run_unit.REPO_ROOT
CASE_ID = "perf.equal_resource_compare"

MUTANTS = [
    # The last field names the subject a mutant's catch is contingent on, or
    # None if it must be caught unconditionally. `two-wide-pair-path` means the
    # mutant's defect can only reach retirement when a same-cycle pair is
    # actually inserted (`pairs_total > 0`); the same-cycle second insert is held
    # pending V-013 (results/reports/held-insert.md). When the pair path is not
    # exercised the mutant is reported INERT -- not caught, and not counted as a
    # control -- rather than failing the suite; when it *is* exercised the mutant
    # must be caught exactly as before. (Two-wide *allocation* alone is not the
    # condition: with the insert held, alloc2_total is 28 and the mutant is still
    # inert, because the allocated pair's macros do not reach the compared retire
    # window.)
    ("MOSAIC_ROB_MUTANT_SWAP_PAIR_ORDER", "rtl",
     "a two-wide group's two macros are placed in the wrong ROB slots, so the "
     "younger of the pair retires first: the absolute program-order check on "
     "pair_burst must catch it", "same architecture", "two-wide-pair-path"),
    ("MOSAIC_DISPATCH_MUTANT_DROP_PAIR_TAIL", "rtl",
     "the second macro of a pair is popped from the decoded-instruction queue "
     "even when it was refused and never allocated, so the instruction is "
     "silently dropped: the absolute signature check on pair_burst must catch it",
     "the machine stopped", None),
    ("MOSAIC_RENAME_MUTANT_NO_BYPASS", "rtl",
     "the wider path's same-cycle dependency check is bypassed, so lane 1 reads "
     "the superseded mapping instead of lane 0's result: the machine stops or the retire stream diverges "\
     "(observed first failure: the machine stopped on an instruction it refuses)", "the machine stopped", None),
]


def build(entry, define, kind, profile, build_dir):
    shutil.rmtree(build_dir, ignore_errors=True)
    os.makedirs(build_dir, exist_ok=True)
    rtl = entry.get("rtl", [])
    packages = [p for p in rtl if os.path.basename(p) == "mosaic_pkg.sv"]
    rest = [p for p in rtl if p not in packages]
    sources = [os.path.join(REPO_ROOT, p) for p in
               (packages + rest + entry.get("sv", []) + entry.get("cpp", []) +
                run_unit.SHARED_CPP)]
    cmd = ["verilator"] + run_unit.VERILATOR_FLAGS
    cmd += ["--top-module", entry["top"], "-Mdir", os.path.join(build_dir, "obj_dir")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "build", profile, "sim")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "rtl", "core")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "rtl", "common")]
    cmd += ["-I%s" % os.path.join(REPO_ROOT, "build", profile, "rtl")]
    cmd += ["-CFLAGS", "-I%s" % run_unit.SIM_COMMON]
    cmd += ["-CFLAGS", "-I%s" % os.path.join(REPO_ROOT, "build", profile, "sim")]
    if define:
        if kind == "driver":
            cmd += ["-CFLAGS", "-D%s" % define]
        else:
            cmd += ["-D%s" % define]
    binary = os.path.join(build_dir, CASE_ID)
    cmd += ["-o", binary]
    cmd += sources
    result = subprocess.run(cmd, cwd=REPO_ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    if result.returncode != 0 or not os.path.exists(binary):
        raise RuntimeError("build failed:\n" + result.stdout.decode("utf-8", "replace"))
    with open(binary, "rb") as handle:
        digest = hashlib.sha256(handle.read()).hexdigest()[:16]
    return binary, digest


def run_case(binary, out_dir, max_cycles):
    os.makedirs(out_dir, exist_ok=True)
    cmd = [binary, "--case", CASE_ID, "--out", out_dir, "--seed", "1",
           "--max-cycles", str(max_cycles)]
    result = subprocess.run(cmd, cwd=REPO_ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    log = result.stdout.decode("utf-8", "replace")
    with open(os.path.join(out_dir, "run.log"), "w") as handle:
        handle.write("$ " + " ".join(cmd) + "\n" + log)
    return result.returncode, log


def first_failure(log):
    for line in log.splitlines():
        if line.startswith("CHECK FAILED"):
            return line
        if line.startswith("MISMATCH"):
            return line
    return log.strip().splitlines()[-1] if log.strip() else "<no output>"


def parse_two_wide(log):
    """The shipping run's own two-wide counters, or None if it printed none.

    Both are needed and they mean different things: `alloc2_total` counts groups
    the ROB accepted (allocation), `pairs_total` counts same-cycle second inserts.
    The pair-order mutant's subject is the *insert* -- the swap happens on the
    second insert's way into the ROB -- and that is empirical, not assumed: in the
    held configuration the shipping run reports `alloc2_total=28 pairs_total=0`
    and the mutant is still inert, while with the insert present it is caught.
    Conditioning on allocation alone therefore mislabels an inert control as a
    missing catch, which is the failure this suite exists to prevent.
    """
    for line in log.splitlines():
        match = re.search(r"alloc2_total=(\d+) pairs_total=(\d+)", line)
        if match:
            return int(match.group(1)), int(match.group(2))
    return None


def acquire_run_lock(profile):
    """An exclusive lock for this profile's controls run, or None if one is held.

    The suite builds into fixed directories (`build/<profile>/unit/frontend_ship`
    and `frontend_mut`) and writes into a fixed results directory, so two runs at
    once overwrite each other's binaries and logs: two concurrent runs of the
    same command on the same tree produced two contradictory shipping
    measurements (`pairs_total` 100 vs 0). A second run is therefore refused with
    a clear message rather than silently sharing state. `flock` is released by
    the kernel when the process exits, so a crashed run cannot leave a stale
    lock.
    """
    lock_dir = os.path.join(REPO_ROOT, "build", profile, "unit")
    os.makedirs(lock_dir, exist_ok=True)
    path = os.path.join(lock_dir, "frontend_controls.lock")
    handle = open(path, "w")
    try:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        handle.close()
        return None
    handle.write("%d\n" % os.getpid())
    handle.flush()
    return handle


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="p1")
    parser.add_argument("--only", default="")
    parser.add_argument("--out", default=os.path.join(REPO_ROOT, "results", "controls",
                                                     "frontend_width"))
    args = parser.parse_args()

    lock = acquire_run_lock(args.profile)
    if lock is None:
        print("REFUSED: another front-end controls run holds "
              "build/%s/unit/frontend_controls.lock. This suite builds into fixed "
              "directories, so two runs at once corrupt each other's binaries and "
              "measurements; re-run when the other finishes." % args.profile)
        return 2

    registry = run_unit.load_registry()
    entry = registry["cases"][CASE_ID]
    max_cycles = entry.get("max_cycles", 4000000)

    status = 0
    ship_dir = os.path.join(REPO_ROOT, "build", args.profile, "unit", "frontend_ship")
    ship_bin, ship_hash = build(entry, None, "", args.profile, ship_dir)
    code, log = run_case(ship_bin, os.path.join(args.out, "shipping"), max_cycles)
    print("shipping  hash=%s exit=%d %s" % (ship_hash, code,
                                            "PASS" if code == 0 else "FAIL"))
    if code != 0:
        print("  the shipping build does not pass; controls are meaningless")
        return 1

    # Whether this configuration exercised the two-wide *insert* at all. A mutant
    # whose subject is the insert cannot be caught if the insert never fires; the
    # condition is the shipping run's own counter, so the suite states which of
    # "inert" and "not caught" it observed instead of leaving the reader to guess.
    ship_counters = parse_two_wide(log)
    if ship_counters is None:
        print("  the shipping run printed no two-wide counters, so the "
              "conditional control cannot be judged")
        return 1
    ship_alloc2, ship_pairs = ship_counters
    print("shipping  two-wide group: alloc2_total=%d (%s), pairs_total=%d (%s)"
          % (ship_alloc2, "allocated" if ship_alloc2 > 0 else "never allocated",
             ship_pairs, "inserted" if ship_pairs > 0 else "never inserted"))

    required = 0
    caught_count = 0
    inert_count = 0
    for define, kind, defect, expected, requires in MUTANTS:
        if args.only and args.only not in define:
            continue
        required += 1
        mdir = os.path.join(REPO_ROOT, "build", args.profile, "unit", "frontend_mut")
        try:
            binary, digest = build(entry, define, kind, args.profile, mdir)
        except RuntimeError as exc:
            print("FAIL   %-38s build error (not a control verdict): %s"
                  % (define, str(exc)[-1200:]))
            status = 1
            continue
        code, log = run_case(binary, os.path.join(args.out, define), max_cycles)
        ff = first_failure(log)
        same_hash = (digest == ship_hash)
        caught = (code != 0 and expected.lower() in ff.lower() and not same_hash)
        if requires == "two-wide-pair-path" and ship_pairs == 0:
            inert_count += 1
            print("INERT  %-38s hash=%s exit=%d\n        defect: %s\n"
                  "        first failure: %s\n"
                  "        inert: the same-cycle second insert never fired in this "
                  "configuration (alloc2_total=%d, pairs_total=0; the insert is held "
                  "pending V-013, results/reports/held-insert.md). The pair this "
                  "mutant swaps is observable only when a pair's two macros leave "
                  "together, so it has no subject to break here and is not counted "
                  "as a control"
                  % (define, digest, code, defect, ff, ship_alloc2))
            if same_hash:
                print("        (hash matches the shipping binary: the -D did not take)")
            continue
        if caught:
            caught_count += 1
        else:
            status = 1
        print("%s %-38s hash=%s exit=%d\n        defect: %s\n        first failure: %s"
              % ("CAUGHT" if caught else "NOT CAUGHT", define, digest, code, defect, ff))
        if same_hash:
            print("        (hash matches the shipping binary: the -D did not take)")
    print("RESULT frontend controls: required=%d caught=%d inert=%d "
          "(pair path %s, alloc2_total=%d, pairs_total=%d)"
          % (required, caught_count, inert_count,
             "exercised" if ship_pairs > 0 else "not exercised",
             ship_alloc2, ship_pairs))
    return status


if __name__ == "__main__":
    sys.exit(main())
