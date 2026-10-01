#!/usr/bin/env python3
"""Audit every MosaicRV p0 firmware ELF against the frozen p0 ISA.

    python3 check_p0_isa.py --objdump riscv64-elf-objdump \
                            --readelf riscv64-elf-readelf \
                            build/*.elf

Checks performed per ELF, each of which is a hard failure:

  1. Every disassembled instruction mnemonic is in the rv64im_zicsr_zifencei
     set (plus the M-mode privileged instructions p0 implies).  objdump runs
     with -M no-aliases so pseudo-instructions (li, mv, j, ret, nop, not,
     neg, ...) are reported as the real encodings they expand to; a
     pseudo-name appearing in the output is itself a failure, because it
     means the image was not audited at the encoding level.
  2. The ELF .riscv.attributes ISA string is exactly the canonical expansion
     of -march=rv64im_zicsr_zifencei, and every extension token in it is a
     p0 extension.
  3. No libc/libgcc: no runtime helper symbol, no PT_INTERP, no NEEDED
     library, no .plt/.dynamic/.got.plt, and no relocation at all.
  4. The entry point is the frozen reset vector 0x80000000.

Exit status 0 only when every ELF passes every check.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys

# Canonical expansion of -march=rv64im_zicsr_zifencei as readelf prints it:
# the toolchain adds version numbers and spells M as zmmul.
DEFAULT_ARCH = "rv64i2p1_m2p0_zicsr2p0_zifencei2p0_zmmul1p0"
DEFAULT_RESET_VECTOR = 0x80000000

# The complete RV64IM_Zicsr_Zifencei instruction set, as objdump prints the
# canonical (non-aliased) mnemonics.
P0_INSTRUCTIONS = frozenset(
    """
    lui auipc jal jalr
    beq bne blt bge bltu bgeu
    lb lh lw lbu lhu lwu ld
    sb sh sw sd
    addi slti sltiu xori ori andi
    slli srli srai
    add sub sll slt sltu xor srl sra or and
    addiw slliw srliw sraiw addw subw sllw srlw sraw
    mul mulh mulhsu mulhu div divu rem remu
    mulw divw divuw remw remuw
    fence fence.i ecall ebreak
    csrrw csrrs csrrc csrrwi csrrsi csrrci
    """.split()
)

# M-mode privileged instructions.  p0 is an M-mode-only profile
# (config/profiles/p0.json -> privilege_modes = ["M"]), so the M-mode
# trap-return instruction belongs to the frozen ISA and is required by
# tests/programs/src/trap.S.  Nothing from S/U mode is permitted.
P0_PRIVILEGED = frozenset(["mret"])

P0_ALLOWED = P0_INSTRUCTIONS | P0_PRIVILEGED

# Extension tokens that may appear in the .riscv.attributes ISA string.
ARCH_TOKENS_ALLOWED = {
    "i": "RV64I",
    "m": "RV64M",
    "zmmul": "RV64M low-word multiply subset",
    "zicsr": "Zicsr",
    "zifencei": "Zifencei",
}

ARCH_TOKEN_RE = re.compile(r"^([a-z]+)(\d+p\d+)?$")

# libgcc / libc runtime helpers a 64-bit multiply or divide can drag in.
FORBIDDEN_SYMBOLS = (
    "__muldi3", "__mulsi3", "__divdi3", "__udivdi3", "__moddi3",
    "__umoddi3", "__divsi3", "__udivsi3", "__modsi3", "__umodsi3",


    "__clzdi2", "__ctzdi2", "__ashldi3", "__ashrdi3", "__lshrdi3",
    "memcpy", "memset", "memmove", "strlen", "printf", "exit",
)


class AuditError(Exception):
    pass


def run(cmd: list) -> str:
    try:
        proc = subprocess.run(cmd, check=True, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT)
    except FileNotFoundError:
        raise AuditError("tool not found: %s" % (cmd[0],))
    except subprocess.CalledProcessError as exc:
        raise AuditError("command failed: %s\n%s"
                         % (" ".join(cmd), exc.stdout.decode("utf-8", "replace")))
    return proc.stdout.decode("utf-8", "replace")


def check_disassembly(objdump: str, path: str) -> int:
    text = run([objdump, "-d", "-M", "no-aliases,numeric", path])
    mnemonic_re = re.compile(r"^\s*[0-9a-f]+:\s+[0-9a-f ]+\t(\S+)\s*(.*)$")
    unknown = []
    count = 0
    for line in text.splitlines():
        match = mnemonic_re.match(line)
        if match is None:
            continue
        mnemonic = match.group(1)
        count += 1
        if mnemonic in P0_ALLOWED:
            continue
        unknown.append((mnemonic, line.strip()))
    if count == 0:
        raise AuditError("%s: no instructions disassembled" % path)
    if unknown:
        detail = "\n".join("    %s   <- %s" % (m, l) for m, l in unknown[:20])
        raise AuditError(
            "%s: %d instruction(s) outside rv64im_zicsr_zifencei:\n%s"
            % (path, len(unknown), detail))
    return count


def check_arch_tokens(path: str, arch: str) -> None:
    """Every extension token in the ISA string must be a p0 extension.

    This is what stops a future -march change from quietly advertising A, C,
    F, D, V, or an unvetted Z-extension: the failure names the offender.
    The leading "rv64" carries XLEN, not an extension, and is stripped.
    """
    match = re.match(r"^rv(\d+)(.*)$", arch)
    if match is None:
        raise AuditError("%s: ISA string %r has no rvN prefix" % (path, arch))
    if match.group(1) != "64":
        raise AuditError("%s: ISA string %r is not RV64" % (path, arch))
    for token in match.group(2).split("_"):
        if not token:
            continue
        name_match = ARCH_TOKEN_RE.match(token)
        if name_match is None:
            raise AuditError("%s: unparsable ISA token %r in %r"
                             % (path, token, arch))
        name = name_match.group(1)
        if name not in ARCH_TOKENS_ALLOWED:
            raise AuditError(
                "%s: ISA string %r advertises %r, which is not in "
                "rv64im_zicsr_zifencei (allowed: %s)"
                % (path, arch, name,
                   ", ".join(sorted(ARCH_TOKENS_ALLOWED))))


def check_arch_attribute(readelf: str, path: str, want: str) -> str:
    text = run([readelf, "-A", path])
    for line in text.splitlines():
        if "Tag_RISCV_arch" in line and ":" in line:
            got = line.split(":", 1)[1].strip().strip('"')
            if got != want:
                raise AuditError(
                    "%s: .riscv.attributes ISA string is %r, expected %r"
                    % (path, got, want))
            check_arch_tokens(path, got)
            return got
    raise AuditError("%s: no Tag_RISCV_arch in .riscv.attributes "
                     "(attributes were stripped?)" % path)


def run_optional(cmd: list) -> str:
    """Run a query that legitimately fails on a static, fully linked image.

    objdump -T and -R exit non-zero with "not a dynamic object" for a static
    executable.  That output is exactly what we want to see, so it is read
    rather than treated as a tool failure.
    """
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT)
    except FileNotFoundError:
        raise AuditError("tool not found: %s" % (cmd[0],))
    return proc.stdout.decode("utf-8", "replace")


def check_no_runtime_helper(objdump: str, path: str) -> None:
    dynsyms = run_optional([objdump, "-t", path])
    dynsyms += run_optional([objdump, "-T", path])
    for name in FORBIDDEN_SYMBOLS:
        if name in dynsyms:
            raise AuditError("%s: dynamic symbol %s -- libc/libgcc leaked in"
                             % (path, name))

    headers = run([objdump, "-p", path])
    if "INTERP" in headers or "interpreter" in headers:
        raise AuditError("%s: PT_INTERP present; image is not static" % path)
    for line in headers.splitlines():
        if line.strip().startswith("NEEDED"):
            raise AuditError("%s: shared library dependency %r"
                             % (path, line.strip()))

    sections = run([objdump, "-h", path])
    for bad in (".plt", ".rela.plt", ".got.plt", ".dynamic"):
        if re.search(r"\s%s\s" % (re.escape(bad)), sections):
            raise AuditError("%s: section %s present" % (path, bad))

    # Any relocation at all means a symbol the loader would have to resolve,
    # which is impossible for a self-contained bare-metal image.  objdump
    # prints only a file header for a fully linked image, so look for actual
    # relocation records rather than for any output at all.
    reloc_text = run_optional([objdump, "-r", path])
    records = [line for line in reloc_text.splitlines() if "R_RISCV" in line]
    if records:
        raise AuditError("%s: %d relocation(s) present, first few:\n%s"
                         % (path, len(records), "\n".join(records[:10])))


def check_entry(objdump: str, path: str, want_entry: int) -> None:
    text = run([objdump, "-f", path])
    match = re.search(r"start address 0x([0-9a-f]+)", text)
    if match is None:
        raise AuditError("%s: no start address in objdump -f" % path)
    got = int(match.group(1), 16)
    if got != want_entry:
        raise AuditError("%s: entry 0x%016x, expected reset vector 0x%016x"
                         % (path, got, want_entry))


def audit_one(objdump: str, readelf: str, arch: str, path: str) -> int:
    count = check_disassembly(objdump, path)
    check_arch_attribute(readelf, path, arch)
    check_no_runtime_helper(objdump, path)
    return count


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elfs", nargs="+")
    parser.add_argument("--objdump", default="riscv64-elf-objdump")
    parser.add_argument("--readelf", default="riscv64-elf-readelf")
    parser.add_argument("--arch", default=DEFAULT_ARCH)
    parser.add_argument("--reset-vector", default=hex(DEFAULT_RESET_VECTOR))
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)

    want_entry = int(args.reset_vector, 16)
    total = 0
    failures = []
    for path in args.elfs:
        try:
            count = audit_one(args.objdump, args.readelf, args.arch, path)
            check_entry(args.objdump, path, want_entry)
            total += count
            if not args.quiet:
                print("ok   %-44s %5d instructions, entry=0x%08x"
                      % (path, count, want_entry))
        except AuditError as exc:
            failures.append(str(exc))
            print("FAIL %s" % (path,))

    if failures:
        sys.stderr.write("\n=== p0 ISA audit failures ===\n")
        for item in failures:
            sys.stderr.write(item + "\n")
        return 1

    print("\naudit: %d ELF(s), %d instructions, all in rv64im_zicsr_zifencei; "
          "no libgcc, no libc, no relocations, entry 0x%08x"
          % (len(args.elfs), total, want_entry))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))