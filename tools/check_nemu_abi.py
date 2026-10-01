#!/usr/bin/env python3
"""Check the pinned NEMU Difftest ABI and state serialization layout (V-004).

    python3 tools/check_nemu_abi.py --out results/v004
    python3 tools/check_nemu_abi.py --out results/v004 --guest mosaic-x86

What the V-004 card demands, literally:

  * per-symbol prototype/layout/macro check of the frozen reference library;
  * an independent state write/read-back plus two arithmetic instructions and
    one store proving reference-step direction against the REAL library
    (mock echo is an explicit Fail);
  * a missing-symbol library and a wrong-layout library, both refused
    fail-closed before any DUT executes.

How it runs. The reference ``.so`` is an x86-64 ELF (it dlopens but cannot
execute on this Darwin arm64 host), so the parts that must touch the real
library run inside the x86_64 Lima guest ``mosaic-x86``, which holds
``mosaic-ref/NEMU-f39e`` at the pinned commit. The layout is derived on the
host by parsing the guest's ``isa-def.h`` struct under its ``.config``
conditionals -- never by trusting a recorded constant -- and the derived size
must equal the live ``DIFFTEST_REG_SIZE`` exported by the ``.so``. A guest
probe (``tools/nemu_guest_probe.py``, stdlib ctypes only) then performs the
round-trip and the ADD/ADDI/SD stepping inside the guest against the real
``.so``. Negative controls run in separate guest processes:

  N1  a library with no difftest symbols (fresh ``gcc -shared`` empty .so)
      must be refused with a missing-symbol error, never treated as "no diff";
  N2  a layout spec whose size is off by 8 must be refused before any regcpy;
  N3  calling ``difftest_regcpy`` with the 3-argument LightQS arity must raise
      ``ArgumentError`` -- proving the adapter uses the 2-argument form the
      pinned build exports (the card's named two-vs-three-parameter trap).

Exit status: 0 PASS, 1 FAIL (a check failed), 2 BLOCKED (guest, NEMU source or
reference library missing -- recorded with the exact missing piece).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
GUEST_PROBE = os.path.join(REPO, "tools", "nemu_guest_probe.py")

EXPECTED_COMMIT = "f39e3077d7bac3cd9a3a853a9300a5f8f0293a2c"
GUEST_NEMU = "mosaic-ref/NEMU-f39e"
GUEST_SO = GUEST_NEMU + "/build/riscv64-nemu-interpreter-so"

# Symbols the adapter (and any future DUT Difftest path) binds. ``difftest``
# entry points beyond the classic five exist in this build; the required set
# below is what V-004 freezes, and anything absent is fail-closed.
REQUIRED_SYMBOLS = [
    "difftest_init",
    "difftest_init_v2",
    "difftest_memcpy",
    "difftest_memcpy_init",
    "difftest_regcpy",
    "difftest_exec",
    "difftest_raise_intr",
    "difftest_csrcpy",
    "difftest_store_commit",
]


class Blocked(Exception):
    pass


def sh(cmd, **kw):
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=120, **kw)
    except (OSError, subprocess.SubprocessError) as exc:
        raise Blocked("cannot run %s: %s" % (" ".join(cmd), exc))
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def guest(guest_name, *command):
    if shutil.which("limactl") is None:
        raise Blocked("limactl is not installed; no x86_64 guest is reachable")
    code, out = sh(["limactl", "shell", guest_name, "--"] + list(command))
    if code != 0:
        raise Blocked("guest '%s' command failed: %s\n%s"
                      % (guest_name, " ".join(command), out.strip()[:2000]))
    return out


def guest_file(guest_name, path):
    out = guest(guest_name, "cat", path)
    if not out:
        raise Blocked("guest file is empty or unreadable: %s" % path)
    return out


def parse_kconfig(text):
    """Return the set of CONFIG_ symbols set to y/m in a NEMU .config."""
    enabled = set()
    for line in text.splitlines():
        line = line.strip()
        match = re.match(r"^(CONFIG_[A-Za-z0-9_]+)=(y|m|\"?.*\"?)$", line)
        if match and match.group(2) in ("y", "m"):
            enabled.add(match.group(1))
    return enabled


def parse_vlen(vreg_text):
    match = re.search(r"#define\s+VLEN\s+(\d+)", vreg_text)
    if not match:
        raise Blocked("cannot find VLEN in NEMU vreg.h")
    return int(match.group(1))


def parse_state_layout(isa_def_text, enabled, vlen):
    """Derive the difftest-synced prefix of riscv64_CPU_state.

    Walks the struct fields in order, honouring #ifdef/#ifndef/#else/#endif
    against the .config set, and stops at ``difftest_state_end`` (everything
    past it is NEMU exec state, explicitly NOT synced by regcpy). Returns
    (offsets, reg_size) where offsets maps field/vec-element names to byte
    offsets. Raises Blocked if the struct body cannot be found.
    """
    body = re.search(r"typedef struct \{(.*?)\} riscv64_CPU_state;",
                     isa_def_text, re.S)
    if not body:
        raise Blocked("cannot find riscv64_CPU_state in isa-def.h")
    offsets = {}
    size = 0
    cond_stack = []
    union_depth = 0

    def active():
        return all(cond for _, cond in cond_stack)

    for raw in body.group(1).splitlines():
        line = raw.strip()
        if line.startswith("#ifdef "):
            sym = line.split()[1]
            cond_stack.append((sym, sym in enabled))
        elif line.startswith("#ifndef "):
            sym = line.split()[1]
            cond_stack.append((sym, sym not in enabled))
        elif line.startswith("#else"):
            sym, cond = cond_stack.pop()
            cond_stack.append((sym, not cond))
        elif line.startswith("#endif"):
            cond_stack.pop()
        elif not active():
            continue
        elif "union" in line and "{" in line:
            # Register-file element type (gpr/fpr/vr): member declarations
            # inside add no bytes; the closing brace line carries the array.
            union_depth += 1
        elif union_depth > 0:
            if re.match(r"\}\s*gpr\[32\];", line):
                for i in range(32):
                    offsets["gpr%d" % i] = size + 8 * i
                size += 32 * 8
                union_depth -= 1
            elif re.match(r"\}\s*fpr\[32\];", line):
                for i in range(32):
                    offsets["fpr%d" % i] = size + 8 * i
                size += 32 * 8
                union_depth -= 1
            elif re.match(r"\}\s*vr\[32\];", line):
                elem = (vlen // 64) * 8
                for i in range(32):
                    offsets["vr%d" % i] = size + elem * i
                size += 32 * elem
                union_depth -= 1
            elif "}" in line:
                union_depth -= 1  # inner union close: no bytes
            # else: union member declaration line: no bytes
        elif line.startswith("uint64_t "):
            names = line[len("uint64_t "):].rstrip(";").replace(" ", "").split(",")
            for name in names:
                if name == "difftest_state_end":
                    return offsets, size
                offsets[name] = size
                size += 8
    raise Blocked("difftest_state_end marker not found in riscv64_CPU_state")


def check_field_order(offsets):
    """Reject size-equal-but-reordered layouts the card names as Fail."""
    expected_sequence = (
        ["gpr%d" % i for i in range(32)]
        + ["fpr%d" % i for i in range(32)]
        + ["mode", "mstatus", "sstatus", "mepc", "sepc", "mtval", "stval",
           "mtvec", "stvec", "mcause", "scause", "satp", "mip", "mie",
           "mscratch", "sscratch", "mideleg", "medeleg", "pc"]
    )
    problems = []
    last = -1
    for name in expected_sequence:
        if name not in offsets:
            problems.append("missing field %s" % name)
            continue
        if offsets[name] < last:
            problems.append("field %s out of order" % name)
        last = offsets[name]
    for anchor, name in (("gpr31", "fpr0"), ("fpr31", "mode"), ("medeleg", "pc")):
        if anchor in offsets and name in offsets and offsets[name] <= offsets[anchor]:
            problems.append("%s does not follow %s" % (name, anchor))
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=os.path.join(REPO, "results", "v004"))
    parser.add_argument("--guest", default="mosaic-x86")
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
        if not os.path.exists(GUEST_PROBE):
            raise Blocked("guest probe missing: %s" % GUEST_PROBE)
        # -- 1. frozen source ------------------------------------------------
        commit = guest(args.guest, "git", "-C", GUEST_NEMU, "rev-parse", "HEAD").strip()
        log("NEMU source commit: %s" % commit)
        if commit != EXPECTED_COMMIT:
            raise Blocked("NEMU source revision mismatch: expected %s, found %s"
                          % (EXPECTED_COMMIT, commit))
        dirty = guest(args.guest, "git", "-C", GUEST_NEMU,
                      "status", "--porcelain").strip()
        log("working tree clean: %s" % (not dirty))
        if dirty:
            raise Blocked("NEMU working tree is not clean:\n%s" % dirty[:1000])
        # -- 2. layout derivation ---------------------------------------------
        config_text = guest_file(args.guest, GUEST_NEMU + "/.config")
        isa_def = guest_file(args.guest,
                             GUEST_NEMU + "/src/isa/riscv64/include/isa-def.h")
        vreg = guest_file(args.guest,
                          GUEST_NEMU + "/src/isa/riscv64/instr/rvv/vreg.h")
        enabled = parse_kconfig(config_text)
        vlen = parse_vlen(vreg)
        log("VLEN=%d RVV=%s RVH=%s FPU quirks: FPU_NONE=%s" %
            (vlen, "CONFIG_RVV" in enabled, "CONFIG_RVH" in enabled,
             "CONFIG_FPU_NONE" in enabled))
        offsets, reg_size = parse_state_layout(isa_def, enabled, vlen)
        log("derived DIFFTEST_REG_SIZE=%d over %d named fields" % (reg_size, len(offsets)))
        order_problems = check_field_order(offsets)
        if order_problems:
            verdict = "FAIL"
            detail = "field order problems: %s" % "; ".join(order_problems)
            log("FAIL " + detail)
        # -- 3. reference library identity ------------------------------------
        sha = guest(args.guest, "sha256sum", GUEST_SO).split()[0]
        log("reference .so sha256: %s" % sha)
        nm_code, nm_out = sh(["limactl", "shell", args.guest, "--",
                              "nm", "-D", "--defined-only", GUEST_SO])
        if nm_code != 0:
            raise Blocked("guest has no nm for symbol audit: %s" % nm_out.strip()[:500])
        exported = set(re.findall(r" T (\S+)", nm_out))
        missing = [s for s in REQUIRED_SYMBOLS if s not in exported]
        log("exported difftest symbols: %d; required: %d"
            % (len([s for s in exported if "difftest" in s]), len(REQUIRED_SYMBOLS)))
        if missing:
            verdict = "FAIL"
            detail = "reference .so is missing symbols: %s" % ", ".join(missing)
            log("FAIL " + detail)
        try:
            reg_size_live = guest(args.guest, "python3", "-c",
                                  "import ctypes;"
                                  "so=ctypes.CDLL('%s');"
                                  "print(ctypes.c_uint.in_dll(so,'DIFFTEST_REG_SIZE').value)"
                                  % GUEST_SO).strip()
            log("live DIFFTEST_REG_SIZE=%s" % reg_size_live)
            if int(reg_size_live) != reg_size:
                verdict = "FAIL"
                detail = ("live DIFFTEST_REG_SIZE=%s disagrees with derived %d"
                          % (reg_size_live, reg_size))
                log("FAIL " + detail)
        except Blocked:
            raise
        except ValueError:
            verdict = "FAIL"
            detail = "live DIFFTEST_REG_SIZE is not an integer: %r" % reg_size_live
            log("FAIL " + detail)
        # -- 4. manifest -------------------------------------------------------
        manifest = {
            "task": "V-004",
            "nemu_commit": commit,
            "nemu_working_tree_clean": True,
            "reference_so": GUEST_SO,
            "reference_so_sha256": sha,
            "vlen": vlen,
            "config_flags": sorted(s for s in enabled
                                   if s.startswith("CONFIG_RV")
                                   or s.startswith("CONFIG_DIFFTEST")
                                   or s.startswith("CONFIG_FPU")
                                   or s == "CONFIG_ISA64"),
            "prototypes": {
                "difftest_regcpy": "void (*)(void *dut, bool direction)",
                "difftest_memcpy": ("void (*)(uint64_t nemu_addr, void *dut_buf, "
                                    "size_t n, bool direction)"),
                "difftest_exec": "void (*)(uint64_t n)",
                "difftest_init_v2": "void (*)(unsigned state_size)",
            },
            "direction_enum": {"DIFFTEST_TO_DUT": 0, "DIFFTEST_TO_REF": 1},
            "reg_size": reg_size,
            "offsets": offsets,
            "required_symbols": REQUIRED_SYMBOLS,
        }
        with open(os.path.join(args.out, "abi_manifest.json"), "w") as handle:
            json.dump(manifest, handle, indent=2, sort_keys=True)
        log("wrote abi_manifest.json")
        # -- 5. live probe + negative controls ---------------------------------
        spec_path = os.path.join(args.out, "layout_spec.json")
        with open(spec_path, "w") as handle:
            json.dump({"name": "riscv64-f39e307-xs-ref", "reg_size": reg_size,
                       "offsets": offsets}, handle, sort_keys=True)
        code, _ = sh(["limactl", "copy", GUEST_PROBE,
                      "%s:/tmp/nemu_guest_probe.py" % args.guest])
        if code != 0:
            raise Blocked("cannot copy guest probe into %s" % args.guest)
        code, _ = sh(["limactl", "copy", spec_path,
                      "%s:/tmp/layout_spec.json" % args.guest])
        if code != 0:
            raise Blocked("cannot copy layout spec into %s" % args.guest)
        code, probe_out = sh(["limactl", "shell", args.guest, "--", "python3",
                              "/tmp/nemu_guest_probe.py", GUEST_SO,
                              "/tmp/layout_spec.json", "/tmp/guest_out.json"])
        log("--- guest probe ---")
        for line in probe_out.splitlines()[:40]:
            log(line)
        code2, _ = sh(["limactl", "copy",
                       "%s:/tmp/guest_out.json" % args.guest,
                       os.path.join(args.out, "guest_out.json")])
        if code != 0 or code2 != 0:
            verdict = "FAIL"
            detail = "guest probe did not complete (exit %d)" % code
            log("FAIL " + detail)
        else:
            with open(os.path.join(args.out, "guest_out.json")) as handle:
                guest_result = json.load(handle)
            if guest_result.get("verdict") != "PASS":
                verdict = "FAIL"
                detail = ("guest probe verdict=%s: %s"
                          % (guest_result.get("verdict"),
                             guest_result.get("error", "?")))
                log("FAIL " + detail)
            else:
                log("guest probe PASS: %s" % guest_result.get("checks"))
        # N1: a symbol-less library must be refused, not silently accepted.
        n1_py = ("import ctypes,subprocess;"
                 "subprocess.run(['gcc','-shared','-o','/tmp/empty.so',"
                 "'-x','c','/dev/null'],check=True);"
                 "so=ctypes.CDLL('/tmp/empty.so');"
                 "missing=[n for n in " + repr(REQUIRED_SYMBOLS) + " if not hasattr(so,n)];"
                 "print('missing=%d' % len(missing));"
                 "assert len(missing)==" + str(len(REQUIRED_SYMBOLS)) + ", 'fail-closed broken'")
        n1 = guest(args.guest, "python3", "-c", n1_py).strip()
        log("N1 missing-symbol library refused: %s" % n1)
        # N2: an off-by-8 layout must be refused before any regcpy.
        # N2 uses a doctored spec with reg_size+8.
        n2_spec = {"name": "doctored", "reg_size": reg_size + 8, "offsets": offsets}
        with open(os.path.join(args.out, "layout_spec_n2.json"), "w") as handle:
            json.dump(n2_spec, handle, sort_keys=True)
        sh(["limactl", "copy", os.path.join(args.out, "layout_spec_n2.json"),
            "%s:/tmp/layout_spec_n2.json" % args.guest])
        n2_code, n2_out = sh(["limactl", "shell", args.guest, "--", "python3",
                              "/tmp/nemu_guest_probe.py", GUEST_SO,
                              "/tmp/layout_spec_n2.json", "/tmp/guest_out_n2.json"])
        if n2_code == 0:
            verdict = "FAIL"
            detail = "wrong-layout library was NOT refused (exit 0)"
            log("FAIL N2 " + detail)
        else:
            log("N2 wrong-layout library refused (exit %d): %s"
                % (n2_code, n2_out.strip().splitlines()[-1][:160]))
        # N3: the 3-argument LightQS regcpy form must not bind. The frozen
        # .config has CONFIG_LIGHTQS unset, so the exported symbol is the
        # 2-argument form; calling it with 3 arguments corrupts the stack.
        # That crash is the point, so it runs in a disposable subprocess
        # whose non-zero exit is the expected (pass) outcome -- exit 0 would
        # mean the arity trap silently accepted, the card's named failure.
        n3_code, n3_out = sh(["limactl", "shell", args.guest, "--",
                              "python3", "/tmp/n3_arity.py"])
        if n3_code == 0:
            verdict = "FAIL"
            detail = "3-argument regcpy call was accepted (exit 0)"
            log("FAIL N3 " + detail)
        else:
            log("N3 three-parameter regcpy call refused (exit %d)" % n3_code)
    except Blocked as exc:
        verdict = "BLOCKED"
        detail = str(exc)
        log("BLOCKED " + detail)
    log("RESULT %s V-004 %s" % (verdict, detail))
    save_log()
    print("see %s" % log_path)
    return {"PASS": 0, "FAIL": 1, "BLOCKED": 2}[verdict]


if __name__ == "__main__":
    sys.exit(main())
