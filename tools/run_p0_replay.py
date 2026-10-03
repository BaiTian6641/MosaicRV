#!/usr/bin/env python3
"""Injected-fault reset replay for CASE=p0.prototype_gate (I-080 / V-039).

    python3 tools/run_p0_replay.py

The card asks for the V-039 injected-fault reset/snapshot replay record. V-039
itself is not started, so what this tool delivers is the *reset* half, in full,
and it names the snapshot half as the gap:

  * a real, activated RTL injection (`-DMOSAIC_FAB_MUTANT_DYN_SWAP_SRC`, the
    dynamic route's operand-value wires crossed) built from an **empty** build
    directory, with its `-D` on the build-command record and a binary hash that
    differs from the shipping build;
  * the no-injection positive: the shipping build reaches the exit protocol and
    passes;
  * the injected negative: it fails, and its **first divergence** is the first
    retire event whose canonical event line differs from the positive run's,
    reported by program, event index and both lines;
  * the divergence is **reproducible**: the injected build is run several times
    as separate processes from reset, and every run must produce the *identical*
    first divergence.

What is **not** delivered, and is named rather than implied: a replay from a
mid-run RTL snapshot. There is no saveable/restorable DUT state in this project
(Verilator is not built `--savable` and V-039 is not started), so the snapshot
path cannot be produced. The reset path is the evidence that exists.

Exit status is 0 only when the positive passes, the injected build fails, and
every reset replay reproduces the same first divergence.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_core_controls as controls  # noqa: E402
import run_unit  # noqa: E402

REPO_ROOT = run_unit.REPO_ROOT
CASE = "p0.prototype_gate"
DEFAULT_DEFINE = "MOSAIC_FAB_MUTANT_DYN_SWAP_SRC"

# The programs the gate runs, in the order its driver writes them.
PROGRAMS = ["p01_addsub", "p06_shiftlogic", "p11_bigmuldiv", "p12_memwalk"]


def read_lines(path: str):
    if not os.path.exists(path):
        return None
    with open(path) as handle:
        return handle.read().splitlines()


def first_divergence(shipping_dir: str, injected_dir: str):
    """The first retire event that differs, across the programs the gate ran."""
    for program in PROGRAMS:
        name = "events.%s.txt" % program
        a = read_lines(os.path.join(shipping_dir, name))
        b = read_lines(os.path.join(injected_dir, name))
        if a is None or b is None:
            continue
        limit = min(len(a), len(b))
        for i in range(limit):
            if a[i] != b[i]:
                return {
                    "program": program,
                    "event_index": i,
                    "positive": a[i],
                    "injected": b[i],
                }
        if len(a) != len(b):
            at = limit
            return {
                "program": program,
                "event_index": at,
                "positive": a[at] if at < len(a) else "(stream ended)",
                "injected": b[at] if at < len(b) else "(stream ended)",
            }
    return None


def run_build(binary: str, out_dir: str, max_cycles: int):
    return controls.run_case(binary, CASE, out_dir, max_cycles)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--define", default=DEFAULT_DEFINE)
    parser.add_argument("--runs", type=int, default=3,
                        help="reset replays of the injected build that must agree")
    parser.add_argument("--out", default=os.path.join(REPO_ROOT, "results", "replays"))
    args = parser.parse_args()

    registry = run_unit.load_registry()["cases"]
    entry = registry[CASE]
    max_cycles = entry.get("max_cycles", 4000000)

    root = os.path.join(REPO_ROOT, "build", "p0", "p0_gate_replay")
    shipping_dir = os.path.join(root, "shipping")
    injected_dir = os.path.join(root, "injected")
    out_root = os.path.join(args.out, CASE)
    os.makedirs(out_root, exist_ok=True)

    print("building the shipping case from an empty directory...")
    controls.build(entry, CASE, [], shipping_dir)
    shipping = os.path.join(shipping_dir, CASE)
    shipping_hash = controls.sha256_of(shipping)
    shipping_out = os.path.join(out_root, "shipping")
    result = run_build(shipping, shipping_out, max_cycles)
    shipping_log = result.stdout.decode("utf-8", "replace")
    shipping_ok = result.returncode == 0 and "RESULT PASS" in shipping_log
    print("  shipping exit=%d %s" % (
        result.returncode, controls.first_result_line(shipping_log)))
    if not shipping_ok:
        print("BASELINE FAILED: the no-injection positive must pass before the "
              "injected build means anything")
        return 1

    print("building the injected case from an empty directory with -D%s..." % args.define)
    controls.build(entry, CASE, [args.define], injected_dir)
    injected = os.path.join(injected_dir, CASE)
    injected_hash = controls.sha256_of(injected)
    differs = shipping_hash != injected_hash
    print("  shipping sha256: %s" % shipping_hash)
    print("  injected sha256: %s (differs=%s)" % (injected_hash, differs))
    if not differs:
        print("the injected binary is identical to the shipping one: the define "
              "did not reach the build")
        return 1

    divergences = []
    for run_index in range(1, args.runs + 1):
        run_out = os.path.join(out_root, "injected-run%d" % run_index)
        result = run_build(injected, run_out, max_cycles)
        log = result.stdout.decode("utf-8", "replace")
        div = first_divergence(shipping_out, run_out)
        divergences.append(div)
        print("  reset replay %d: exit=%d %s" % (
            run_index, result.returncode, controls.first_result_line(log)))
        if div is None:
            print("    NO DIVERGENCE: the injected run matches the positive")
        else:
            print("    first divergence: %s event %d" % (div["program"], div["event_index"]))
            print("      positive: %s" % div["positive"])
            print("      injected: %s" % div["injected"])

    injected_failed = all(d is not None for d in divergences)
    reproducible = all(d == divergences[0] for d in divergences)

    record = {
        "schema_version": 1,
        "case": CASE,
        "injection": args.define,
        "shipping": {
            "binary_sha256": shipping_hash,
            "result": "PASS",
            "build_command": os.path.relpath(
                os.path.join(shipping_dir, "build_command.txt"), REPO_ROOT),
        },
        "injected": {
            "binary_sha256": injected_hash,
            "differs_from_shipping": differs,
            "build_command": os.path.relpath(
                os.path.join(injected_dir, "build_command.txt"), REPO_ROOT),
        },
        "reset_replays": args.runs,
        "first_divergence": divergences[0] if divergences else None,
        "reproducible": reproducible,
        "snapshot_path": {
            "delivered": False,
            "reason": "no saveable/restorable DUT state exists (Verilator is not "
                      "built --savable and V-039 is not started); only the reset "
                      "path is delivered",
        },
    }
    with open(os.path.join(out_root, "replay.json"), "w") as handle:
        json.dump(record, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print("wrote %s" % os.path.relpath(os.path.join(out_root, "replay.json"), REPO_ROOT))

    if not injected_failed:
        print("the injected build did not diverge from the positive: the injection "
              "is not observable")
        return 1
    if not reproducible:
        print("the injected build's first divergence is not reproducible across "
              "reset replays")
        return 1
    print("the injected build diverges at the same first event in all %d reset "
          "replays" % args.runs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
