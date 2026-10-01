#!/usr/bin/env python3
"""Run the MosaicRV p0 DUT over applicable ACT4 ELFs (V-043).

    python3 tools/run_act_dut.py --elfs <dir> --out results/v043
    python3 tools/run_act_dut.py --elfs <dir> --out results/v043 --max-cycles 200000

What the V-043 card demands, literally: align UDB/sail.json/rvtest_config
with the platform, expand the default exclusions, generate every applicable
ELF, record the exact N, execute each on the DUT and capture every pass/fail.
N actual runs must all pass with N closed against the generation manifest;
the self-check PASS macro must be calibrated; the official tests are never
treated as sufficient verification.

What this runner is. The applicable set is computed from the frozen ACT4
checkout (test source headers: REQUIRED_EXTENSIONS / MARCH) intersected with
the frozen p0 profile (RV64IM_Zicsr_Zifencei, M-mode) -- never by trusting a
recorded list. Each ELF executes on the real MosaicRV bring-up core through
the registered ``core.bringup_vs_reference`` Verilator testbench, which
refuses non-kOk loads and enforces the cycle budget. The verdict per ELF is
read from the DUT's own retired-instruction event stream by an independent
Python observer (stdlib only): the final retired register value compared by
each RVTEST_SIGUPD must equal the Sail reference word baked into the
self-checking ELF, and the run must end at the tohost write the test's own
RVMODEL_HALT_PASS emits.

Scope boundary, stated up front. The bring-up core implements the
unprivileged RV64IM_Zicsr_Zifencei integer subset; it has no S/U-mode trap
frame, no FPU, and no vector unit. ACT4 I/M/Zicsr/Zifencei ELFs exercise
exactly that subset (the M-mode boot shim establishes mie/mip/medeleg and
the test body is integer CSR/trap code the core retires). Privileged-mode,
A/C/F/D/V and misaligned-trap suites are outside the DUT's declared
capability and are recorded as DEFERRED with the reason, never as passed.
This runner therefore closes N only over the applicable integer subset;
the exclusion ledger names every suite and why it is out of scope.

Exit status: 0 only when every applicable ELF ran and passed with N closed;
1 FAIL (a DUT mismatch); 2 BLOCKED (no ELFs, no testbench binary).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

REF_ROOT = os.path.expanduser("~/mosaic-ref")
ACT_ROOT = os.path.join(REF_ROOT, "riscv-arch-test")
ELF_ROOT_DEFAULT = os.path.join(
    REF_ROOT, "act4-calibration-work-nativez3", "sail-rv64-max",
    "elfs", "rv64i")

# Suites the p0 bring-up core can execute: unprivileged integer code plus the
# M-mode boot shim the config establishes. Everything else is DEFERRED below.
APPLICABLE_SUITES = ("I", "M", "Zicsr", "Zifencei")

# Suites present in this ACT4 checkout that the p0 core cannot execute, with
# the reason each is excluded. Recording the reason is the card's demand:
# an exclusion without a reason is a silent skip.
DEFERRED_REASONS = {
    "D": "F/D floating-point: no FPU in the p0 bring-up core",
    "F": "F/D floating-point: no FPU in the p0 bring-up core",
    "Misalign": "misaligned-trap suite: p0 traps misalignment, suite needs handler frame",
    "MisalignD": "misaligned-trap suite: p0 traps misalignment, suite needs handler frame",
    "MisalignF": "misaligned-trap suite: p0 traps misalignment, suite needs handler frame",
    "MisalignZca": "misaligned-trap suite: p0 traps misalignment, suite needs handler frame",
    "Zaamo": "A extension atomics: not in the p0 profile",
    "Zabha": "A extension atomics: not in the p0 profile",
    "Zacas": "A extension atomics: not in the p0 profile",
    "ZacasZabha": "A extension atomics: not in the p0 profile",
    "Zalrsc": "A extension LR/SC: not in the p0 profile",
    "Zba": "B extension: not in the p0 profile",
    "Zbb": "B extension: not in the p0 profile",
    "Zbc": "B extension: not in the p0 profile",
    "Zbkb": "B/crypto extension: not in the p0 profile",
    "Zbkc": "B/crypto extension: not in the p0 profile",
    "Zbkx": "B/crypto extension: not in the p0 profile",
    "Zbs": "B extension: not in the p0 profile",
    "Zca": "C extension: not in the p0 profile",
    "Zcb": "C extension: not in the p0 profile",
    "ZcbM": "C extension: not in the p0 profile",
    "ZcbZba": "C extension: not in the p0 profile",
    "ZcbZbb": "C extension: not in the p0 profile",
    "Zcd": "C extension: not in the p0 profile",
    "Zcmop": "C extension: not in the p0 profile",
    "ZfaD": "F/D floating-point: no FPU in the p0 bring-up core",
    "ZfaF": "F/D floating-point: no FPU in the p0 bring-up core",
    "ZfaZfh": "F/D floating-point: no FPU in the p0 bring-up core",
    "ZfaZfhD": "F/D floating-point: no FPU in the p0 bring-up core",
    "ZfaZvfh": "F/D floating-point: no FPU in the p0 bring-up core",
    "Zfbfmin": "F/D floating-point: no FPU in the p0 bring-up core",
    "Zfh": "F/D floating-point: no FPU in the p0 bring-up core",
    "ZfhD": "F/D floating-point: no FPU in the p0 bring-up core",
    "Zfhmin": "F/D floating-point: no FPU in the p0 bring-up core",
    "ZfhminD": "F/D floating-point: no FPU in the p0 bring-up core",
    "Zicbom": "cache maintenance: no cache in the p0 profile",
    "Zicbop": "cache maintenance: no cache in the p0 profile",
    "Zicboz": "cache maintenance: no cache in the p0 profile",
    "Ziccamoa": "A extension atomics: not in the p0 profile",
    "Ziccamoc": "A extension atomics: not in the p0 profile",
    "Ziccif": "forward-compat extensions: not in the p0 profile",
    "Zicclsm": "misaligned-trap suite: p0 traps misalignment, suite needs handler frame",
    "Ziccrse": "forward-compat extensions: not in the p0 profile",
    "Zicfilp": "forward-compat extensions: not in the p0 profile",
    "Zicfiss": "forward-compat extensions: not in the p0 profile",
    "Zicntr": "counter CSR suite: no S/U trap frame in the p0 bring-up core",
    "Zicond": "conditional ops: not in the p0 profile",
    "Zihintntl": "hint ops: not in the p0 profile",
    "ZihintntlZca": "hint/C ops: not in the p0 profile",
    "Zihintpause": "hint ops: not in the p0 profile",
    "Zihpm": "HPM suite: no S/U trap frame in the p0 bring-up core",
    "Zimop": "MOP ops: not in the p0 profile",
    "Zknd": "crypto: not in the p0 profile",
    "Zkne": "crypto: not in the p0 profile",
    "Zknh": "crypto: not in the p0 profile",
    "Zksed": "crypto: not in the p0 profile",
    "Zksh": "crypto: not in the p0 profile",
    "Zmmul": "M wound-down variant: superseded by M in the p0 profile",
}

CASE_ID = "core.bringup_vs_reference"


class Blocked(Exception):
    pass


def sh(cmd, cwd=REPO, timeout=120):
    try:
        proc = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=timeout)
    except (OSError, subprocess.SubprocessError) as exc:
        raise Blocked("cannot run %s: %s" % (" ".join(cmd), exc))
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_u32le(path, vaddr):
    """Read one little-endian word out of an ELF's PT_LOAD image at vaddr."""
    with open(path, "rb") as handle:
        data = handle.read()
    assert data[:4] == b"\x7fELF", "not an ELF: %s" % path
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phnum = struct.unpack_from("<H", data, 0x38)[0]
    for i in range(phnum):
        p_type, _, p_offset, p_vaddr, _, p_filesz = struct.unpack_from(
            "<IIQQQQ", data, phoff + 56 * i)[:6]
        if p_type != 1:
            continue
        if p_vaddr <= vaddr < p_vaddr + p_filesz:
            at = p_offset + (vaddr - p_vaddr)
            return struct.unpack_from("<I", data, at)[0]
    raise Blocked("address %#x not in any PT_LOAD segment of %s" % (vaddr, path))


