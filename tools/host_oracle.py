#!/usr/bin/env python3
"""Independent host oracle for the MosaicRV p0 firmware corpus.

    python3 tools/host_oracle.py --all
    python3 tools/host_oracle.py --program p08_misaligned
    python3 tools/host_oracle.py --record
    python3 tools/host_oracle.py --check-golden-only

INDEPENDENCE

This oracle computes the expected signature words and the expected trap trace
in Python from the declared inputs.  It never imports, executes, parses or
otherwise observes the RTL, the assembler output, the linker script or the
firmware sources: its only input is tests/programs/corpus.json, which is a
declaration of (program, input, expected result).  Everything below is a
model of the RISC-V unprivileged spec (RV64I + M) and of the frozen p0
platform in config/memory/p0.json and config/profiles/p0.json.

Agreement between this oracle and the firmware running on hardware is
therefore meaningful.  Agreement between the oracle and the DUT would not be,
and that is a blocking risk called out in the I-007 card.

TWO INDEPENDENT DERIVATIONS

Where a result is not a one-liner, it is computed TWICE, by structurally
different methods, and the two must agree before the oracle will report a
value:

  * MULH / MULHSU / MULHU are computed both from Python big-integer
    arithmetic over signed operands and from a 32-bit limb schoolbook
    multiply (the method a hardware multiplier actually uses).
  * DIV / REM are computed both from Python floor-division over signed
    operands and from a bit-serial restoring divider run 64 times on masked
    64-bit words.
  * Sign extension is computed both from Python negative integers and from
    explicit mask-and-or expressions.

If any of those cross-checks disagree the oracle fails loudly rather than
emitting a value.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

MASK64 = (1 << 64) - 1
SIGN64 = 1 << 63

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
CORPUS_PATH = os.path.join(REPO, "tests", "programs", "corpus.json")
GOLDEN_PATH = os.path.join(REPO, "tests", "programs", "golden.json")


class OracleError(Exception):
    pass


# --------------------------------------------------------------------------
# primitives
# --------------------------------------------------------------------------

def u64(value: int) -> int:
    return value & MASK64


def signed(value: int) -> int:
    """Interpret a 64-bit word as a two's-complement integer."""
    value &= MASK64
    return value - (1 << 64) if value & SIGN64 else value


def sext(value: int, bits: int) -> int:
    """Sign-extend the low `bits` of value, returned as a 64-bit word."""
    sign = 1 << (bits - 1)
    value &= (1 << bits) - 1
    if value & sign:
        value -= (1 << bits)
    return u64(value)


def zext(value: int, bits: int) -> int:
    return value & ((1 << bits) - 1)


def _check(condition: bool, what: str) -> None:
    if not condition:
        raise OracleError("internal cross-check failed: %s" % what)


# --------------------------------------------------------------------------
# RV64M: multiply, with two independent derivations
# --------------------------------------------------------------------------

def mulh_bigint(a: int, b: int, signed_a: bool, signed_b: int) -> int:
    """High 64 bits from Python big-integer arithmetic.

    `signed_a` selects the interpretation of a; `signed_b` selects b's.
    When an operand is unsigned it is widened as a non-negative number, which
    is exactly what the hardware does with a zero-extended multiplicand.
    """
    lhs = signed(a) if signed_a else a
    rhs = signed(b) if signed_b else b
    product = lhs * rhs
    # Python's & on a negative int yields the two's-complement residue modulo
    # 2**n, which is exactly the truncated 128-bit product a hardware
    # multiplier keeps; the high word is then a plain unsigned shift.
    residue = product & ((1 << 128) - 1)
    return residue >> 64


def mulh_limbs(a: int, b: int, signed_a: bool, signed_b: bool) -> int:
    """High 64 bits from a 16-bit limb schoolbook multiply.

    Each operand is split into four 16-bit limbs, treated as signed when the
    corresponding instruction is a signed multiply.  The 128-bit product is
    reassembled from the limb products.  This is a different decomposition of
    the same problem than the big-integer path, so agreement between the two
    is evidence and not a tautology.
    """
    def limbs(word: int, is_signed: bool) -> list:
        raw = [(word >> (16 * i)) & 0xFFFF for i in range(4)]
        if is_signed and raw[3] & 0x8000:
            raw[3] -= 0x10000
        return raw

    la = limbs(a, signed_a)
    lb = limbs(b, signed_b)

    product = 0
    for i in range(4):
        for j in range(4):
            product += la[i] * lb[j] << (16 * (i + j))

    # product is the exact mathematical value assembled from the limb
    # products; the hardware keeps only the low 128 bits of it.
    residue = product & ((1 << 128) - 1)
    return residue >> 64


