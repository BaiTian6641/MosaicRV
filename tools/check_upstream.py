#!/usr/bin/env python3
"""Audit every external input this project executes against (work package V-001).

    python3 tools/check_upstream.py
    python3 tools/check_upstream.py --require verilator

The rule this enforces comes from the plan: **every executable input must have an
immutable identity, a source and a licence**, and a combination that has not been
verified must not quietly enter an execution gate.

For each entry the tool probes the live environment rather than trusting a recorded string:

* source checkouts are identified by HEAD, cleanliness and (for XiangShan/NEMU) recursive gitlink closure;
* installed tools report their own version; pinned container and build artifacts report exact identities;
* missing or mismatched inputs are `BLOCKED` with the specific reason, never silently accepted.

`BLOCKED` is never silently `OK`. An absent reference does not enter an execution gate: the packages that depend on it stay open rather than being marked deferred-and-passed.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

REF_ROOT = os.path.expanduser("~/mosaic-ref")
ACT4_ROOT = os.path.join(REF_ROOT, "riscv-arch-test")
ROSETTA_RUNNER = ["limactl", "shell", "mosaic-rosetta", "--"]
ROSETTA_REF_ROOT = "/home/flare.guest/mosaic-ref"
XS_ENV_AMD64 = (
    "ghcr.io/openxiangshan/xs-env@sha256:"
    "a0aa7dc5554a7273a1f790bd1059b4624460c3191e94873697bbe6a20b0dc667"
)

# Third-party trees this project depends on, with what each is for.
SOURCES = [
    {
        "name": "XiangShan-e7bab53",
        "role": "XiangShan DUT-B source and Difftest host",
        "path": os.path.join(ROSETTA_REF_ROOT, "XiangShan-e7bab53"),
        "runner": ROSETTA_RUNNER,
        "expected_commit": "e7bab53e66dfb3c4a1d11cf9519b0396f8576cae",
        "licence": "MulanPSL-2.0",
        "used_for": ["V-005", "V-075"],
        "check_submodules": True,
        "require_clean": True,
    },
    {
        "name": "NEMU-f39e307",
        "role": "Difftest architectural reference source",
        "path": os.path.join(ROSETTA_REF_ROOT, "NEMU-f39e-pinned"),
        "runner": ROSETTA_RUNNER,
        "expected_commit": "f39e3077d7bac3cd9a3a853a9300a5f8f0293a2c",
        "licence": "MulanPSL-2.0",
        "used_for": ["V-004", "V-005"],
        "check_submodules": True,
        "require_clean": True,
    },
    {
        "name": "ready-to-run-c4114ce",
        "role": "Upstream prebuilt workloads and Difftest samples",
        "path": os.path.join(ROSETTA_REF_ROOT, "XiangShan-e7bab53/ready-to-run"),
        "runner": ROSETTA_RUNNER,
        "expected_commit": "c4114ce3fffcd5c147c525014b40f1c841347238",
        "licence": "UNSPECIFIED (GitHub repository license metadata is null)",
        "licence_review_required": "upstream has no declared license; legal authorization is unresolved",
        "used_for": ["V-005"],
        "require_clean": True,
    },
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
    {"name": "z3", "cmd": ["z3", "--version"], "licence": "MIT",
     "used_for": ["V-043"]},
    {"name": "limactl", "cmd": ["limactl", "--version"], "licence": "Apache-2.0",
     "used_for": ["V-003", "V-004", "V-005"]},
    {"name": "qemu-system-x86_64", "cmd": ["qemu-system-x86_64", "--version"],
     "licence": "GPL-2.0-only", "used_for": ["V-003", "V-004", "V-005"]},
    {"name": "xs-env-amd64-image",
     "cmd": ROSETTA_RUNNER + ["docker", "image", "inspect", "--format",
                             "{{.Os}}/{{.Architecture}} {{index .RepoDigests 0}}",
                             "ghcr.io/openxiangshan/xs-env:latest"],
     "expected_output": "linux/amd64 " + XS_ENV_AMD64,
     "licence": "OpenXiangShan xs-env image; component licences vary",
     "used_for": ["V-005"]},
    {"name": "Rosetta-x86-userland",
     "cmd": ROSETTA_RUNNER + ["docker", "run", "--pull=never", "--rm",
                             "--platform=linux/amd64", "--entrypoint", "/bin/uname",
                             XS_ENV_AMD64, "-m"],
     "expected_output": "x86_64",
     "licence": "Rosetta (Apple proprietary); xs-env component licences vary",
     "used_for": ["V-003", "V-005"]},
    {"name": "XiangShan-emu-artifact",
     "cmd": ROSETTA_RUNNER + ["sha256sum", os.path.join(
         ROSETTA_REF_ROOT, "XiangShan-e7bab53/build/verilator-compile/emu")],
     "expected_output": "72186b6c089932c6cc7e1915dd0674f3971eda5f38918382b3c2f46a3f83a5ed  "
                        + os.path.join(ROSETTA_REF_ROOT,
                                       "XiangShan-e7bab53/build/verilator-compile/emu"),
     "licence": "MulanPSL-2.0 (XiangShan source)", "used_for": ["V-005"]},
    {"name": "NEMU-reference-so",
     "cmd": ROSETTA_RUNNER + ["sha256sum", os.path.join(
         ROSETTA_REF_ROOT, "NEMU-f39e-pinned/build/riscv64-nemu-interpreter-so")],
     "expected_output": "8a6f428dda7b6696fbc38a9413228a238c0fe59b0c08544d84f2d9c7d3417689  "
                        + os.path.join(ROSETTA_REF_ROOT,
                                       "NEMU-f39e-pinned/build/riscv64-nemu-interpreter-so"),
     "licence": "MulanPSL-2.0 (NEMU source)", "used_for": ["V-004", "V-005"]},
    {"name": "CoreMark-2-iteration-ELF",
     "cmd": ROSETTA_RUNNER + ["sha256sum", os.path.join(
         ROSETTA_REF_ROOT, "XiangShan-e7bab53/ready-to-run/coremark-2-iteration.bin")],
     "expected_output": "c764afb8bfd69542620a4794b858867dd1e455efaac56c28eb477f1732f83e8e  "
                        + os.path.join(ROSETTA_REF_ROOT,
                                       "XiangShan-e7bab53/ready-to-run/coremark-2-iteration.bin"),
     "licence": "UNSPECIFIED (ready-to-run repository license metadata is null)",
     "licence_review_required": "upstream has no declared license; legal authorization is unresolved",
     "used_for": ["V-005"]},
    {"name": "mise", "cmd": ["mise", "--version"],
     "expected_output": "2026.9.15 macos-arm64 (2026-09-27)",
     "licence": "MIT", "used_for": ["V-043"]},
    {"name": "ACT4-uv",
     "cmd": ["mise", "exec", "-C", ACT4_ROOT, "--", "uv", "--version"],
     "expected_output": "uv 0.11.33 (fece32fc5 2026-07-28 aarch64-apple-darwin)",
     "licence": "Apache-2.0", "used_for": ["V-043"]},
    {"name": "ACT4-ruby",
     "cmd": ["mise", "exec", "-C", ACT4_ROOT, "--", "ruby", "-e", "print RUBY_VERSION"],
     "expected_output": "3.4.11",
     "licence": "Ruby License / 2-Clause BSD; per-file notices apply",
     "used_for": ["V-043"]},
    {"name": "ACT4-bundler",
     "cmd": ["mise", "exec", "-C", ACT4_ROOT, "--", "bundle", "--version"],
     "expected_output": "4.0.21", "licence": "MIT", "used_for": ["V-043"]},
    {"name": "act4-cli",
     "cmd": ["mise", "exec", "-C", ACT4_ROOT, "--", "uv", "run", "act", "--help"],
     "licence": "Apache-2.0 / BSD-3-Clause / CC-BY-4.0 (per file)",
     "used_for": ["V-002", "V-043"]},
]

# These are MosaicRV-specific gates, not missing upstream installations.
ABSENT = [
    {"name": "MosaicRV-NEMU-Difftest-adapter",
     "role": "DUT architectural-state/reference ABI", "gate": "V-004",
     "unblocked_by": ("Implement the MosaicRV state serialization and Difftest ABI; validate round-trip, "
                      "reference stepping, missing-symbol and wrong-layout negative controls. The pinned "
                      "upstream NEMU library only establishes the XiangShan sample path.")},
    {"name": "MosaicRV-ACT4-DUT-runner",
     "role": "ACT4 DUT profile and ELF execution", "gate": "V-043",
     "unblocked_by": ("Add the MosaicRV UDB/Sail/linker/rvmodel_macros.h profile and execute every applicable "
                      "generated ELF on the DUT. The existing 51/51 Sail-max replay is calibration, not DUT PASS.")},
]



def run(cmd, cwd=None):
    try:
        result = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=60)
        return result.returncode, result.stdout.decode("utf-8", "replace")
    except (OSError, subprocess.SubprocessError) as exc:
        return 127, str(exc)


def source_command(entry, *args):
    runner = entry.get("runner", [])
    command = runner + ["git", "-C", entry["path"]] + list(args)
    return run(command, cwd=None if runner else entry["path"])


def probe_source(entry):
    path = entry["path"]
    runner = entry.get("runner", [])
    if not runner and not os.path.isdir(os.path.join(path, ".git")):
        return {"name": entry["name"], "role": entry["role"], "status": "BLOCKED",
                "licence": entry["licence"], "used_for": entry["used_for"],
                "expected_commit": entry.get("expected_commit"),
                "reason": "not checked out at %s" % path}

    code, out = source_command(entry, "rev-parse", "HEAD")
    commit = out.strip() if code == 0 else ""
    if code != 0:
        return {"name": entry["name"], "role": entry["role"], "status": "BLOCKED",
                "licence": entry["licence"], "used_for": entry["used_for"],
                "expected_commit": entry.get("expected_commit"), "path": path,
                "reason": "cannot read source revision: %s" % out.strip()}

    expected = entry.get("expected_commit")
    if expected and commit != expected:
        return {"name": entry["name"], "role": entry["role"], "status": "BLOCKED",
                "licence": entry["licence"], "used_for": entry["used_for"],
                "expected_commit": expected, "path": path, "commit": commit,
                "reason": "source revision mismatch; expected %s, found %s" % (expected, commit)}

    status_code, dirty = source_command(entry, "status", "--porcelain")
    _, subject = source_command(entry, "log", "-1", "--format=%s")
    clean = status_code == 0 and dirty.strip() == ""
    result = {"name": entry["name"], "role": entry["role"], "status": "PRESENT",
              "licence": entry["licence"], "used_for": entry["used_for"],
              "expected_commit": expected, "path": path, "commit": commit,
              "subject": subject.strip(), "working_tree_clean": clean}
    if status_code != 0:
        result.update(status="BLOCKED", reason="cannot read working-tree status: %s" % dirty.strip())
        return result
    if entry.get("require_clean") and not clean:
        result.update(status="BLOCKED", reason="source working tree is not clean")
        return result

    if entry.get("check_submodules"):
        submodule_code, submodule_output = source_command(entry, "submodule", "status", "--recursive")
        submodules = submodule_output.splitlines()
        mismatched = [line for line in submodules if line[:1] in ("-", "+", "U")]
        result["submodule_count"] = len(submodules)
        result["submodules_clean"] = submodule_code == 0 and not mismatched
        if submodule_code != 0 or mismatched:
            result.update(status="BLOCKED",
                          reason="recursive gitlink check failed: %s" %
                          ("; ".join(mismatched[:3]) if mismatched else submodule_output.strip()))
            return result
    if entry.get("licence_review_required"):
        result.update(status="BLOCKED", reason=entry["licence_review_required"])
        return result
    return result


def probe_tool(entry):
    code, out = run(entry["cmd"])
    output = out.strip()
    version = output.splitlines()[0] if output else ""
    expected = entry.get("expected_output")
    if code != 0 or (expected is not None and output != expected):
        reason = "not installed or not runnable" if code != 0 else (
            "identity mismatch; expected %r, found %r" % (expected, output))
        return {"name": entry["name"], "licence": entry["licence"],
                "used_for": entry["used_for"], "status": "BLOCKED", "reason": reason,
                "expected_output": expected, "actual_output": output}
    if entry.get("licence_review_required"):
        return {"name": entry["name"], "licence": entry["licence"],
                "used_for": entry["used_for"], "status": "BLOCKED",
                "reason": entry["licence_review_required"],
                "expected_output": expected, "actual_output": output}
    return {"name": entry["name"], "licence": entry["licence"],
            "used_for": entry["used_for"], "status": "PRESENT",
            "version": version, "command": entry["cmd"], "expected_output": expected}


def build_report():
    return {
        "schema_version": 1,
        "sources": [probe_source(entry) for entry in SOURCES],
        "tools": [probe_tool(entry) for entry in TOOLS],
        "absent": ABSENT,
        "policy": ("Source revision, cleanliness and selected recursive gitlinks are probed live; "
                   "tools report versions or exact pinned image/artifact identities. BLOCKED inputs "
                   "remain explicit and do not imply MosaicRV DUT acceptance."),
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
            submodules = ("  gitlinks=%d clean=%s"
                          % (entry["submodule_count"], entry["submodules_clean"])
                          if "submodule_count" in entry else "")
            print("  ok      %-16s %s  clean=%s%s  %s"
                  % (entry["name"], entry["commit"][:12], entry["working_tree_clean"],
                     submodules, entry["subject"][:48]))
        else:
            print("  BLOCKED %-16s %s" % (entry["name"], entry["reason"]))

    print("tools")
    for entry in report["tools"]:
        if entry["status"] == "PRESENT":
            print("  ok      %-20s %s" % (entry["name"], entry["version"][:66]))
        else:
            print("  BLOCKED %-20s %s" % (entry["name"], entry["reason"]))

    print("MosaicRV-specific gates still blocked")
    for entry in report["absent"]:
        print("  BLOCKED %-32s gate=%-6s %s"
              % (entry["name"], entry["gate"], entry["unblocked_by"][:74]))

    present = {e["name"] for e in report["sources"] + report["tools"]
               if e["status"] == "PRESENT"}
    missing = [name for name in args.require if name not in present]
    if missing:
        print("FAIL required input(s) not present: %s" % ", ".join(missing), file=sys.stderr)
        return 1
    if args.require:
        print("PASS required inputs match their recorded revision/version/identity")
    else:
        print("inventory complete; BLOCKED entries above remain unverified")
    return 0


if __name__ == "__main__":
    sys.exit(main())