#!/usr/bin/env python3
"""Audit every external input this project executes against (work package V-001).

    python3 tools/check_upstream.py
    python3 tools/check_upstream.py --require verilator

The rule this enforces comes from the plan: **every executable input must have an
immutable identity, a source and a licence**, and a combination that has not been
verified must not quietly enter an execution gate.

For each entry the tool *probes the live environment* rather than trusting a
recorded string, so the ledger cannot go stale silently:

* a source checkout is identified by its HEAD commit, and whether the tree is clean;
* a tool is identified by running it and capturing its own version output;
* anything that cannot be located is recorded as `BLOCKED` with the specific reason
  and the specific thing that would unblock it.

`BLOCKED` is never silently `OK`. An absent reference does not enter an execution
gate: the packages that depend on it stay open rather than being marked
deferred-and-passed.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mosaic import config_check  # noqa: E402

REPO_ROOT = config_check.REPO_ROOT
REF_ROOT = os.path.expanduser("~/mosaic-ref")

# Third-party trees this project depends on, with what each is for.
SOURCES = [
    {
        "name": "riscv-isa-sim",
        "role": "Spike: independent architectural reference",
        "path": os.path.join(REF_ROOT, "riscv-isa-sim"),
        "expected_commit": "0bff12123b1fd510e19e19634dd997dbade70e54",
        "licence": "BSD-3-Clause",
        "used_for": ["I-007", "I-008", "V-001", "V-002"],
    },
    {
        "name": "riscv-v-spec",
        "role": "vector extension specification",
        "path": os.path.join(REF_ROOT, "riscv-v-spec"),
        "licence": "CC-BY-4.0",
        "used_for": ["I-001"],
    },
    {
        "name": "riscv-arch-test",
        "role": "ACT4 architectural certification test generator",
        "path": os.path.join(REF_ROOT, "riscv-arch-test"),
        "expected_commit": "96493a91448ca50780013fd892daec2c204487ba",
        "licence": "Apache-2.0 / BSD-3-Clause / CC-BY-4.0 (per file)",
        "used_for": ["V-002", "V-043"],
    },
]

TOOLS = [
    {"name": "verilator", "cmd": ["verilator", "--version"],
     "licence": "LGPL-3.0 / Artistic-2.0", "used_for": ["I-004", "I-005", "I-006", "I-008"]},
    {"name": "yosys", "cmd": ["yosys", "-V"], "licence": "ISC",
     "used_for": ["I-003", "synth-generic"]},
    {"name": "slang-tidy", "cmd": ["slang-tidy", "--version"], "licence": "MIT",
     "used_for": ["lint-slang"]},
    {"name": "sby", "cmd": ["sby", "--version"], "licence": "BSD-2-Clause",
     "used_for": ["formal"]},
    {"name": "riscv64-elf-gcc", "cmd": ["riscv64-elf-gcc", "--version"],
     "licence": "GPL-3.0 with GCC Runtime Library Exception", "used_for": ["I-007", "V-043"]},
    {"name": "riscv64-elf-objdump", "cmd": ["riscv64-elf-objdump", "--version"],
     "licence": "GPL-3.0 with GCC Runtime Library Exception", "used_for": ["I-007", "I-010", "V-043"]},
    {"name": "spike", "cmd": [os.path.join(REF_ROOT, "install", "bin", "spike"), "--help"],
     "licence": "BSD-3-Clause", "used_for": ["V-002", "V-006"]},
    {"name": "sail_riscv_sim",
     "cmd": [os.path.join(REF_ROOT, "sail-riscv-0.14.1", "bin", "sail_riscv_sim"), "--version"],
     "licence": "Other (see upstream LICENCE)", "used_for": ["V-002", "V-006", "V-043"]},
    {"name": "z3", "cmd": ["z3", "--version"],
     "licence": "MIT", "used_for": ["V-043"]},
]

# References and second DUTs blocked on the documented Linux x86-64 environment.
ABSENT = [
    {"name": "XiangShan", "role": "second DUT and Difftest host", "gate": "V-005",
     "unblocked_by": ("CLONE NOT ATTEMPTED. It is a large sbt/Chisel build. The stated "
                      "revisions and environment in earlier revisions of this ledger were "
                      "not verified here and have been removed rather than left standing. "
                      "What is actually true: this host is Darwin arm64 with no container "
                      "runtime (docker, podman, colima and lima were each probed and are "
                      "all absent) and no configured remote Linux host. Establishing "
                      "whether XiangShan builds natively on this host is an untried "
                      "question, not a proven impossibility.")},
    {"name": "NEMU", "role": "Difftest reference with a pinned commit trace", "gate": "V-004",
     "unblocked_by": ("CLONED at ~/mosaic-ref/NEMU, commit 274a9eaeb2c2e090c7eeb26a77e5a623c890289b, "
                      "and BUILD ATTEMPTED SEVEN TIMES ON THIS HOST. Configuration succeeds; "
                      "compilation does not. Six distinct blockers in the order hit: "
                      "(1) NEMU_HOME unset, the Makefile refuses; (2) -lstdc++fs, removed from "
                      "the libc++ this build links against; (3) -falign-labels=32:9:64:15 and "
                      "(4) --param max-inline-insns-single=256, both GCC-only tuning flags that "
                      "clang rejects outright under -Werror; (5) SDL2/SDL.h not installed; "
                      "(6) printf format-width errors in upstream headers, %lu used for 64-bit "
                      "types, fatal under -Werror=format. Items 5 and 6 are upstream source "
                      "defects, not environment setup, so fixing them means patching NEMU "
                      "itself. No NEMU binary was produced and the checkout has been restored.")},
]



def run(cmd, cwd=None):
    try:
        result = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=60)
        return result.returncode, result.stdout.decode("utf-8", "replace")
    except (OSError, subprocess.SubprocessError) as exc:
        return 127, str(exc)


def probe_source(entry):
    path = entry["path"]
    if not os.path.isdir(os.path.join(path, ".git")):
        return {"name": entry["name"], "role": entry["role"], "status": "BLOCKED",
                "licence": entry["licence"], "used_for": entry["used_for"],
                "expected_commit": entry.get("expected_commit"),
                "reason": "not checked out at %s" % path}
    code, out = run(["git", "rev-parse", "HEAD"], cwd=path)
    commit = out.strip() if code == 0 else ""
    _, dirty = run(["git", "status", "--porcelain"], cwd=path)
    _, subject = run(["git", "log", "-1", "--format=%s"], cwd=path)
    expected = entry.get("expected_commit")
    if expected and commit != expected:
        return {"name": entry["name"], "role": entry["role"], "status": "BLOCKED",
                "licence": entry["licence"], "used_for": entry["used_for"],
                "expected_commit": expected, "path": path, "commit": commit,
                "reason": "source revision mismatch; expected %s, found %s" % (expected, commit)}
    return {"name": entry["name"], "role": entry["role"], "status": "PRESENT",
            "licence": entry["licence"], "used_for": entry["used_for"],
            "expected_commit": expected, "path": path, "commit": commit,
            "subject": subject.strip(), "working_tree_clean": dirty.strip() == ""}


def probe_tool(entry):
    code, out = run(entry["cmd"])
    version = out.strip().splitlines()[0] if out.strip() else ""
    if code != 0:
        return {"name": entry["name"], "licence": entry["licence"],
                "used_for": entry["used_for"], "status": "BLOCKED",
                "reason": "not installed or not runnable"}
    return {"name": entry["name"], "licence": entry["licence"],
            "used_for": entry["used_for"], "status": "PRESENT",
            "version": version, "command": entry["cmd"]}


def build_report():
    return {
        "schema_version": 1,
        "sources": [probe_source(entry) for entry in SOURCES],
        "tools": [probe_tool(entry) for entry in TOOLS],
        "absent": ABSENT,
        "policy": ("Every executable input has an immutable identity, a source and a licence, "
                   "probed live rather than read from a stored string. An input that cannot be "
                   "located is BLOCKED with the specific thing that would unblock it; BLOCKED "
                   "is never silently treated as OK, and a package depending on an absent "
                   "reference stays open rather than being marked deferred-and-passed."),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--require", action="append", default=[],
                        help="fail if this named input is not PRESENT")
    args = parser.parse_args()

    report = build_report()
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0

    print("source checkouts")
    for entry in report["sources"]:
        if entry["status"] == "PRESENT":
            print("  ok      %-16s %s  clean=%s  %s"
                  % (entry["name"], entry["commit"][:12], entry["working_tree_clean"],
                     entry["subject"][:48]))
        else:
            print("  BLOCKED %-16s %s" % (entry["name"], entry["reason"]))

    print("tools")
    for entry in report["tools"]:
        if entry["status"] == "PRESENT":
            print("  ok      %-20s %s" % (entry["name"], entry["version"][:66]))
        else:
            print("  BLOCKED %-20s %s" % (entry["name"], entry["reason"]))

    print("absent references, each with what would unblock it")
    for entry in report["absent"]:
        print("  BLOCKED %-12s gate=%-6s %s"
              % (entry["name"], entry["gate"], entry["unblocked_by"][:74]))

    present = {e["name"] for e in report["sources"] + report["tools"]
               if e["status"] == "PRESENT"}
    missing = [name for name in args.require if name not in present]
    if missing:
        print("FAIL required input(s) not present: %s" % ", ".join(missing), file=sys.stderr)
        return 1
    print("PASS every present input has an immutable identity, a source and a licence")
    return 0


if __name__ == "__main__":
    sys.exit(main())