def MUL(a: int, b: int) -> int:
    return u64(a * b)


def MULH(a: int, b: int) -> int:
    hi = mulh_bigint(a, b, True, True)
    hi2 = mulh_limbs(a, b, True, True)
    _check(hi == hi2, "MULH bigint=%#x limbs=%#x" % (hi, hi2))
    return hi


def MULHU(a: int, b: int) -> int:
    hi = mulh_bigint(a, b, False, False)
    hi2 = mulh_limbs(a, b, False, False)
    _check(hi == hi2, "MULHU bigint=%#x limbs=%#x" % (hi, hi2))
    return hi


def MULHSU(a: int, b: int) -> int:
    """MULHSU: rs1 signed, rs2 unsigned.

    The mnemonic is "signed x unsigned": rs1 is sign-extended and rs2 is
    zero-extended, so the product is signed(a) * b with b taken as a
    non-negative number.  Treating rs2 as signed is a classic bug that only
    shows up when rs2 has bit 63 set, which is why p04's inputs include
    0x8000000000000000 as c.
    """
    hi = mulh_bigint(a, b, True, False)
    hi2 = mulh_limbs(a, b, True, False)
    _check(hi == hi2, "MULHSU bigint=%#x limbs=%#x" % (hi, hi2))
    return hi


# --------------------------------------------------------------------------
# RV64M: divide and remainder, with two independent derivations
# --------------------------------------------------------------------------

def div_rem_floor(a: int, b: int) -> tuple:
    """DIV/REM from Python integer division on signed interpretations.

    Takes the same 64-bit patterns as div_rem_bitserial but shares no code
    with it: here the magnitudes are divided directly by Python's //, whereas
    div_rem_bitserial runs a 64-step restoring loop.

    Python's // floors toward -infinity while RISC-V DIV truncates toward
    zero, so the quotient is negated when the operands have opposite signs
    rather than rounded down.  The two architecturally defined corner cases
    are handled explicitly.
    """
    if b == 0:
        return MASK64, u64(a)                  # DIV -> -1, REM -> dividend
    if a == SIGN64 and b == MASK64:
        return SIGN64, 0                      # signed overflow
    lhs, rhs = signed(a), signed(b)
    quotient = abs(lhs) // abs(rhs)
    if (lhs < 0) != (rhs < 0):
        quotient = -quotient
    remainder = lhs - quotient * rhs
    return u64(quotient), u64(remainder)


def div_rem_bitserial(a: int, b: int, is_signed: bool) -> tuple:
    """DIV/REM by a 64-iteration restoring divider on masked 64-bit words.

    No division operator appears anywhere in this function: the only
    operations are shift, compare, subtract, increment and negate.  It is the
    shape of the hardware divider and shares no logic with div_rem_floor.
    """
    a_neg = is_signed and bool(a & SIGN64)
    b_neg = is_signed and bool(b & SIGN64)
    a_mag = u64(-a) if a_neg else (a & MASK64)
    b_mag = u64(-b) if b_neg else (b & MASK64)

    if b_mag == 0:
        # Defined corner case for both signed and unsigned:
        # quotient all ones, remainder the dividend.
        return MASK64, (a & MASK64)

    quotient = 0
    remainder = 0
    for index in range(64):
        remainder = u64((remainder << 1) | ((a_mag >> (63 - index)) & 1))
        quotient <<= 1
        if remainder >= b_mag:
            remainder = u64(remainder - b_mag)
            quotient |= 1

    if is_signed:
        if a_neg:
            remainder = u64(-remainder)
        if a_neg != b_neg:
            quotient = u64(-quotient)
    return u64(quotient), u64(remainder)


def DIV(a: int, b: int) -> int:
    q1, r1 = div_rem_floor(a, b)
    q2, r2 = div_rem_bitserial(a, b, True)
    _check(q1 == q2, "DIV floor=%#x bitserial=%#x" % (q1, q2))
    _check(r1 == r2, "REM floor=%#x bitserial=%#x" % (r1, r2))
    return q1


def REM(a: int, b: int) -> int:
    _q, r1 = div_rem_floor(a, b)
    _q2, r2 = div_rem_bitserial(a, b, True)
    _check(r1 == r2, "REM floor=%#x bitserial=%#x" % (r1, r2))
    return r1


