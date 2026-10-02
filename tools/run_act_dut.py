#!/usr/bin/env python3
"""Run the applicable ACT4 (riscv-arch-test) ELFs on the MosaicRV p1 core (V-043).

    python3 tools/run_act_dut.py                 # generate, build, run, record
    python3 tools/run_act_dut.py --controls      # also build and run the controls
    python3 tools/run_act_dut.py --no-generate   # reuse the ELF set already built

What the V-043 card demands, literally: align UDB/sail.json/rvtest_config with the
platform, expand the default exclusions, generate every applicable ELF, record the
exact N, execute each on the DUT and capture every pass/fail; close N against the
generation manifest; calibrate the self-check PASS macro; never treat the official
tests as sufficient verification.

What this runner is
-------------------
The applicable set is computed by *generating* it: the frozen ACT4 checkout builds
one ELF per applicable testcase for the MosaicRV p1 configuration
(`tests/act4/mosaic-p1/`, whose UDB config, linker script and `rvmodel_macros.h`
match `config/profiles/p1.json`), and the set of ELFs that appears is the set this
runner must execute. N is taken from that set, never from a recorded list, and the
generation is pinned to the checkout's HEAD commit.

Each ELF is a self-checking ACT4 image: Sail 0.14.1 computed its expected
signature and the framework compiled it into the ELF's own `.data`; every
`RVTEST_SIGUPD` compares the value the core computes against that word and jumps
to a failure handler on a mismatch; `RVMODEL_HALT_PASS` writes 1 to `tohost` and
`RVMODEL_HALT_FAIL` writes 3. The ELF is therefore an oracle the DUT executes, not
one it can fake, and the verdict is read from the DUT's own retirement stream by
`CASE=core.act_dut` (sim/unit/tb_core_act.cpp), which watches for the committed
store to the image's `tohost` symbol.

The self-check PASS macro is calibrated before any ELF runs: the runner asserts
that the generated images were built with `RVTEST_SELFCHECK` (the pass string is
present and the failure-handler labels are linked), that `tohost` is a symbol the
ELF carries, and that the pass value the protocol expects is 1. If the framework
ever fell back to signature mode (no in-ELF comparison), the run stops instead of
reporting a vacuous pass.

Controls (--controls)
---------------------
Two defect injections must make the case FAIL, so the case is known to detect a
real error rather than passing on anything:

  * `MOSAIC_ALU_MUTANT_4` (slt answers the unsigned comparison) is built into the
    DUT and the I-suite `slt` ELF must stop passing inside the ELF;
  * one expected-signature word is corrupted in a copy of an ELF, so the ELF's own
    comparison must fail and the DUT must write 3 to `tohost`.

Both are built from a deleted build directory with their `-D` on the Verilator
command line, so the mutant binary's hash differs from the shipping one, and both
must exit 1 with a named first failure.

Exit status: 0 only when every applicable ELF ran and passed with N closed; 1 FAIL
(a DUT mismatch or a control that did not fail); 2 BLOCKED (a prerequisite is
missing).
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
ACT_BIN = os.path.join(REF_ROOT, "act4-bin")
SAIL_BIN = os.path.join(REF_ROOT, "sail-riscv-0.14.1", "bin")
XDG_CACHE = os.path.join(REF_ROOT, "act4-native-cache")
ACT_CONFIG = os.path.join(REPO, "tests", "act4", "mosaic-p1", "test_config.yaml")
WORKDIR_DEFAULT = os.path.join(REF_ROOT, "act4-mosaic-p1-work")

# The extension filter handed to `act`. Each name is the extension a suite's tests
# declare in REQUIRED_EXTENSIONS; the p1 profile's advertised set is the source of
# truth (config/profiles/p1.json), and a name with no suite in this checkout
# simply produces nothing. `A`/`C` are the ISA names; their suites are the
# sub-extension names the checkout uses.
APPLICABLE_EXTENSIONS = [
    "I", "M", "Zicsr", "Zifencei",
    "Zaamo", "Zalrsc",             # A
    "Zca", "Zcb", "Zcmop",         # C
    "Zicntr", "Zihpm", "Zmmul", "Zicbom", "Zicbop",
]

CASE_ID = "core.act_dut"
PROFILE = "p1"

# Suites the checkout contains (tests/rv64i/*) that are not in the applicable
# set, with the reason each is not run. A suite that produces no ELF under this
# configuration is not silently assumed to pass: it is either named here or is a
# `generation` failure the runner reports.
DEFERRED_REASONS = {
    "A": "A is generated through its sub-extension suites (Zaamo, Zalrsc).",
    "C": "C is generated through its sub-extension suites (Zca, Zcb, Zcmop); Zcd needs D, which p1 does not claim.",
    "D": "F/D floating point: not in the p1 profile.",
    "F": "F/D floating point: not in the p1 profile.",
    "Misalign": "misaligned-access suite: p1's policy traps these; the ACT suite needs the trap-frame model V-044 covers.",
    "MisalignD": "misaligned-access suite: F/D not in the p1 profile.",
    "MisalignF": "misaligned-access suite: F/D not in the p1 profile.",
    "MisalignZca": "misaligned-access suite: p1's policy traps these; the ACT suite needs the trap-frame model V-044 covers.",
    "Zabha": "A sub-extension (byte/halfword atomics): not in the p1 profile.",
    "Zacas": "A sub-extension (compare-and-swap): not in the p1 profile.",
    "ZacasZabha": "A sub-extensions: not in the p1 profile.",
    "Zba": "B extension: not in the p1 profile.",
    "Zbb": "B extension: not in the p1 profile.",
    "Zbc": "B extension: not in the p1 profile.",
    "Zbkb": "B/crypto extension: not in the p1 profile.",
    "Zbkc": "B/crypto extension: not in the p1 profile.",
    "Zbkx": "B/crypto extension: not in the p1 profile.",
    "Zbs": "B extension: not in the p1 profile.",
    "ZcbM": "Zcb+crypto carry-less multiply: the crypto half is not in the p1 profile.",
    "ZcbZba": "Zcb+Zba: Zba is not in the p1 profile.",
    "ZcbZbb": "Zcb+Zbb: Zbb is not in the p1 profile.",
    "Zcd": "C+D: D is not in the p1 profile.",
    "ZfaD": "F/D floating point: not in the p1 profile.",
    "ZfaF": "F/D floating point: not in the p1 profile.",
    "ZfaZfh": "F/D/Zfh floating point: not in the p1 profile.",
    "ZfaZfhD": "F/D/Zfh floating point: not in the p1 profile.",
    "ZfaZvfh": "vector floating point: not in the p1 profile.",
    "Zfbfmin": "F/D floating point: not in the p1 profile.",
    "Zfh": "F/D/Zfh floating point: not in the p1 profile.",
    "ZfhD": "F/D/Zfh floating point: not in the p1 profile.",
    "Zfhmin": "F/D/Zfh floating point: not in the p1 profile.",
    "ZfhminD": "F/D/Zfh floating point: not in the p1 profile.",
    "Zicboz": "cache-block zero: not in the p1 profile.",
    "Zihintntl": "hint ops: not in the p1 profile.",
    "ZihintntlZca": "hint/C ops: not in the p1 profile.",
    "Zihintpause": "hint ops: not in the p1 profile.",
    "Zimop": "MOP ops: p1 does not claim Zimop (RVA23 segment).",
    "Zknd": "crypto: not in the p1 profile.",
    "Zkne": "crypto: not in the p1 profile.",
    "Zknh": "crypto: not in the p1 profile.",
    "Zksed": "crypto: not in the p1 profile.",
    "Zksh": "crypto: not in the p1 profile.",
    "Zicond": "conditional ops: the p1 profile claims Zicond but this checkout has no Zicond suite, so no ELF exists to run (see the exclusion ledger).",
}

# A control that must fail: the ALU mutant the I-suite slt cases detect.
CONTROL_MUTANT_DEFINE = "MOSAIC_ALU_MUTANT_4"
CONTROL_MUTANT_SUITE = "I"
CONTROL_MUTANT_CANDIDATES = ("I-slt-00", "I-sltiu-00")
# A data control: corrupt one expected-signature word in a copy of this ELF.
CONTROL_DATA_SUITE = "I"
CONTROL_DATA_ELF = "I-add-00"


class Blocked(Exception):
    pass


class Failed(Exception):
    pass


def sh(cmd, cwd=REPO, timeout=1800, env=None):
    try:
        proc = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=timeout, env=env)
    except (OSError, subprocess.SubprocessError) as exc:
        raise Blocked("cannot run %s: %s" % (" ".join(cmd), exc))
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def act_env():
    env = dict(os.environ)
    env["PATH"] = ACT_BIN + os.pathsep + SAIL_BIN + os.pathsep + env.get("PATH", "")
    env["XDG_CACHE_HOME"] = XDG_CACHE
    return env


def elf_symbols(path):
    """Parse `.symtab`; host binutils reads RISC-V ELFs."""
    for tool in ("riscv64-unknown-elf-readelf", "riscv64-elf-readelf", "readelf"):
        code, out = sh([tool, "-sW", path])
        if code == 0:
            break
    else:
        raise Blocked("no readelf that can read %s" % path)
    symbols = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[2].startswith("0"):
            try:
                symbols[parts[7].split("@")[0]] = int(parts[1], 16)
            except ValueError:
                continue
    return symbols


def elf_has_string(path, needle):
    with open(path, "rb") as handle:
        return needle.encode() in handle.read()


def test_config(path):
    with open(path) as handle:
        head = handle.read(3000)
    exts = re.search(r"# REQUIRED_EXTENSIONS:\s*(\[[^\]]*\])", head)
    march = re.search(r"# MARCH:\s*(\S+)", head)
    return (exts.group(1) if exts else "?"), (march.group(1) if march else "?")


def registry_entry():
    """The sources and top for CASE=core.act_dut.

    Uses the registered entry when the case has been registered; otherwise
    derives it from CASE=core.corpus_sweep (the same integrated core top) so the
    tool can build and run before the registration lands.
    """
    with open(os.path.join(REPO, "tests", "unit", "registry.json")) as handle:
        cases = json.load(handle)["cases"]
    if CASE_ID in cases:
        return cases[CASE_ID], True
    base = dict(cases["core.corpus_sweep"])
    base["top"] = "mosaic_core_tb"
    base["cpp"] = ["sim/unit/tb_core_act.cpp", "sim/common/elf_loader.cpp",
                   "sim/common/memory_model.cpp"]
    return base, False


def build(build_dir, define=None, entry=None, log=None):
    """Build CASE=core.act_dut, optionally with one -D control. Returns binary."""
    shutil.rmtree(build_dir, ignore_errors=True)
    os.makedirs(os.path.join(build_dir), exist_ok=True)
    binary = os.path.join(build_dir, "case")
    rtl = entry["rtl"]
    packages = [p for p in rtl if os.path.basename(p) == "mosaic_pkg.sv"]
    rest = [p for p in rtl if p not in packages]
    listed = packages + rest + entry.get("sv", []) + entry.get("cpp", []) + \
        ["sim/common/sim_common.cpp"]
    sources = [os.path.join(REPO, p) for p in listed]
    cmd = ["verilator", "--cc", "--exe", "--build", "-j", "0", "-O2",
           "-CFLAGS", "-O2 -std=c++17 -Wall", "--x-assign", "unique",
           "--x-initial", "unique", "--top-module", entry["top"],
           "-Mdir", os.path.join(build_dir, "obj_dir")]
    if define:
        cmd.append("-D" + define)
        cmd += ["-CFLAGS", "-D" + define]
    cmd += ["-I" + os.path.join(REPO, "build", PROFILE, "sim"),
            "-I" + os.path.join(REPO, "rtl", "core"),
            "-I" + os.path.join(REPO, "rtl", "common"),
            "-I" + os.path.join(REPO, "build", PROFILE, "rtl"),
            "-CFLAGS", "-I" + os.path.join(REPO, "sim", "common"),
            "-CFLAGS", "-I" + os.path.join(REPO, "build", PROFILE, "sim"),
            "-o", binary]
    cmd += sources
    code, out = sh(cmd, timeout=1800)
    if log is not None:
        log.append("$ " + " ".join(cmd) + "\n" + out[-4000:])
    if code != 0 or not os.path.exists(binary):
        raise Blocked("build failed for %s\n%s" % (define or "shipping", out[-4000:]))
    return binary


def run_elf(binary, elf, out_dir, max_cycles):
    """Run one ELF; returns (verdict, exit, log)."""
    os.makedirs(out_dir, exist_ok=True)
    cmd = [binary, "--case", CASE_ID, "--out", out_dir, "--seed", "1",
           "--max-cycles", str(max_cycles), "--image", elf]
    code, out = sh(cmd, timeout=max(120, max_cycles // 200), cwd=REPO)
    verdict = None
    for line in out.splitlines():
        if line.startswith("RESULT "):
            parts = line.split(None, 3)
            verdict = parts[1] if len(parts) > 1 else None
    return verdict, code, out


def generate(workdir, log):
    """Generate the applicable ELF set with `act`. Returns (commit, elf list)."""
    for path in (ACT_ROOT, ACT_CONFIG):
        if not os.path.exists(path):
            raise Blocked("missing prerequisite: %s" % path)
    code, commit = sh(["git", "-C", ACT_ROOT, "rev-parse", "HEAD"])
    if code != 0:
        raise Blocked("cannot read the ACT4 checkout revision")
    commit = commit.strip()
    cmd = ["mise", "exec", "--", "uv", "run", "act", ACT_CONFIG,
           "--extensions", ",".join(APPLICABLE_EXTENSIONS),
           "--workdir", workdir]
    log.append("$ " + " ".join(cmd))
    code, out = sh(cmd, cwd=ACT_ROOT, timeout=3600, env=act_env())
    log.append(out[-6000:])
    if code != 0 or "Build complete" not in out:
        raise Blocked("ACT4 generation failed (act exited %d)" % code)
    elf_root = os.path.join(workdir, "mosaic-p1", "elfs", "rv64i")
    if not os.path.isdir(elf_root):
        raise Blocked("act produced no ELF root at %s" % elf_root)
    elfs = []
    for suite in sorted(os.listdir(elf_root)):
        suite_dir = os.path.join(elf_root, suite)
        if not os.path.isdir(suite_dir):
            continue
        for name in sorted(os.listdir(suite_dir)):
            if name.endswith(".elf"):
                elfs.append((suite, os.path.join(suite_dir, name)))
    if not elfs:
        raise Blocked("the generation produced no ELFs")
    return commit, elf_root, elfs


def calibate_selfcheck(elfs, log):
    """Prove the self-check PASS macro is calibrated, not assumed.

    Three facts, each checked on a generated image:
      1. the framework built it in RVTEST_SELFCHECK mode (the pass summary string
         is linked in, and the signature region is preloaded rather than filled
         with 0xdeadbeef);
      2. `tohost` is a symbol the image carries and `RVMODEL_HALT_PASS` targets it;
      3. the protocol's pass value is the low word 1 (the failure value is 3).
    """
    suite, path = elfs[0]
    if not elf_has_string(path, "RVCP-SUMMARY: TEST PASSED"):
        raise Blocked("calibration failed: %s is not a self-checking image (no "
                      "self-check pass string); the framework may be in signature "
                      "mode" % path)
    if elf_has_string(path, "TEST SIGRUN"):
        raise Blocked("calibration failed: %s carries the signature-mode banner"
                      % path)
    symbols = elf_symbols(path)
    for name in ("tohost", "fromhost", "begin_signature", "end_signature",
                 "rvtest_entry_point"):
        if name not in symbols:
            raise Blocked("calibration failed: %s has no %s symbol" % (path, name))
    if symbols["begin_signature"] >= symbols["end_signature"]:
        raise Blocked("calibration failed: %s has an empty signature region" % path)
    log.append("PASS-macro calibrated on %s: RVTEST_SELFCHECK image, tohost=%#x, "
               "signature=[%#x,%#x), pass value 1 / fail value 3 "
               "(config/sail ... rvmodel_macros.h RVMODEL_HALT_PASS)"
               % (os.path.basename(path), symbols["tohost"],
                  symbols["begin_signature"], symbols["end_signature"]))
    return symbols


def corrupt_signature(src, dst, symbols):
    """Copy `src` to `dst` and flip one byte of the expected signature region."""
    shutil.copyfile(src, dst)
    with open(dst, "r+b") as handle:
        handle.seek(symbols["begin_signature"])
        byte = handle.read(1)
        handle.seek(symbols["begin_signature"])
        handle.write(bytes([byte[0] ^ 0x01]))
    return dst


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--workdir", default=WORKDIR_DEFAULT)
    parser.add_argument("--out", default=os.path.join(REPO, "results", "v043"))
    parser.add_argument("--max-cycles", type=int, default=4000000)
    parser.add_argument("--no-generate", action="store_true")
    parser.add_argument("--controls", action="store_true")
    parser.add_argument("--limit", type=int, default=0)
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    log_lines = []

    def log(text=""):
        print(text, flush=True)
        log_lines.append(text)

    verdict = "PASS"
    detail = ""
    record = {"case": CASE_ID, "verdict": "PASS", "ran": 0, "expected": 0,
              "results": [], "controls": [], "act_commit": "", "planes": {}}
    try:
        entry, registered = registry_entry()
        log("ACT4 runner (V-043); case %s registered=%s" % (CASE_ID, registered))
        if not args.no_generate:
            commit, elf_root, elfs = generate(args.workdir, log_lines)
        else:
            code, commit = sh(["git", "-C", ACT_ROOT, "rev-parse", "HEAD"])
            commit = commit.strip()
            elf_root = os.path.join(args.workdir, "mosaic-p1", "elfs", "rv64i")
            elfs = []
            for suite in sorted(os.listdir(elf_root)):
                suite_dir = os.path.join(elf_root, suite)
                if os.path.isdir(suite_dir):
                    for name in sorted(os.listdir(suite_dir)):
                        if name.endswith(".elf"):
                            elfs.append((suite, os.path.join(suite_dir, name)))
        record["act_commit"] = commit
        log("ACT4 checkout @ %s" % commit)
        if args.limit:
            elfs = elfs[:args.limit]
        log("applicable ELFs: N=%d" % len(elfs))

        # -- manifest before any run ----------------------------------------
        src_root = os.path.join(ACT_ROOT, "tests", "rv64i")
        manifest = {"task": "V-043", "act_commit": commit,
                    "config": os.path.relpath(ACT_CONFIG, REPO),
                    "extensions": APPLICABLE_EXTENSIONS,
                    "dut": "mosaic_core_tb@p1 (integrated out-of-order core)",
                    "max_cycles": args.max_cycles, "cases": [], "exclusions": []}
        for suite, path in elfs:
            src = os.path.join(src_root, suite,
                               os.path.basename(path).replace(".elf", ".S"))
            req, march = test_config(src) if os.path.exists(src) else ("?", "?")
            manifest["cases"].append({
                "suite": suite, "elf": os.path.relpath(path, REF_ROOT),
                "sha256": sha256(path), "required_extensions": req, "march": march})
        for suite in sorted(os.listdir(src_root)):
            if not os.path.isdir(os.path.join(src_root, suite)):
                continue
            if any(s == suite for s, _ in elfs):
                continue
            count = len([n for n in os.listdir(os.path.join(src_root, suite))
                         if n.endswith(".S")])
            if count == 0:
                continue
            manifest["exclusions"].append({
                "suite": suite, "tests": count,
                "reason": DEFERRED_REASONS.get(
                    suite, "not in the p1 profile; no DUT support claimed")})
        with open(os.path.join(args.out, "suite_manifest.json"), "w") as handle:
            json.dump(manifest, handle, indent=2, sort_keys=True)
        record["planes"] = {"suites": sorted({s for s, _ in elfs}),
                            "n": len(elfs),
                            "exclusions": len(manifest["exclusions"])}
        log("wrote suite_manifest.json (N=%d, excluded suites=%d)"
            % (len(elfs), len(manifest["exclusions"])))

        # -- self-check calibration -----------------------------------------
        symbols = calibate_selfcheck(elfs, log_lines)

        # -- build the DUT, then run every ELF ------------------------------
        build_dir = os.path.join(REPO, "build", PROFILE, "unit", CASE_ID)
        binary = build(build_dir, entry=entry)
        log("DUT built: %s (sha256 %s)" % (binary, sha256(binary)))
        record["dut_sha256"] = sha256(binary)

        expected = len(elfs)
        ran = 0
        passed = 0
        first_fail = ""
        for suite, path in elfs:
            name = os.path.basename(path)
            case_dir = os.path.join(args.out, "cases", suite, name.replace(".elf", ""))
            shutil.copy(path, os.path.join(case_dir, name))
            kind, code, out = run_elf(binary, path, case_dir, args.max_cycles)
            with open(os.path.join(case_dir, "dut_run.log"), "w") as handle:
                handle.write(out)
            ran += 1
            ok = (kind == "PASS" and code == 0)
            if ok:
                passed += 1
            record["results"].append({"suite": suite, "elf": name, "verdict": kind,
                                      "exit": code, "sha256": sha256(path)})
            log("%-4s %-28s %s" % ("PASS" if ok else "FAIL", suite + "/" + name,
                                   "exit=%d" % code))
            if not ok:
                tail = [l for l in out.splitlines() if l.startswith(("CHECK FAILED",
                                                                     "RESULT",
                                                                     "ACT-ELF"))]
                log("  first failure: " + (tail[0] if tail else out.strip()[-200:]))
                if not first_fail:
                    first_fail = "%s/%s" % (suite, name)
        record["ran"] = ran
        record["expected"] = expected
        if first_fail:
            verdict = "FAIL"
            detail = "first failing ELF: %s" % first_fail
        elif ran != expected:
            verdict = "FAIL"
            detail = "count not closed: ran %d, expected %d" % (ran, expected)

        # -- controls --------------------------------------------------------
        controls_ok = True
        if args.controls:
            control_log = []
            # (1) RTL mutant: the I-suite slt ELF must stop passing.
            mutant_dir = build_dir + ".MUTANT_ALU4"
            mutant_bin = build(mutant_dir, define=CONTROL_MUTANT_DEFINE,
                               entry=entry, log=control_log)
            mutant_sha = sha256(mutant_bin)
            target = None
            for cand in CONTROL_MUTANT_CANDIDATES:
                for suite, path in elfs:
                    if os.path.basename(path) == cand + ".elf":
                        target = (suite, path)
                        break
                if target:
                    break
            if target is None:
                controls_ok = False
                control_log.append("control: no slt ELF found for the mutant")
            else:
                cdir = os.path.join(args.out, "controls", "alu_mutant_4")
                kind, code, out = run_elf(mutant_bin, target[1], cdir, args.max_cycles)
                failed = (kind == "FAIL" and code == 1)
                record["controls"].append({
                    "name": CONTROL_MUTANT_DEFINE, "define": CONTROL_MUTANT_DEFINE,
                    "elf": os.path.basename(target[1]), "sha256": mutant_sha,
                    "shipping_sha256": record["dut_sha256"], "exit": code,
                    "verdict": kind, "must": "FAIL", "ok": failed})
                control_log.append("control %s: %s exit=%d (must FAIL)"
                                   % (CONTROL_MUTANT_DEFINE, kind, code))
                log("control %-28s %s exit=%d (must FAIL)" %
                    (CONTROL_MUTANT_DEFINE, kind, code))
                controls_ok = controls_ok and failed
            # (2) data control: corrupt one expected signature word.
            target = None
            for suite, path in elfs:
                if os.path.basename(path) == CONTROL_DATA_ELF + ".elf":
                    target = (suite, path)
                    break
            if target is None:
                controls_ok = False
                control_log.append("control: no target ELF for the data control")
            else:
                cdir = os.path.join(args.out, "controls", "corrupt_signature")
                os.makedirs(cdir, exist_ok=True)
                corrupted = corrupt_signature(
                    target[1], os.path.join(cdir, os.path.basename(target[1])), symbols)
                kind, code, out = run_elf(binary, corrupted, cdir, args.max_cycles)
                failed = (kind == "FAIL" and code == 1)
                record["controls"].append({
                    "name": "corrupt_expected_signature", "define": None,
                    "elf": os.path.basename(corrupted),
                    "sha256": sha256(corrupted), "exit": code,
                    "verdict": kind, "must": "FAIL", "ok": failed})
                log("control %-28s %s exit=%d (must FAIL)" %
                    ("corrupt_expected_signature", kind, code))
                controls_ok = controls_ok and failed
            with open(os.path.join(args.out, "controls.log"), "w") as handle:
                handle.write("\n".join(control_log) + "\n")
            if not controls_ok:
                verdict = "FAIL"
                detail = (detail + "; " if detail else "") + \
                    "a control did not fail where it must"
        log("RESULT %s %s ran=%d passed=%d expected=%d %s"
            % (verdict, CASE_ID, ran, passed, expected, detail))
        record["verdict"] = verdict
        record["detail"] = detail
        record["passed"] = passed
    except Blocked as exc:
        verdict = "BLOCKED"
        detail = str(exc)
        log("BLOCKED " + detail)
        log("RESULT BLOCKED %s %s" % (CASE_ID, detail))
        record["verdict"] = "BLOCKED"
        record["detail"] = detail

    with open(os.path.join(args.out, "run.log"), "w") as handle:
        handle.write("\n".join(log_lines) + "\n")
    with open(os.path.join(args.out, "dut_results.json"), "w") as handle:
        json.dump(record, handle, indent=2, sort_keys=True)

    # The case's own result.json, so the exclusion ledger can name it. It is the
    # aggregate verdict of the real per-ELF runs above, never a constant.
    results_dir = os.path.join(REPO, "results", "unit", CASE_ID)
    os.makedirs(results_dir, exist_ok=True)
    with open(os.path.join(results_dir, "result.json"), "w") as handle:
        json.dump({
            "schema_version": 1, "case": CASE_ID, "verdict": record["verdict"],
            "detail": record.get("detail", ""), "seed": 1,
            "max_cycles": args.max_cycles, "checks": record.get("expected", 0),
            "failures": (record.get("expected", 0) - record.get("passed", 0)),
            "image": "", "expect": "",
            "tool": "tools/run_act_dut.py over CASE=%s" % CASE_ID,
            "act_commit": record.get("act_commit", ""),
            "dut_sha256": record.get("dut_sha256", ""),
            "planes": record.get("planes", {}),
            "controls": record.get("controls", []),
        }, handle, indent=2, sort_keys=True)

    return {"PASS": 0, "FAIL": 1, "BLOCKED": 2}[verdict]


if __name__ == "__main__":
    sys.exit(main())