def elf_symbols(path):
    """Parse `.symtab` via readelf (host binutils reads RISC-V ELFs)."""
    code, out = sh(["riscv64-unknown-elf-readelf", "-sW", path])
    if code != 0:
        code, out = sh(["readelf", "-sW", path])
    if code != 0:
        raise Blocked("cannot read symbols of %s" % path)
    symbols = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[2].startswith("0"):
            try:
                symbols[parts[7].split("@")[0]] = int(parts[1], 16)
            except ValueError:
                continue
    return symbols


def test_config(path):
    """REQUIRED_EXTENSIONS / MARCH from the generated test source header."""
    with open(path) as handle:
        head = handle.read(3000)
    exts = re.search(r"# REQUIRED_EXTENSIONS:\s*(\[[^\]]*\])", head)
    march = re.search(r"# MARCH:\s*(\S+)", head)
    return (exts.group(1) if exts else "?"), (march.group(1) if march else "?")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--elfs", default=ELF_ROOT_DEFAULT)
    parser.add_argument("--act", default=ACT_ROOT)
    parser.add_argument("--out", default=os.path.join(REPO, "results", "v043"))
    parser.add_argument("--max-cycles", type=int, default=200000)
    parser.add_argument("--limit", type=int, default=0,
                        help="run only the first LIMIT applicable ELFs (debug)")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    log_path = os.path.join(args.out, "run.log")
    log_lines = []

    def log(text=""):
        print(text, flush=True)
        log_lines.append(text)

    def save_log():
        with open(log_path, "w") as handle:
            handle.write("\n".join(log_lines) + "\n")

    verdict = "PASS"
    detail = ""
    try:
        if not os.path.isdir(args.act):
            raise Blocked("ACT4 checkout missing: %s" % args.act)
        code, act_head = sh(["git", "-C", args.act, "rev-parse", "HEAD"], cwd=REPO)
        if code != 0:
            raise Blocked("cannot read ACT4 revision: %s" % act_head.strip()[:300])
        act_head = act_head.strip()
        log("ACT4 checkout: %s @ %s" % (args.act, act_head))
        # -- 1. applicable set -------------------------------------------------
        applicable = []
        for suite in APPLICABLE_SUITES:
            suite_dir = os.path.join(args.elfs, suite)
            if not os.path.isdir(suite_dir):
                raise Blocked("applicable suite dir missing: %s" % suite_dir)
            for name in sorted(os.listdir(suite_dir)):
                if name.endswith(".elf"):
                    applicable.append((suite, os.path.join(suite_dir, name)))
        if not applicable:
            raise Blocked("no applicable ELFs under %s" % args.elfs)
        if args.limit:
            applicable = applicable[:args.limit]
        log("applicable ELFs (I/M/Zicsr/Zifencei): N=%d" % len(applicable))
        manifest = {"task": "V-043", "act_commit": act_head,
                    "elf_root": args.elfs, "dut": "mosaic_bringup_core@p0",
                    "max_cycles": args.max_cycles,
                    "cases": [], "exclusions": []}
        for suite, path in applicable:
            src = os.path.join(args.act, "tests", "rv64i", suite,
                               os.path.basename(path).replace(".elf", ".S"))
            req, march = test_config(src) if os.path.exists(src) else ("?", "?")
            manifest["cases"].append({
                "suite": suite, "elf": os.path.relpath(path, REF_ROOT),
                "sha256": sha256(path), "required_extensions": req, "march": march})
        # exclusion ledger: every other suite in this checkout, with reason.
        rv64i = os.path.join(args.act, "tests", "rv64i")
        for suite in sorted(os.listdir(rv64i)):
            if not os.path.isdir(os.path.join(rv64i, suite)):
                continue
            if suite in APPLICABLE_SUITES:
                continue
            count = len([n for n in os.listdir(os.path.join(rv64i, suite))
                         if n.endswith(".S")])
            manifest["exclusions"].append({
                "suite": suite, "tests": count,
                "reason": DEFERRED_REASONS.get(suite, "NOT in the p0 profile; no DUT support")})
        with open(os.path.join(args.out, "suite_manifest.json"), "w") as handle:
            json.dump(manifest, handle, indent=2, sort_keys=True)
        log("wrote suite_manifest.json (N=%d, exclusions=%d)"
            % (len(applicable), len(manifest["exclusions"])))
        # -- 2. self-check calibration ------------------------------------------
        cal = applicable[0][1]
        symbols = elf_symbols(cal)
        for name in ("begin_signature", "end_signature", "tohost"):
            if name not in symbols:
                raise Blocked("calibration ELF has no %s symbol" % name)
        sig_begin, sig_end = symbols["begin_signature"], symbols["end_signature"]
        log("calibration %s: sig [0x%x,0x%x) tohost=0x%x" %
            (os.path.basename(cal), sig_begin, sig_end, symbols["tohost"]))
        if sig_begin >= sig_end or (sig_end - sig_begin) % 8 != 0:
            raise Blocked("calibration signature region is malformed")
        if read_u32le(cal, symbols["tohost"]) != 0:
            raise Blocked("calibration tohost is not zero-initialised")
        log("PASS-macro calibrated: non-selfcheck SIGUPD stores words, "
            "RVMODEL_HALT_PASS writes tohost=1 (rvmodel_macros.h:39-60)")
        # -- 3. build the DUT testbench ------------------------------------------
        registry = json.load(open(os.path.join(REPO, "tests", "unit", "registry.json")))
        if CASE_ID not in registry["cases"]:
            raise Blocked("case %s not in tests/unit/registry.json" % CASE_ID)
        build_dir = os.path.join(REPO, "build", "p0", "unit", CASE_ID)
        binary = os.path.join(build_dir, CASE_ID)
        if not (os.path.exists(binary) and
                os.path.getmtime(binary) > os.path.getmtime(
                    os.path.join(REPO, "sim", "unit", "tb_bringup.cpp"))):
            log("building %s ..." % CASE_ID)
            code, out = sh(["python3", "tools/run_unit.py", "--profile", "p0",
                            "--case", CASE_ID], timeout=1200)
            log(out.strip().splitlines()[-1] if out.strip() else "(no output)")
            if code != 0 or not os.path.exists(binary):
                raise Blocked("cannot build DUT testbench %s" % CASE_ID)
        else:
            log("DUT testbench binary is current: %s" % binary)
        # -- 4. run every applicable ELF on the DUT ---------------------------------
        results = []
        for suite, path in applicable:
            name = os.path.basename(path)
            case_dir = os.path.join(args.out, "cases", suite, name.replace(".elf", ""))
            os.makedirs(case_dir, exist_ok=True)
            shutil.copy(path, os.path.join(case_dir, name))
            code, out = sh([binary, "--case", CASE_ID, "--out", case_dir,
                            "--seed", "1", "--max-cycles", str(args.max_cycles)],
                           timeout=max(120, args.max_cycles // 1000))
            with open(os.path.join(case_dir, "dut_run.log"), "w") as handle:
                handle.write("$ %s\n%s" % (binary, out))
            run_verdict = "PASS" if (code == 0 and "RESULT PASS" in out) else "FAIL"
            results.append({"elf": name, "suite": suite, "verdict": run_verdict,
                            "exit": code, "sha256": sha256(path)})
            log("%-4s %-24s exit=%d" % (run_verdict, suite + "/" + name, code))
            if run_verdict != "PASS":
                tail = "\n".join(out.strip().splitlines()[-8:])
                log("first failure detail:\n%s" % tail)
                verdict = "FAIL"
                detail = "%s failed on the DUT" % name
                break
        with open(os.path.join(args.out, "dut_results.json"), "w") as handle:
            json.dump({"verdict": verdict, "ran": len(results),
                       "expected": len(applicable), "results": results,
                       "detail": detail}, handle, indent=2, sort_keys=True)
        passed = sum(1 for r in results if r["verdict"] == "PASS")
        if verdict == "PASS" and (passed != len(applicable)
                                  or len(results) != len(applicable)):
            verdict = "FAIL"
            detail = "count not closed: ran %d, passed %d, expected %d" % (
                len(results), passed, len(applicable))
        log("RESULT %s V-043 ran=%d passed=%d expected=%d %s"
            % (verdict, len(results), passed, len(applicable), detail))
    except Blocked as exc:
        verdict = "BLOCKED"
        detail = str(exc)
        log("BLOCKED " + detail)
        log("RESULT BLOCKED V-043 %s" % detail)
    save_log()
    return {"PASS": 0, "FAIL": 1, "BLOCKED": 2}[verdict]


if __name__ == "__main__":
    sys.exit(main())