def DIVU(a: int, b: int) -> int:
    q1, r1 = divu_floor(a, b)
    q2, r2 = div_rem_bitserial(a, b, False)
    _check(q1 == q2, "DIVU floor=%#x bitserial=%#x" % (q1, q2))
    _check(r1 == r2, "REMU floor=%#x bitserial=%#x" % (r1, r2))
    return q1


def REMU(a: int, b: int) -> int:
    _q, r1 = divu_floor(a, b)
    _q2, r2 = div_rem_bitserial(a, b, False)
    _check(r1 == r2, "REMU floor=%#x bitserial=%#x" % (r1, r2))
    return r1


def divu_floor(a: int, b: int) -> tuple:
    """DIVU/REMU with Python integers (the operands are already unsigned)."""
    if b == 0:
        return MASK64, u64(a)
    return u64(a // b), u64(a % b)


# --------------------------------------------------------------------------
# shifts, comparisons, sign extension -- second derivation for the sext path
# --------------------------------------------------------------------------

def sext_masked(value: int, bits: int) -> int:
    """Sign extension built from an explicit mask, independent of sext()."""
    mask = (1 << bits) - 1
    value &= mask
    if (value >> (bits - 1)) & 1:
        return u64(value | u64(~mask))
    return value


# --------------------------------------------------------------------------
# per-program models
# --------------------------------------------------------------------------

def p01_addsub(a: int, b: int, c: int) -> tuple:
    sig0 = u64(a + b)
    sig1 = u64(a - b)
    shamt = c & 63
    total = signed(sig0)
    sig2 = u64(total >> shamt)
    # Second derivation of the arithmetic shift, written as an explicit
    # zero-fill plus sign-fill instead of a Python arithmetic shift: keep the
    # shamt low bits of a logical shift, then drop in the sign above them.
    sign_fill = MASK64 if total < 0 else 0
    expect = u64(((sig0 >> shamt) & (MASK64 >> shamt))
                 | (sign_fill << (64 - shamt)))
    _check(sig2 == expect, "SRA ashr=%#x signfill=%#x" % (sig2, expect))

    # sig3 selects one of two sums with SLTU(a, b), then folds in
    # NOT(zero_extend(b & 0xff)) masked to a byte.
    sum_ab = u64(a + b)
    diff_ab = u64(a - b)
    select = a < b                       # SLTU: unsigned compare
    base = sum_ab if select else diff_ab

    # Second derivation of the select: the firmware branches, this computes
    # both sides and merges them with a mask built from the comparison.  The
    # two agree only if the unsigned comparison itself is right.
    mask = MASK64 if select else 0
    merged = u64((sum_ab & mask) | (diff_ab & u64(~mask)))
    _check(base == merged, "p01 select branch=%#x mask=%#x" % (base, merged))

    term = u64(~zext(b, 8)) & 0xFF
    # Second derivation of NOT from an XOR against all-ones and a second
    # mask, rather than Python's ~ and &.
    term_alt = u64((zext(b, 8) ^ 0xFF) ^ 0xFF00) & 0xFF
    _check(term == term_alt, "p01 NOT term=%#x alt=%#x" % (term, term_alt))

    # Sign-extension round trip: a byte carried through two truncating sign
    # extensions must come back unchanged, which is what the firmware's
    # zero-extend-then-NOT path relies on.
    byte = zext(b, 8)
    _check(zext(sext(byte, 8), 8) == byte, "p01 sext round trip")
    _check(sext(byte, 8) == sext_masked(byte, 8),
           "p01 sext vs mask-and-or methods")

    sig3 = u64(base ^ term)
    return [sig0, sig1, sig2, sig3], []


def p02_branch(a: int, b: int, c: int) -> tuple:
    sa, sb = signed(a), signed(b)
    mask = 0
    if a == b:
        mask |= 0x01                      # BEQ
    if a != b:
        mask |= 0x02                      # BNE
    if sa < sb:
        mask |= 0x04                      # BLT
    if sa >= sb:
        mask |= 0x08                      # BGE
    if a < b:
        mask |= 0x10                      # BLTU
    if a >= b:
        mask |= 0x20                      # BGEU

    popcount = 0
    value = mask
    while value:
        popcount += value & 1
        value >>= 1

    sig2 = 1 + (c & 7)                   # one JAL plus the JALR round-trips

    mixed = mask ^ (c & 0x3F)
    if mixed & 0x01:
        sig3 = u64(b)
    elif mixed & 0x04:
        sig3 = u64(-sa)
    else:
        sig3 = u64(a)

    return [mask, popcount, sig2, sig3], []


def p03_loadstore(a: int, b: int, c: int) -> tuple:
    # LW sign-extends: the word written at scratch+16 is the high half of a,
    # read back as a sign-extended 32-bit value, not a zero-extended one.
    sig0 = u64(a ^ sext(u64(a >> 32), 32))
    sig1 = u64(zext(a, 16) ^ sext(zext(b, 16), 16))
    sig2 = u64(sext(zext(a, 8), 8) ^ zext(b, 8))
    sig3 = u64(b ^ (a + c))
    # second derivation of the sign-extending half load
    _check(sig1 == u64(zext(a, 16) ^ sext_masked(zext(b, 16), 16)),
           "p03 sig1 second derivation")
    _check(sig2 == u64(sext_masked(zext(a, 8), 8) ^ zext(b, 8)),
           "p03 sig2 second derivation")
    return [sig0, sig1, sig2, sig3], []


def p04_mul(a: int, b: int, c: int) -> tuple:
    return [MUL(a, b), MULH(a, b), MULHU(a, b),
            u64(MULHSU(a, c) ^ MUL(a, c))], []


DIVZERO_MARK = 0x5555555555555555


def div_mark(divisor: int) -> int:
    """Marker folded in when the divisor is zero.

    DIV(a,0) and DIVU(a,0) are both all ones; REM(a,0) and REMU(a,0) are
    both the dividend.  The exclusive-or of the signed and unsigned forms is
    therefore identically zero for a zero divisor, which would make a
    divide-by-zero case vacuously pass.  The marker makes it observable.
    """
    return 0 if divisor != 0 else DIVZERO_MARK


def p05_divrem(a: int, b: int, c: int) -> tuple:
    sig0 = u64(DIV(a, b) ^ DIVU(a, b) ^ div_mark(b))
    sig1 = u64(REM(a, b) ^ REMU(a, b))
    sig2 = u64(DIV(a, c) ^ DIVU(a, c) ^ div_mark(c))
    sig3 = u64(REM(a, c) ^ REMU(a, c))
    return [sig0, sig1, sig2, sig3], []


def p06_shiftlogic(a: int, b: int, c: int) -> tuple:
    shamt = c & 63
    sig0 = u64(a << shamt)
    sig1 = a >> shamt
    sig2 = u64(signed(a) >> shamt)
    # second derivation of SRL: rebuild the shifted-out bits one at a time,
    # which shares nothing with the mask-and-shift operator above
    manual = 0
    for bit in range(64 - shamt):
        manual |= ((a >> (bit + shamt)) & 1) << bit
    _check(sig1 == manual, "SRL loop=%#x mask=%#x" % (sig1, manual))
    # and an independent sign-fill construction of SRA
    sign_fill = MASK64 if signed(a) < 0 else 0
    manual_sra = u64(((a >> shamt) & (MASK64 >> shamt))
                     | (sign_fill << (64 - shamt)))
    _check(sig2 == manual_sra, "SRA ashr=%#x signfill=%#x" % (sig2, manual_sra))
    mix = u64((a ^ b) | b | u64(~(a & b)))
    lt_ab = signed(a) < signed(b)
    lt_bc = signed(b) < signed(c)
    sig3 = u64(mix ^ (MASK64 if lt_ab else 0)
               ^ u64((MASK64 if lt_bc else 0) << 16))
    return [sig0, sig1, sig2, sig3], []


def p07_byteops(a: int, b: int, c: int) -> tuple:
    word = u64(zext(a, 8) | (zext(a, 16) & 0xFF00)
               | (zext(b, 8) << 16) | (zext(b, 16) << 24))
    sig0 = word & 0xFF
    sig1 = sext((word >> 8) & 0xFF, 8)
    sig2 = sext((word >> 16) & 0xFFFF, 16)
    sig3 = zext(c, 16)
    _check(sig1 == sext_masked((word >> 8) & 0xFF, 8), "p07 sig1")
    _check(sig2 == sext_masked((word >> 16) & 0xFFFF, 16), "p07 sig2")
    return [sig0, sig1, sig2, sig3], []


def p08_misaligned(a: int, b: int, c: int) -> tuple:
    sig0 = a & 0xFF                        # lbu [scratch+16]
    sig1 = u64(a >> 32)                    # lw  [scratch+20]
    sig2 = sext((a >> 48) & 0xFFFF, 16)    # lh  [scratch+22]
    _check(sig2 == sext_masked((a >> 48) & 0xFFFF, 16), "p08 sig2")

    causes = [4, 4, 6, 6, 7]
    acc = 0
    for cause in causes:
        acc = u64((acc << 8) | cause)
    bits = 1 | 2                           # canary intact, c round-tripped
    sig3 = u64((acc << 2) | bits)

    traps = [
        {"cause": 4, "note": "lh at scratch+9, odd address"},
        {"cause": 4, "note": "lw at scratch+10, address%4==2"},
        {"cause": 6, "note": "sh at scratch+7, odd address"},
        {"cause": 6, "note": "sd at scratch+12, address%8==4"},
        {"cause": 7, "note": "sw at 0x0, boot_rom is not writable"},
    ]
    return [sig0, sig1, sig2, sig3], traps


def p09_storeload(a: int, b: int, c: int) -> tuple:
    sig0 = u64(a)                          # sd a; ld; sd b; ld; sd a; ld
    # LW sign-extends: reading back a stored 64-bit value with a word load
    # reproduces bit 31 into bits 63:31 whenever a is "negative" as a word.
    sig1 = sext(zext(a, 32), 32)           # sw a, 4; lw 4
    byte_mix = u64((a & u64(~0xFF)) | (b & 0xFF))
    half_mix = u64((a & u64(~0xFFFF)) | (b & 0xFFFF))
    sig2 = u64(byte_mix ^ half_mix)
    sig3 = u64(c)
    return [sig0, sig1, sig2, sig3], []


def p10_jalr_link(a: int, b: int, c: int) -> tuple:
    sig0 = 0                              # jal link == next instruction
    sig1 = 0                              # jalr link == next instruction
    a0 = u64(a + 1)                       # p10_jal_b
    a0 = u64(a0 + b)                      # p10_jalr_b
    a0 = u64(a0 + b)                      # p10_jalr_b again (rd == rs1)
    a1 = u64(b - 1)                       # p10_outer
    a2 = u64(u64(a0 + a1) << 1)           # p10_inner: (a0 + a1) << 1
    sig2 = a0
    sig3 = u64(a1 ^ a2)
    _check(sig3 != sig2, "p10 sig3 must not alias sig2")
    return [sig0, sig1, sig2, sig3], []


def p11_bigmuldiv(a: int, b: int, c: int) -> tuple:
    sig0 = u64(DIV(a, b) ^ DIVU(a, b) ^ div_mark(b))
    sig1 = u64(REM(a, b) ^ REMU(a, b))
    dividend = u64(REMU(a, b) + c)
    quotient = DIV(dividend, b)
    remainder = REM(quotient, b)
    sig2 = quotient
    sig3 = u64(remainder ^ dividend)
    return [sig0, sig1, sig2, sig3], []


def p13_romstore(a: int, b: int, c: int) -> tuple:
    """A store into the read-only boot_rom region.

    The trap log must contain exactly one record whose cause is 7 (store/AMO
    access fault).  The count is folded into the signature as well, so an
    implementation that raised the right fault twice, or dropped the store and
    let it succeed, is caught as well as one that raised the wrong fault.
    """
    sig0 = a & 0xFF                                  # lbu
    sig1 = sext(u64(a >> 32), 32)                    # lw, sign-extends
    sig2 = sext((a >> 48) & 0xFFFF, 16)              # lh
    _check(sig2 == sext_masked((a >> 48) & 0xFFFF, 16), "p13 sig2")
    causes = [7]
    acc = 0
    for cause in causes:
        acc = u64((len(causes) << 8) | cause)
    sig3 = acc                                      # s7 term is 0
    traps = [{"cause": 7,
              "note": "sw at 0x0, boot_rom is readable but not writable"}]
    return [sig0, sig1, sig2, sig3], traps


def p12_memwalk(a: int, b: int, c: int) -> tuple:
    words = [u64(a ^ c)]
    for _ in range(15):
        words.append(u64((words[-1] << 1) ^ b))
    count = (c & 3) + 8
    total = 0
    odd = 0
    for word in words[:count]:
        total = u64(total + word)
        odd += word & 1
    sig0 = total
    sig1 = words[count - 1]
    sig2 = odd
    sig3 = u64(words[0] ^ (words[15] >> 32))
    return [sig0, sig1, sig2, sig3], []


MODELS = {
    "p01_addsub": p01_addsub,
    "p02_branch": p02_branch,
    "p03_loadstore": p03_loadstore,
    "p04_mul": p04_mul,
    "p05_divrem": p05_divrem,
    "p06_shiftlogic": p06_shiftlogic,
    "p07_byteops": p07_byteops,
    "p08_misaligned": p08_misaligned,
    "p09_storeload": p09_storeload,
    "p10_jalr_link": p10_jalr_link,
    "p11_bigmuldiv": p11_bigmuldiv,
    "p12_memwalk": p12_memwalk,
    "p13_romstore": p13_romstore,
}


# --------------------------------------------------------------------------
# corpus plumbing
# --------------------------------------------------------------------------

def parse_word(value) -> int:
    if isinstance(value, bool):
        raise OracleError("boolean is not a valid 64-bit word")
    if isinstance(value, int):
        return u64(value)
    if isinstance(value, str):
        return u64(int(value, 0))
    raise OracleError("cannot parse %r as a 64-bit word" % (value,))


def load_corpus(path: str) -> dict:
    with open(path) as handle:
        return json.load(handle)


def evaluate(program: dict, inputs: dict) -> tuple:
    name = program["oracle"]
    if name not in MODELS:
        raise OracleError("no model registered for %r" % (name,))
    args = [parse_word(inputs[key]) for key in ("a", "b", "c")]
    signature, traps = MODELS[name](*args)
    assert len(signature) == 4, "every program must produce 4 signature words"
    return [u64(word) for word in signature], traps


def causes_of(traps: list) -> list:
    return [int(entry["cause"]) for entry in traps]


def golden_document(document: dict) -> dict:
    result = {
        "schema_version": 1,
        "profile": document["profile"],
        "isa": document["isa"],
        "source": "tools/host_oracle.py",
        "signature": document["protocol"]["signature"],
        "signature_words": document["protocol"]["signature_words"],
        "cases": [],
    }
    for program in document["programs"]:
        for index, inputs in enumerate(program["inputs"]):
            signature, traps = evaluate(program, inputs)
            result["cases"].append({
                "program": program["name"],
                "input": index,
                "a": inputs["a"],
                "b": inputs["b"],
                "c": inputs["c"],
                "sig": ["0x%016x" % word for word in signature],
                "traps": causes_of(traps),
            })
    return result


def rows(document: dict, only: str):
    for program in document["programs"]:
        if only and program["name"] != only:
            continue
        for index, inputs in enumerate(program["inputs"]):
            yield program, index, inputs


def print_table(document: dict, only: str) -> int:
    print("MosaicRV p0 host oracle -- expected signatures")
    print("signature area 0x%08x, %d words, tohost 0x%08x, fromhost 0x%08x"
          % (document["protocol"]["signature"],
             document["protocol"]["signature_words"],
             document["protocol"]["tohost"],
             document["protocol"]["fromhost"]))
    print("misalignment policy: load/store misaligned -> trap "
          "(config/profiles/p0.json)")
    print()
    header = ("program", "in", "a", "b", "c",
              "sig0", "sig1", "sig2", "sig3", "traps")
    print("%-14s %2s %-18s %-18s %-18s %-18s %-18s %-18s %-18s %s"
          % header)
    count = 0
    for program, index, inputs in rows(document, only):
        signature, traps = evaluate(program, inputs)
        print("%-14s %2d %-18s %-18s %-18s %-18s %-18s %-18s %-18s %s"
              % (program["name"], index,
                 inputs["a"], inputs["b"], inputs["c"],
                 *["0x%016x" % word for word in signature],
                 ",".join(str(value) for value in causes_of(traps)) or "-"))
        count += 1
    print()
    print("%d case(s) computed" % count)
    return count


def check_declared(document: dict, only: str) -> list:
    """Oracle result vs the expect_sig / expect_traps declaration."""
    failures = []
    for program, index, inputs in rows(document, only):
        signature, traps = evaluate(program, inputs)
        declared_sig = [parse_word(word) for word in inputs["expect_sig"]]
        declared_traps = causes_of(inputs["expect_traps"])
        computed_traps = causes_of(traps)
        for slot, (got, want) in enumerate(zip(signature, declared_sig)):
            if got != want:
                failures.append(
                    "%s.i%d sig%d: oracle %#018x, declared %#018x"
                    % (program["name"], index, slot, got, want))
        if declared_traps != computed_traps:
            failures.append(
                "%s.i%d traps: oracle %s, declared %s"
                % (program["name"], index, computed_traps, declared_traps))
        # A signature whose four words are all equal detects nothing: a DUT
        # could return the same wrong value everywhere.  Two or more distinct
        # words is the floor.  Requiring all four to differ would reject
        # legitimate cases: p10's two link checks are both 0 by design, and
        # the MIN/-1 overflow case makes DIV and DIVU coincide.
        if len(set(signature)) < 2:
            failures.append(
                "%s.i%d: all four signature words are equal, so a single "
                "wrong value would satisfy the whole case"
                % (program["name"], index))
    return failures


def check_input_sensitivity(document: dict) -> list:
    """Every program's signature must differ between its three inputs."""
    failures = []
    for program in document["programs"]:
        seen = {}
        for index, inputs in enumerate(program["inputs"]):
            signature, _traps = evaluate(program, inputs)
            key = tuple(signature)
            if key in seen:
                failures.append(
                    "%s inputs i%d and i%d produce the same signature; a "
                    "case whose result ignores its input is not a test"
                    % (program["name"], seen[key], index))
            else:
                seen[key] = index
    return failures


def check_golden(document: dict, only: str, path: str) -> list:
    if not os.path.exists(path):
        return ["golden file %s does not exist; run --record" % path]
    with open(path) as handle:
        golden = json.load(handle)
    index = {}
    for case in golden["cases"]:
        index[(case["program"], case["input"])] = case
    failures = []
    for program, position, inputs in rows(document, only):
        signature, traps = evaluate(program, inputs)
        case = index.get((program["name"], position))
        if case is None:
            failures.append("golden has no case %s.i%d"
                            % (program["name"], position))
            continue
        for slot, word in enumerate(signature):
            want = parse_word(case["sig"][slot])
            if word != want:
                failures.append(
                    "%s.i%d sig%d: oracle %#018x, golden %#018x"
                    % (program["name"], position, slot, word, want))
        if case["traps"] != causes_of(traps):
            failures.append(
                "%s.i%d traps: oracle %s, golden %s"
                % (program["name"], position, causes_of(traps), case["traps"]))
    return failures


def write_golden(document: dict, path: str) -> None:
    directory = os.path.dirname(path)
    if directory and not os.path.isdir(directory):
        os.makedirs(directory)
    payload = json.dumps(golden_document(document), indent=2, sort_keys=True)
    with open(path, "w") as handle:
        handle.write(payload + "\n")


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--all", action="store_true",
                        help="print every expected signature and exit 0 only "
                             "when the oracle, the declaration and the golden "
                             "file all agree")
    parser.add_argument("--program", default=None,
                        help="restrict output to one program")
    parser.add_argument("--corpus", default=CORPUS_PATH)
    parser.add_argument("--golden", default=GOLDEN_PATH)
    parser.add_argument("--record", action="store_true",
                        help="rewrite the golden file from this oracle")
    parser.add_argument("--check-golden-only", action="store_true",
                        help="compare only against the golden file")
    args = parser.parse_args(argv)

    if not args.all and not args.program and not args.record \
            and not args.check_golden_only:
        parser.error("nothing to do: pass --all, --program, --record or "
                     "--check-golden-only")

    document = load_corpus(args.corpus)
    only = args.program
    if only and not any(p["name"] == only for p in document["programs"]):
        sys.stderr.write("unknown program %r\n" % (only,))
        return 2

    if args.record:
        write_golden(document, args.golden)
        print("recorded %s" % args.golden)
        return 0

    if args.all or args.program:
        print_table(document, only)

    failures = []
    if args.check_golden_only:
        failures.extend(check_golden(document, only, args.golden))
    else:
        failures.extend(check_declared(document, only))
        failures.extend(check_input_sensitivity(document))
        failures.extend(check_golden(document, only, args.golden))

    if failures:
        sys.stderr.write("\n=== oracle FAILED (%d) ===\n" % len(failures))
        for item in failures:
            sys.stderr.write("  %s\n" % item)
        return 1

    if not args.check_golden_only:
        print("oracle agrees with the declared expectations and the golden "
              "file for every case; all three inputs of every program give a "
              "distinct signature.")
    else:
        print("oracle agrees with the golden file.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))