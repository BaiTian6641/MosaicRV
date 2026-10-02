// ============================================================================
// tb_fpu.cpp -- CASE=fp.operation_matrix, work package I-049.
//
// The DUT is never its own oracle. The oracle for each operation class is named
// here, because a float test whose expectation came from the hardware under
// test would agree with a wrong implementation perfectly:
//
//   * ARITHMETIC (fadd/fsub/fmul/fdiv, both formats, every mode the host
//     supports) comes from the HOST's own floating-point unit through C's
//     `float`/`double` operators with `fesetround`, and the exception flags come
//     from `fetestexcept`. That is a second, independent implementation of
//     IEEE 754 on this machine -- the ARM FPU -- not a model written here, and
//     it is the strongest oracle available without a network. Operands are
//     volatile so the compiler cannot fold the operation away, and the flags are
//     cleared and read around each single operation.
//   * THE ONE MODE THE HOST LACKS: this platform's <fenv.h> does not define
//     FE_TONEARESTFROMZERO (checked, not assumed), so RMM has no host oracle.
//     RMM is measured on a directed tie table whose expectation is *derived from
//     the host* rather than written out by hand: at an exact tie the
//     ties-to-maximum-magnitude result is the away-from-zero neighbour, which is
//     exactly the host's RUP result for a positive result and its RDN result for
//     a negative one. The driver additionally proves, with exact dyadic
//     arithmetic (`Dya*` below, no rounding anywhere, independent of both the
//     RTL and the host), that each vector in that table really is a tie: the
//     exact result equals the midpoint of the two neighbours the host produces
//     at RDN and RUP. So the RMM expectation is hosted *and* the vector that
//     exercises it is independently shown to be a tie.
//   * CONVERSIONS int->fp come from the host's own conversion instruction under
//     the current rounding mode, with NX from fetestexcept. Conversions fp->int
//     have no C equivalent for the RISC-V saturation rule (C's cast is undefined
//     out of range), so they come from a small model written here from the
//     spec, and its in-range answers are *cross-checked* against the host's
//     llrint/lrint, which round identically.
//   * COMPARISONS, fmin/fmax, fclass, fsgnj and the moves are bit-level
//     predicates over the operand encodings, written here from the ISA: they
//     involve no arithmetic to have a second implementation of.
//
// Phases, each of which can fail on its own:
//
//   1. reset-state    the documented cold state.
//   2. ties           every arithmetic op and both formats at an exact tie,
//                     five modes, including the RMM table and the dyadic proof
//                     that each vector is a tie.
//   3. subnormal      subnormal operands, subnormal results, the boundary
//                     between subnormal and normal, UF/NX on tininess after
//                     rounding, and the S form's ignored upper half.
//   4. nan-policy     qNaN and sNaN operands, the canonical NaN, the invalid
//                     flag per operation, fmin/fmax with a NaN operand, and
//                     comparisons.
//   5. flags-matrix   NV, DZ, OF, UF and NX each on the operation that must set
//                     it, and the operations that must set none.
//   6. conversions    all eight int<->fp forms in both formats, with overflow,
//                     underflow, saturation and inexact.
//   7. misc           fsgnj*, fmv.*, fclass on every class, comparisons over
//                     every combination of zero, subnormal, normal and infinity.
//   8. latency        the measured latency against the unit's own declaration.
//   9. backpressure   a held result keeps its value, its flags and its tag.
//  10. flush          cancelling an operation in flight and a held result.
//  11. unimplemented  fsqrt returns the declared marker, not a number.
//  12. random         a seeded soak against the host oracle.
//
// Mutation hooks: the shipping build passes; each `-DMOSAIC_FPU_MUTANT_*` build
// must fail on the named check. See results/reports/I-049-fpu.md.
// ============================================================================

#include <verilated.h>

#include <cfenv>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_fpu_tb.h"

#pragma STDC FENV_ACCESS ON

namespace {

// ------------------------------------------------------------- small helpers
struct Abort {
  std::string what;
};

[[noreturn]] void Stop(const std::string& where, const std::string& detail) {
  throw Abort{where + ": " + detail};
}

std::string Bool(bool value) { return value ? "1" : "0"; }
std::string Dec(uint64_t value) { return std::to_string(value); }
std::string HexU(uint64_t value) { return mosaic::Hex(value); }

// --------------------------------------------------------------- encodings
// Mirrors of mosaic_pkg::fp_op_e. The numbering is the contract between the
// package, the unit and this driver, so the phases drive operations by number:
// a renumbering in the package fails here instead of silently agreeing.
enum : uint8_t {
  FP_ADD = 0, FP_SUB = 1, FP_MUL = 2, FP_DIV = 3, FP_SQRT = 4,
  FP_SGNJ = 5, FP_SGNJN = 6, FP_SGNJX = 7, FP_MIN = 8, FP_MAX = 9,
  FP_CMP_EQ = 10, FP_CMP_LT = 11, FP_CMP_LE = 12, FP_CLASS = 13,
  FP_MV_X = 14, FP_MV_W = 15, FP_CVT_FI = 16, FP_CVT_IF = 17,
  FP_CVT_FS = 18, FP_CVT_SF = 19
};

const char* OpName(uint8_t op) {
  switch (op) {
    case FP_ADD: return "fadd";
    case FP_SUB: return "fsub";
    case FP_MUL: return "fmul";
    case FP_DIV: return "fdiv";
    case FP_SQRT: return "fsqrt";
    case FP_SGNJ: return "fsgnj";
    case FP_SGNJN: return "fsgnjn";
    case FP_SGNJX: return "fsgnjx";
    case FP_MIN: return "fmin";
    case FP_MAX: return "fmax";
    case FP_CMP_EQ: return "feq";
    case FP_CMP_LT: return "flt";
    case FP_CMP_LE: return "fle";
    case FP_CLASS: return "fclass";
    case FP_MV_X: return "fmv.x";
    case FP_MV_W: return "fmv.w";
    case FP_CVT_FI: return "fcvt.fi";
    case FP_CVT_IF: return "fcvt.if";
    case FP_CVT_FS: return "fcvt.s.d";
    case FP_CVT_SF: return "fcvt.d.s";
    default: return "?";
  }
}

enum : uint8_t { RM_RNE = 0, RM_RTZ = 1, RM_RDN = 2, RM_RUP = 3, RM_RMM = 4 };
const char* RmName(uint8_t rm) {
  switch (rm) {
    case RM_RNE: return "rne";
    case RM_RTZ: return "rtz";
    case RM_RDN: return "rdn";
    case RM_RUP: return "rup";
    case RM_RMM: return "rmm";
    default: return "?";
  }
}

// RISC-V fflags bit order.
enum : uint32_t {
  FL_NX = 1u << 0,
  FL_UF = 1u << 1,
  FL_OF = 1u << 2,
  FL_DZ = 1u << 3,
  FL_NV = 1u << 4
};

std::string FlagStr(uint32_t f) {
  std::string s;
  if (f & FL_NV) s += "NV|";
  if (f & FL_DZ) s += "DZ|";
  if (f & FL_OF) s += "OF|";
  if (f & FL_UF) s += "UF|";
  if (f & FL_NX) s += "NX|";
  if (s.empty()) return "-";
  s.pop_back();
  return s;
}

// ------------------------------------------------------------ bit patterns
uint32_t F32Bits(float f) {
  uint32_t u = 0;
  std::memcpy(&u, &f, 4);
  return u;
}
float F32From(uint32_t u) {
  float f = 0;
  std::memcpy(&f, &u, 4);
  return f;
}
uint64_t F64Bits(double d) {
  uint64_t u = 0;
  std::memcpy(&u, &d, 8);
  return u;
}
double F64From(uint64_t u) {
  double d = 0;
  std::memcpy(&d, &u, 8);
  return d;
}
uint64_t CanonNaN(bool fmt) {
  return fmt ? 0x0000'0000'7FC0'0000ull : 0x7FF8'0000'0000'0000ull;
}
bool IsNaN64(uint64_t bits, bool fmt) {
  if (fmt) return ((bits >> 23) & 0xFF) == 0xFF && (bits & 0x7FFFFF) != 0;
  return ((bits >> 52) & 0x7FF) == 0x7FF && (bits & 0xFFFFFFFFFFFFFull) != 0;
}
// ---------------------------------------------------------- the host oracle
struct HostResult {
  uint64_t bits;
  uint32_t flags;
  bool is_nan;
};

int HostMode(uint8_t rm) {
  switch (rm) {
    case RM_RTZ: return FE_TOWARDZERO;
    case RM_RDN: return FE_DOWNWARD;
    case RM_RUP: return FE_UPWARD;
    default: return FE_TONEAREST;
  }
}

uint32_t MapFlags(int f) {
  uint32_t r = 0;
  if (f & FE_INEXACT) r |= FL_NX;
  if (f & FE_UNDERFLOW) r |= FL_UF;
  if (f & FE_OVERFLOW) r |= FL_OF;
  if (f & FE_DIVBYZERO) r |= FL_DZ;
  if (f & FE_INVALID) r |= FL_NV;
  return r;
}

template <typename T>
HostResult HostBinaryT(int op, T a, T b, int host_mode) {
  feclearexcept(FE_ALL_EXCEPT);
  fesetround(host_mode);
  volatile T va = a;
  volatile T vb = b;
  T r = va;
  switch (op) {
    case 0: r = static_cast<T>(va + vb); break;
    case 1: r = static_cast<T>(va - vb); break;
    case 2: r = static_cast<T>(va * vb); break;
    default: r = static_cast<T>(va / vb); break;
  }
  HostResult out;
  out.is_nan = std::isnan(r) != 0;
  if (sizeof(T) == 4) {
    const float v = r;
    out.bits = F32Bits(v);
  } else {
    const double v = r;
    out.bits = F64Bits(v);
  }
  out.flags = MapFlags(fetestexcept(FE_ALL_EXCEPT));
  fesetround(FE_TONEAREST);
  return out;
}

// op: 0 add, 1 sub, 2 mul, 3 div. `fmt` true selects single precision.
HostResult HostArith(int op, bool fmt, uint64_t a, uint64_t b, uint8_t rm) {
  const int hm = HostMode(rm);
  if (fmt) {
    return HostBinaryT<float>(op, F32From(static_cast<uint32_t>(a)),
                              F32From(static_cast<uint32_t>(b)), hm);
  }
  return HostBinaryT<double>(op, F64From(a), F64From(b), hm);
}

HostResult HostIntToFp(bool fmt, uint64_t value, bool iw, bool is_signed,
                       uint8_t rm) {
  feclearexcept(FE_ALL_EXCEPT);
  fesetround(HostMode(rm));
  volatile long long sv = iw ? static_cast<long long>(value)
                             : static_cast<long long>(static_cast<int32_t>(value));
  volatile unsigned long long uv = iw ? value : (value & 0xFFFFFFFFull);
  HostResult out;
  if (fmt) {
    volatile float r = is_signed ? static_cast<float>(sv) : static_cast<float>(uv);
    const float v = r;
    out.bits = F32Bits(v);
    out.is_nan = false;
  } else {
    volatile double r = is_signed ? static_cast<double>(sv) : static_cast<double>(uv);
    const double v = r;
    out.bits = F64Bits(v);
    out.is_nan = false;
  }
  out.flags = MapFlags(fetestexcept(FE_ALL_EXCEPT));
  fesetround(FE_TONEAREST);
  return out;
}

// The host's round-to-integer, used only to cross-check the model below on
// in-range signed results.
bool HostRoundToInt64(uint64_t bits, bool fmt, uint8_t rm, uint64_t* out,
                      uint32_t* flags) {
  feclearexcept(FE_ALL_EXCEPT);
  fesetround(HostMode(rm));
  long long r;
  if (fmt) {
    const float v = F32From(static_cast<uint32_t>(bits));
    r = llrintf(v);
  } else {
    const double v = F64From(bits);
    r = llrint(v);
  }
  const int f = fetestexcept(FE_ALL_EXCEPT);
  fesetround(FE_TONEAREST);
  *out = static_cast<uint64_t>(r);
  *flags = MapFlags(f);
  return true;
}

// ------------------------------------------------------- the fp->int model
// RISC-V FCVT: round to an integer in the current mode, saturate out of range
// or on NaN with NV -- and, as the RISC-V reference does, no NX alongside that
// NV -- and set NX when the in-range result is inexact.
struct Expect {
  uint64_t bits;
  uint32_t flags;
};

Expect ModelCvtFI(bool fmt, bool iw, bool is_signed, uint64_t bits, uint8_t rm) {
  const int fw = fmt ? 23 : 52;
  const int bias = fmt ? 127 : 1023;
  const uint64_t frac_mask = (1ull << fw) - 1u;
  const uint64_t frac = bits & frac_mask;
  const uint32_t exp_raw = fmt ? static_cast<uint32_t>((bits >> 23) & 0xFF)
                               : static_cast<uint32_t>((bits >> 52) & 0x7FF);
  const bool sign = fmt ? ((bits >> 31) & 1) != 0 : ((bits >> 63) & 1) != 0;
  const uint64_t smax = is_signed ? (iw ? 0x7FFF'FFFF'FFFF'FFFFull : 0x7FFF'FFFFull)
                                  : (iw ? 0xFFFF'FFFF'FFFF'FFFFull : 0xFFFF'FFFFull);
  const uint64_t smin = is_signed ? (iw ? 0x8000'0000'0000'0000ull : 0xFFFF'FFFF'8000'0000ull)
                                  : 0ull;
  Expect e;
  e.bits = 0;
  e.flags = 0;
  if (exp_raw == (fmt ? 255u : 2047u)) {
    if (frac != 0) {                       // NaN
      e.flags = FL_NV;
      e.bits = smax;
    } else {                               // infinity
      e.flags = FL_NV;
      e.bits = sign ? smin : smax;
    }
    return e;
  }
  if (exp_raw == 0 && frac == 0) return e;  // zero

  const unsigned __int128 m =
      (exp_raw == 0) ? static_cast<unsigned __int128>(frac)
                     : (static_cast<unsigned __int128>(1) << fw) | frac;
  const int e2 = (exp_raw == 0) ? (1 - bias - fw) : (static_cast<int>(exp_raw) - bias - fw);

  unsigned __int128 q = 0;
  bool g = false;
  bool rb = false;
  bool sb = false;
  bool inexact = false;
  if (e2 >= 0) {
    if (e2 > 70) {
      e.flags = FL_NV;
      e.bits = sign ? smin : smax;
      return e;
    }
    q = m << e2;
  } else {
    const int sh = -e2;
    if (sh > 130) {
      // Far below the last integer place: nothing survives but the knowledge
      // that something was there, which is what makes RUP/RDN round it to one.
      inexact = true;
      sb = true;
    } else {
      q = m >> sh;
      g = (sh >= 1) && (((m >> (sh - 1)) & 1u) != 0);
      rb = (sh >= 2) && (((m >> (sh - 2)) & 1u) != 0);
      if (sh >= 3) {
        const unsigned __int128 mask = (static_cast<unsigned __int128>(1) << (sh - 2)) - 1;
        sb = (m & mask) != 0;
      }
      inexact = g || rb || sb;
    }
  }
  bool inc = false;
  switch (rm) {
    case RM_RNE: inc = g && (rb || sb || ((q & 1) != 0)); break;
    case RM_RTZ: inc = false; break;
    case RM_RDN: inc = sign && inexact; break;
    case RM_RUP: inc = !sign && inexact; break;
    default:     inc = g; break;
  }
  if (inc) q += 1;

  const unsigned __int128 max_mag =
      is_signed ? (sign ? (static_cast<unsigned __int128>(1) << (iw ? 63 : 31))
                        : (static_cast<unsigned __int128>(1) << (iw ? 63 : 31)) - 1)
                : (static_cast<unsigned __int128>(1) << (iw ? 64 : 32)) - 1;
  if (!is_signed && sign) {
    e.flags = (q != 0) ? FL_NV : 0u;
    e.bits = 0;
    return e;
  }
  if (q > max_mag) {
    e.flags = FL_NV;
    e.bits = sign ? smin : smax;
    return e;
  }
  const uint64_t mag = static_cast<uint64_t>(q);
  if (sign) {
    const uint64_t v = (~mag) + 1ull;
    e.bits = iw ? v : static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v)));
  } else {
    e.bits = iw ? mag : mag;
  }
  e.flags = inexact ? FL_NX : 0u;
  return e;
}

// ------------------------------------------------------ non-arithmetic model
struct FpVal {
  bool sign;
  int exp_raw;
  uint64_t frac;
  int fw;
  bool is_zero() const { return exp_raw == 0 && frac == 0; }
  bool is_sub() const { return exp_raw == 0 && frac != 0; }
  bool is_inf() const { return exp_raw == (fw == 23 ? 255 : 2047) && frac == 0; }
  bool is_nan() const { return exp_raw == (fw == 23 ? 255 : 2047) && frac != 0; }
  bool is_snan() const { return is_nan() && (((frac >> (fw - 1)) & 1u) == 0); }
  // Magnitude comparison for finite non-zero values: (exponent, significand)
  // ordered lexicographically is the same as ordering the values.
  uint64_t sig() const { return (exp_raw == 0 ? 0ull : (1ull << fw)) | frac; }
};

FpVal Decode(bool fmt, uint64_t bits) {
  FpVal v;
  v.fw = fmt ? 23 : 52;
  if (fmt) {
    v.sign = ((bits >> 31) & 1) != 0;
    v.exp_raw = static_cast<int>((bits >> 23) & 0xFF);
    v.frac = bits & 0x7FFFFFull;
  } else {
    v.sign = ((bits >> 63) & 1) != 0;
    v.exp_raw = static_cast<int>((bits >> 52) & 0x7FF);
    v.frac = bits & 0xFFFFFFFFFFFFFull;
  }
  return v;
}

bool MagLt(const FpVal& a, const FpVal& b) {
  if (a.exp_raw != b.exp_raw) return a.exp_raw < b.exp_raw;
  return a.frac < b.frac;
}

bool LtVal(const FpVal& a, const FpVal& b) {
  if (a.is_inf() && b.is_inf()) return a.sign && !b.sign;
  if (a.is_inf()) return a.sign;
  if (b.is_inf()) return !b.sign;
  if (a.sign != b.sign) return a.sign;
  if (a.sign) return !MagLt(a, b);
  return MagLt(a, b);
}

bool EqVal(const FpVal& a, const FpVal& b) {
  if (a.is_inf() || b.is_inf()) return a.is_inf() && b.is_inf() && a.sign == b.sign;
  if (a.is_zero() || b.is_zero()) return a.is_zero() && b.is_zero();
  return a.sign == b.sign && a.exp_raw == b.exp_raw && a.frac == b.frac;
}

Expect ModelCompare(uint8_t kind, bool fmt, uint64_t abits, uint64_t bbits) {
  const FpVal a = Decode(fmt, abits);
  const FpVal b = Decode(fmt, bbits);
  Expect e;
  e.bits = 0;
  e.flags = 0;
  if (a.is_nan() || b.is_nan()) {
    e.flags = (kind == FP_CMP_EQ) ? ((a.is_snan() || b.is_snan()) ? FL_NV : 0u) : FL_NV;
    return e;
  }
  if (kind == FP_CMP_EQ) e.bits = EqVal(a, b) ? 1 : 0;
  else if (kind == FP_CMP_LT) e.bits = LtVal(a, b) ? 1 : 0;
  else e.bits = (LtVal(a, b) || EqVal(a, b)) ? 1 : 0;
  return e;
}

Expect ModelMinMax(bool is_min, bool fmt, uint64_t abits, uint64_t bbits) {
  const FpVal a = Decode(fmt, abits);
  const FpVal b = Decode(fmt, bbits);
  Expect e;
  e.flags = 0;
  // The single form's answer is a 32-bit value in the low half, zero-extended
  // exactly as the unit delivers it; the upper half of the operands is not part
  // of a single-precision value and is masked off here for the same reason.
  const uint64_t mask = fmt ? 0xFFFFFFFFull : ~0ull;
  if (a.is_nan() && b.is_nan()) {
    e.bits = CanonNaN(fmt);
    e.flags = (a.is_snan() || b.is_snan()) ? FL_NV : 0u;
    return e;
  }
  if (a.is_nan()) {
    e.bits = bbits & mask;
    e.flags = a.is_snan() ? FL_NV : 0u;
    return e;
  }
  if (b.is_nan()) {
    e.bits = abits & mask;
    e.flags = b.is_snan() ? FL_NV : 0u;
    return e;
  }
  if (a.is_zero() && b.is_zero()) {
    const bool sign = is_min ? (a.sign || b.sign) : (a.sign && b.sign);
    e.bits = fmt ? (static_cast<uint64_t>(sign) << 31) : (static_cast<uint64_t>(sign) << 63);
    return e;
  }
  const bool a_lt_b = LtVal(a, b);
  if (is_min) e.bits = (a_lt_b ? abits : bbits) & mask;
  else e.bits = (a_lt_b ? bbits : abits) & mask;
  return e;
}

Expect ModelClass(bool fmt, uint64_t bits) {
  const FpVal a = Decode(fmt, bits);
  Expect e;
  e.flags = 0;
  uint64_t mask = 0;
  if (a.is_inf() && a.sign) mask |= 1u << 0;
  if (a.sign && !a.is_zero() && !a.is_sub() && !a.is_inf() && !a.is_nan()) mask |= 1u << 1;
  if (a.sign && a.is_sub()) mask |= 1u << 2;
  if (a.sign && a.is_zero()) mask |= 1u << 3;
  if (!a.sign && a.is_zero()) mask |= 1u << 4;
  if (!a.sign && a.is_sub()) mask |= 1u << 5;
  if (!a.sign && !a.is_zero() && !a.is_sub() && !a.is_inf() && !a.is_nan()) mask |= 1u << 6;
  if (!a.sign && a.is_inf()) mask |= 1u << 7;
  if (a.is_nan() && a.is_snan()) mask |= 1u << 8;
  if (a.is_nan() && !a.is_snan()) mask |= 1u << 9;
  e.bits = mask;
  return e;
}

Expect ModelSgnj(uint8_t kind, bool fmt, uint64_t abits, uint64_t bbits) {
  const FpVal a = Decode(fmt, abits);
  const FpVal b = Decode(fmt, bbits);
  const bool sb = (kind == FP_SGNJX) ? (a.sign != b.sign)
                                     : (kind == FP_SGNJN ? !b.sign : b.sign);
  Expect e;
  e.flags = 0;
  e.bits = fmt ? ((abits & 0x7FFFFFFFull) | (static_cast<uint64_t>(sb) << 31))
               : ((abits & 0x7FFFFFFFFFFFFFFFull) | (static_cast<uint64_t>(sb) << 63));
  return e;
}

Expect ModelMove(bool fmt, uint64_t abits, bool to_int) {
  Expect e;
  e.flags = 0;
  if (to_int) {
    e.bits = fmt ? static_cast<uint64_t>(static_cast<int64_t>(
                       static_cast<int32_t>(abits & 0xFFFFFFFFull)))
                 : abits;
  } else {
    e.bits = fmt ? (abits & 0xFFFFFFFFull) : abits;
  }
  return e;
}

// ------------------------------------- exact dyadic arithmetic (tie proof)
// |v| = m * 2^e with no rounding anywhere. Used only to prove that a vector is
// an exact tie; it shares no code with the RTL and performs no rounding.
struct Dya {
  bool neg;
  unsigned __int128 m;
  int e;
};

Dya DyaFromFp(bool fmt, uint64_t bits) {
  const FpVal v = Decode(fmt, bits);
  Dya d;
  d.neg = v.sign;
  d.e = 0;
  d.m = 0;
  if (v.is_zero() || v.is_inf() || v.is_nan()) return d;
  const int bias = fmt ? 127 : 1023;
  if (v.exp_raw == 0) {
    d.m = v.frac;
    d.e = 1 - bias - v.fw;
  } else {
    d.m = (static_cast<unsigned __int128>(1) << v.fw) | v.frac;
    d.e = v.exp_raw - bias - v.fw;
  }
  return d;
}

Dya DyaAdd(const Dya& a, const Dya& b) {
  if (a.m == 0) return b;
  if (b.m == 0) return a;
  int e = a.e < b.e ? a.e : b.e;
  unsigned __int128 am = a.m;
  unsigned __int128 bm = b.m;
  const int sa = a.e - e;
  const int sb = b.e - e;
  if (sa > 120 || sb > 120) {   // not comparable within this representation
    Dya bad;
    bad.neg = false;
    bad.m = 0;
    bad.e = -100000;
    return bad;
  }
  am <<= sa;
  bm <<= sb;
  Dya r;
  r.e = e;
  if (a.neg == b.neg) {
    r.neg = a.neg;
    r.m = am + bm;
  } else if (am >= bm) {
    r.neg = a.neg;
    r.m = am - bm;
  } else {
    r.neg = b.neg;
    r.m = bm - am;
  }
  if (r.m == 0) r.neg = false;
  return r;
}

Dya DyaMul(const Dya& a, const Dya& b) {
  Dya r;
  r.neg = a.neg != b.neg;
  r.m = a.m * b.m;
  r.e = a.e + b.e;
  if (r.m == 0) r.neg = false;
  return r;
}

bool DyaEq(const Dya& a, const Dya& b) {
  if (a.m == 0 || b.m == 0) return a.m == 0 && b.m == 0;
  Dya x = a;
  Dya y = b;
  while ((x.m & 1u) == 0 && x.m != 0) { x.m >>= 1; ++x.e; }
  while ((y.m & 1u) == 0 && y.m != 0) { y.m >>= 1; ++y.e; }
  return x.neg == y.neg && x.m == y.m && x.e == y.e;
}

// The exact result of one of the four arithmetic operations, as a pair of
// dyadic values for a division (numerator/denominator) and a single value
// otherwise.
struct ExactOp {
  Dya value;
  Dya numer;
  Dya denom;
  bool is_div;
};

ExactOp ExactCompute(int op, bool fmt, uint64_t a, uint64_t b) {
  ExactOp x;
  x.is_div = (op == 3);
  const Dya da = DyaFromFp(fmt, a);
  const Dya db = DyaFromFp(fmt, b);
  if (op == 0) {
    x.value = DyaAdd(da, db);
  } else if (op == 1) {
    Dya nb = db;
    nb.neg = !nb.neg;
    x.value = DyaAdd(da, nb);
  } else if (op == 2) {
    x.value = DyaMul(da, db);
  } else {
    x.numer = da;
    x.denom = db;
    x.value.m = 0;
    x.value.e = 0;
    x.value.neg = false;
  }
  return x;
}

// Is the exact result exactly the midpoint of the two neighbours the host
// produces at RDN and RUP?
bool IsExactTie(int op, bool fmt, uint64_t a, uint64_t b) {
  const HostResult lo = HostArith(op, fmt, a, b, RM_RDN);
  const HostResult hi = HostArith(op, fmt, a, b, RM_RUP);
  if (lo.is_nan || hi.is_nan) return false;
  if (lo.bits == hi.bits) return false;             // exactly representable
  if (fmt) {
    const uint32_t l = static_cast<uint32_t>(lo.bits);
    const uint32_t h = static_cast<uint32_t>(hi.bits);
    const int32_t diff = static_cast<int32_t>(h) - static_cast<int32_t>(l);
    if (diff != 1 && diff != -1) return false;      // must be adjacent
  } else {
    const int64_t diff = static_cast<int64_t>(hi.bits) - static_cast<int64_t>(lo.bits);
    if (diff != 1 && diff != -1) return false;
  }
  const Dya mid = DyaAdd(DyaFromFp(fmt, lo.bits), DyaFromFp(fmt, hi.bits));
  const ExactOp ex = ExactCompute(op, fmt, a, b);
  if (!ex.is_div) {
    return DyaEq(DyaAdd(ex.value, ex.value), mid);
  }
  // a/b == mid/1  <=>  2*a == mid*b
  Dya two_a = ex.numer;
  two_a = DyaAdd(two_a, two_a);
  return DyaEq(two_a, DyaMul(mid, ex.denom));
}

// ------------------------------------------------------------------ identity
struct Ident {
  uint64_t rob_index = 0;
  uint64_t rob_gen = 0;
  uint64_t uop_index = 0;
  bool operator==(const Ident& o) const {
    return rob_index == o.rob_index && rob_gen == o.rob_gen && uop_index == o.uop_index;
  }
  std::string str() const {
    return "(rob=" + Dec(rob_index) + ", gen=" + Dec(rob_gen) + ", uop=" + Dec(uop_index) + ")";
  }
};

// ------------------------------------------------------------------ stimulus
struct Stim {
  bool req_valid = false;
  uint8_t op = FP_ADD;
  bool fmt = false;
  uint8_t rm = RM_RNE;
  bool iw = false;
  bool is = false;
  uint64_t a = 0;
  uint64_t b = 0;
  Ident id{};
  bool flush = false;
  bool res_ready = true;
};

struct Obs {
  uint64_t cycle = 0;
  bool req_ready = false;
  bool res_valid = false;
  uint64_t res_data = 0;
  uint32_t res_flags = 0;
  Ident res_id{};
  bool busy = false;
  uint8_t latency = 0;
  uint8_t iter = 0;
  uint32_t accepted = 0;
  uint32_t completed = 0;
  uint32_t cancelled = 0;
  uint32_t killed = 0;
};

// -------------------------------------------------------------------- harness
class Harness {
 public:
  Harness(Vmosaic_fpu_tb* dut, mosaic::ClockDriver* clk, mosaic::Reporter* reporter,
          uint64_t max_cycles)
      : dut_(dut), clk_(clk), reporter_(reporter), max_cycles_(max_cycles) {}

  void Phase(const std::string& name) { phase_ = name; }
  void Context(const std::string& text) { context_ = text; }
  uint64_t cycles() const { return cycles_; }

  void Masks(uint64_t rob, uint64_t gen, uint64_t uop) {
    rob_mask_ = rob;
    gen_mask_ = gen;
    uop_mask_ = uop;
  }
  Ident MaskId(const Ident& id) const {
    return Ident{id.rob_index & rob_mask_, id.rob_gen & gen_mask_,
                 id.uop_index & uop_mask_};
  }

  void Reset(int cycles) {
    for (int i = 0; i < cycles; ++i) Cycle(Stim{}, true);
  }

  Obs Cycle(const Stim& s, bool rst = false) {
    if (cycles_ >= max_cycles_) {
      Stop(phase_, "max-cycles exhausted before the campaign finished");
    }
    dut_->clk = 0;
    dut_->rst = rst ? 1 : 0;
    dut_->req_valid = s.req_valid ? 1 : 0;
    dut_->req_op = s.op & 0x1F;
    dut_->req_fmt = s.fmt ? 1 : 0;
    dut_->req_rm = s.rm & 0x7;
    dut_->req_iw = s.iw ? 1 : 0;
    dut_->req_is = s.is ? 1 : 0;
    dut_->req_a = s.a;
    dut_->req_b = s.b;
    dut_->req_rob_index = s.id.rob_index & rob_mask_;
    dut_->req_rob_gen = s.id.rob_gen & gen_mask_;
    dut_->req_uop_index = s.id.uop_index & uop_mask_;
    dut_->flush = s.flush ? 1 : 0;
    dut_->res_ready = s.res_ready ? 1 : 0;
    dut_->eval();

    Obs o;
    o.req_ready = dut_->req_ready != 0;
    o.res_valid = dut_->res_valid != 0;
    o.res_data = dut_->res_data;
    o.res_flags = static_cast<uint32_t>(dut_->res_fflags);
    o.res_id = Ident{dut_->res_rob_index, dut_->res_rob_gen, dut_->res_uop_index};

    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;

    o.cycle = clk_->cycle();
    o.busy = dut_->o_busy != 0;
    o.latency = static_cast<uint8_t>(dut_->o_latency);
    o.iter = static_cast<uint8_t>(dut_->o_iter);
    o.accepted = dut_->o_accepted_ctr;
    o.completed = dut_->o_completed_ctr;
    o.cancelled = dut_->o_cancelled_ctr;
    o.killed = dut_->o_killed_res_ctr;
    return o;
  }

  // Check the standing conservation invariant against the DUT's own counters.
  void CheckInvariant(const Obs& o, const std::string& where) {
    reporter_->Check(o.accepted == o.completed + o.cancelled + (o.busy ? 1u : 0u),
                     where + " conservation: accepted=" + Dec(o.accepted) +
                         " completed=" + Dec(o.completed) + " cancelled=" +
                         Dec(o.cancelled) + " busy=" + Bool(o.busy));
  }

 private:
  Vmosaic_fpu_tb* dut_;
  mosaic::ClockDriver* clk_;
  mosaic::Reporter* reporter_;
  uint64_t max_cycles_;
  uint64_t cycles_ = 0;
  uint64_t rob_mask_ = 0, gen_mask_ = 0, uop_mask_ = 0;
  std::string phase_;
  std::string context_;
};

// ------------------------------------------------------------------ issuing
struct Issued {
  uint64_t bits = 0;
  uint32_t flags = 0;
  uint64_t latency = 0;
  Ident id{};
};

class Dut {
 public:
  Dut(Harness* h, mosaic::Reporter* r) : h_(h), rep_(r) {}

  // Issue one operation and return its result. The identity is derived from a
  // counter so every operation is tagged differently: a unit that returned a
  // stale result would be caught by the tag even if the value happened to
  // match.
  Issued Issue(const std::string& name, uint8_t op, bool fmt, uint8_t rm, bool iw,
               bool is, uint64_t a, uint64_t b, bool check_latency = true) {
    Stim s;
    s.req_valid = true;
    s.op = op;
    s.fmt = fmt;
    s.rm = rm;
    s.iw = iw;
    s.is = is;
    s.a = a;
    s.b = b;
    s.id = NextId();
    s.res_ready = true;
    h_->Context(name);
    const Obs acc = h_->Cycle(s);
    if (!acc.req_ready) Stop(name, "request refused while the unit was idle");
    const uint64_t accept_cycle = acc.cycle;
    const uint8_t declared = acc.latency;
    for (int i = 0; i < 400; ++i) {
      Stim idle;
      idle.res_ready = true;
      const Obs o = h_->Cycle(idle);
      if (o.res_valid) {
        h_->CheckInvariant(o, name);
        Issued out;
        out.bits = o.res_data;
        out.flags = o.res_flags;
        out.latency = o.cycle - accept_cycle;
        out.id = o.res_id;
        if (check_latency) {
          rep_->Check(out.latency == declared,
                      name + " latency: measured " + Dec(out.latency) +
                          " cycles, declared " + Dec(declared));
        }
        rep_->Check(o.res_id == h_->MaskId(s.id),
                    name + " tag: expected " + s.id.str() + ", got " + o.res_id.str());
        return out;
      }
    }
    Stop(name, "no result within 400 cycles");
  }

  // Compare a result and its flags against an expectation, naming the check so
  // that a failure says which vector and which of the two failed.
  void Check(const std::string& name, const Issued& got, const Expect& exp) {
    const bool bits_ok = (got.bits == exp.bits);
    if (!bits_ok) {
      rep_->Mismatch(name + ".bits", HexU(exp.bits), HexU(got.bits));
    }
    rep_->Check(bits_ok, name + ".bits: expected " + HexU(exp.bits) + ", got " +
                             HexU(got.bits));
    const bool flags_ok = (got.flags == exp.flags);
    if (!flags_ok) {
      rep_->Mismatch(name + ".fflags", FlagStr(exp.flags), FlagStr(got.flags));
    }
    rep_->Check(flags_ok, name + ".fflags: expected " + FlagStr(exp.flags) +
                              ", got " + FlagStr(got.flags));
  }

  Ident NextId() {
    const uint64_t n = counter_++;
    return Ident{n & 0x3Full, (n * 5) & 0x7Full, n & 0x7ull};
  }

 private:
  Harness* h_;
  mosaic::Reporter* rep_;
  uint64_t counter_ = 1;
};

// --------------------------------------------------------------- op mapping
int HostOp(uint8_t op) {
  switch (op) {
    case FP_ADD: return 0;
    case FP_SUB: return 1;
    case FP_MUL: return 2;
    default: return 3;
  }
}

Expect ExpectArith(uint8_t op, bool fmt, uint64_t a, uint64_t b, uint8_t rm) {
  const HostResult h = HostArith(HostOp(op), fmt, a, b, rm);
  Expect e;
  e.bits = h.is_nan ? CanonNaN(fmt) : h.bits;
  e.flags = h.flags;
  return e;
}

// The oracle for one operation. The arithmetic goes to the host FPU, the
// conversions go to the host's conversion instruction, and the bit-level
// predicates go to the model written above from the ISA.
Expect OracleFor(uint8_t op, bool fmt, uint8_t rm, bool iw, bool is, uint64_t a,
                 uint64_t b) {
  switch (op) {
    case FP_ADD:
    case FP_SUB:
    case FP_MUL:
    case FP_DIV:
      return ExpectArith(op, fmt, a, b, rm);
    case FP_CMP_EQ:
    case FP_CMP_LT:
    case FP_CMP_LE:
      return ModelCompare(op, fmt, a, b);
    case FP_MIN:
      return ModelMinMax(true, fmt, a, b);
    case FP_MAX:
      return ModelMinMax(false, fmt, a, b);
    case FP_CLASS:
      return ModelClass(fmt, a);
    case FP_SGNJ:
    case FP_SGNJN:
    case FP_SGNJX:
      return ModelSgnj(op, fmt, a, b);
    case FP_MV_X:
      return ModelMove(fmt, a, true);
    case FP_MV_W:
      return ModelMove(fmt, a, false);
    case FP_CVT_FI:
      return ModelCvtFI(fmt, iw, is, a, rm);
    case FP_CVT_IF: {
      const HostResult r = HostIntToFp(fmt, a, iw, is, rm);
      Expect e;
      e.bits = r.bits;
      e.flags = r.flags;
      return e;
    }
    default: {
      // Format conversion: D->S rounds (the host does it under the current
      // mode), S->D is exact.
      feclearexcept(FE_ALL_EXCEPT);
      fesetround(HostMode(rm));
      Expect e;
      if (fmt) {   // S -> D
        volatile float f = F32From(a);
        volatile double d = f;
        e.bits = F64Bits(d);
      } else {     // D -> S
        volatile double d = F64From(a);
        volatile float f = static_cast<float>(d);
        e.bits = F32Bits(f);
      }
      e.flags = MapFlags(fetestexcept(FE_ALL_EXCEPT));
      fesetround(FE_TONEAREST);
      return e;
    }
  }
}

// ---------------------------------------------------------------- constants
const uint32_t S_PZERO = 0x00000000u, S_NZERO = 0x80000000u;
const uint32_t S_PINF = 0x7F800000u, S_NINF = 0xFF800000u;
const uint32_t S_QNAN = 0x7FC00000u, S_SNAN = 0x7F800001u;
const uint32_t S_MINSUB = 0x00000001u, S_MAXSUB = 0x007FFFFFu;
const uint32_t S_MINNORM = 0x00800000u, S_MAXFIN = 0x7F7FFFFFu;
const uint32_t S_ONE = 0x3F800000u, S_NEGONE = 0xBF800000u, S_TWO = 0x40000000u;
const uint32_t S_THREE = 0x40400000u, S_HALF = 0x3F000000u;
const uint32_t S_TIE = 0x33800000u;         // 2^-24
const uint32_t S_ONEP = 0x3F800001u;        // 1 + 2^-23
const uint32_t S_ROUND_TO_NORM_A = 0x3FFFFFFFu;   // (2^24-1) * 2^-24
const uint32_t S_ROUND_TO_NORM_B = 0x7E800000u;   // 2^126

const uint64_t D_PZERO = 0x0000000000000000ull, D_NZERO = 0x8000000000000000ull;
const uint64_t D_PINF = 0x7FF0000000000000ull, D_NINF = 0xFFF0000000000000ull;
const uint64_t D_QNAN = 0x7FF8000000000000ull, D_SNAN = 0x7FF0000000000001ull;
const uint64_t D_MINSUB = 0x0000000000000001ull, D_MAXSUB = 0x000FFFFFFFFFFFFFull;
const uint64_t D_MINNORM = 0x0010000000000000ull, D_MAXFIN = 0x7FEFFFFFFFFFFFFFull;
const uint64_t D_ONE = 0x3FF0000000000000ull, D_NEGONE = 0xBFF0000000000000ull;
const uint64_t D_TWO = 0x4000000000000000ull, D_THREE = 0x4008000000000000ull;
const uint64_t D_HALF = 0x3FE0000000000000ull, D_FOUR = 0x4010000000000000ull;
const uint64_t D_TIE = 0x3CA0000000000000ull;     // 2^-53
const uint64_t D_ONEP = 0x3FF0000000000001ull;    // 1 + 2^-52
const uint64_t D_ROUND_TO_NORM_A = 0x3FFFFFFFFFFFFFFFull;
const uint64_t D_ROUND_TO_NORM_B = 0x7FE0000000000000ull;

const uint16_t RM_HOST[4] = {RM_RNE, RM_RTZ, RM_RDN, RM_RUP};

struct AVec {
  const char* name;
  uint8_t op;
  bool fmt;
  uint64_t a;
  uint64_t b;
};

// --------------------------------------------------------------- the phases
void PhaseResetState(Harness* h, Dut* d, mosaic::Reporter* rep) {
  (void)d;
  h->Reset(4);
  const Obs o = h->Cycle(Stim{});
  rep->Check(o.req_ready, "reset-state: not ready after reset");
  rep->Check(!o.busy, "reset-state: busy after reset");
  rep->Check(!o.res_valid, "reset-state: a result is offered after reset");
  rep->Check(o.latency == 0, "reset-state: the latency declaration survived reset");
  rep->Check(o.accepted == 0 && o.completed == 0 && o.cancelled == 0 && o.killed == 0,
             "reset-state: the counters survived reset");

  // And the unit is usable straight afterwards.
  const Issued r = d->Issue("reset-state.follow-up", FP_ADD, false, RM_RNE, false,
                            false, D_ONE, D_ONE);
  Expect e;
  e.bits = D_TWO;
  e.flags = 0;
  d->Check("reset-state.follow-up", r, e);
}

// Every arithmetic operation at an exact tie, in five rounding modes. The
// expectation for RNE/RTZ/RDN/RUP is the host's; the expectation for RMM is
// *derived from the host* as the away-from-zero neighbour (RUP for a positive
// result, RDN for a negative one), and each vector is proved to be a tie by
// exact dyadic arithmetic before it is used.
void PhaseTies(Harness* h, Dut* d, mosaic::Reporter* rep) {
  struct TieVec {
    const char* name;
    uint8_t op;
    bool fmt;
    uint64_t a;
    uint64_t b;
  };
  const TieVec ties[] = {
      {"ties.d.add.pos", FP_ADD, false, D_ONE, D_TIE},
      {"ties.d.add.neg", FP_ADD, false, D_NEGONE, 0xBCA0000000000000ull},
      {"ties.d.sub.pos", FP_SUB, false, D_ONE, 0xBCA0000000000000ull},
      {"ties.d.mul.pos", FP_MUL, false, D_THREE, D_ONEP},
      {"ties.s.add.pos", FP_ADD, true, S_ONE, S_TIE},
      {"ties.s.add.neg", FP_ADD, true, S_NEGONE, 0xB3800000u},
      {"ties.s.sub.pos", FP_SUB, true, S_ONE, 0xB3800000u},
      {"ties.s.mul.pos", FP_MUL, true, S_THREE, S_ONEP},
  };
  uint64_t ties_rne_differs = 0;
  for (const TieVec& t : ties) {
    h->Context(t.name);
    const bool is_tie = IsExactTie(HostOp(t.op), t.fmt, t.a, t.b);
    rep->Check(is_tie, std::string("ties: ") + t.name +
                           " is not an exact tie under exact dyadic arithmetic");
    const HostResult away_r = HostArith(HostOp(t.op), t.fmt, t.a, t.b, RM_RDN);
    const HostResult away_u = HostArith(HostOp(t.op), t.fmt, t.a, t.b, RM_RUP);
    const bool negative = t.fmt ? (((away_u.bits >> 31) & 1) != 0)
                                : (((away_u.bits >> 63) & 1) != 0);
    for (uint8_t rm : RM_HOST) {
      const Expect e = ExpectArith(t.op, t.fmt, t.a, t.b, rm);
      const Issued got = d->Issue(std::string(t.name) + "." + RmName(rm), t.op,
                                  t.fmt, rm, false, false, t.a, t.b);
      d->Check(std::string(t.name) + "." + RmName(rm), got, e);
      if (rm == RM_RNE) {
        const uint64_t rmm_bits = negative ? away_r.bits : away_u.bits;
        if (rmm_bits != e.bits) ++ties_rne_differs;
      }
    }
    // RMM: the away-from-zero neighbour at a tie.
    const uint64_t rmm_bits = negative ? away_r.bits : away_u.bits;
    const uint32_t rmm_flags = negative ? away_r.flags : away_u.flags;
    Expect rmm;
    rmm.bits = rmm_bits;
    rmm.flags = rmm_flags;
    const Issued got = d->Issue(std::string(t.name) + ".rmm", t.op, t.fmt, RM_RMM,
                                false, false, t.a, t.b);
    d->Check(std::string(t.name) + ".rmm", got, rmm);
    // The ties-away rule must agree with the exact tie: RMM equals the
    // away-from-zero neighbour, which is one of the two neighbours the tie
    // proof produced.
    rep->Check(rmm_bits == away_u.bits || rmm_bits == away_r.bits,
               std::string("ties: ") + t.name + " rmm is not a neighbour");
  }
  rep->Check(ties_rne_differs > 0,
             "ties: no vector distinguishes RMM from RNE (" +
                 Dec(ties_rne_differs) + ")");

  // Division cannot produce an exact tie in a binary format, at any precision:
  // a tie needs the quotient's significand to be exactly a midpoint, which
  // needs an odd factor of 2^p in the divisor's significand, and a normal
  // significand in [2^(p-1), 2^p) has at most p-1 factors of two. The claim is
  // checked rather than asserted: every random division must fail the exact
  // tie test.
  mosaic::Rng rng(0xD1D1DE5ull);
  uint64_t checked = 0;
  for (int i = 0; i < 400; ++i) {
    const bool fmt = (i % 2) == 0;
    uint64_t a = rng.Next();
    uint64_t b = rng.Next();
    if (fmt) {
      a &= 0xFFFFFFFFull;
      b &= 0xFFFFFFFFull;
    }
    // Keep both operands finite and the divisor nonzero so the tie question is
    // about the arithmetic and not about a special case.
    const uint64_t exp_mask = fmt ? 0x7F800000ull : 0x7FF0000000000000ull;
    if ((a & exp_mask) == exp_mask) a &= ~exp_mask;
    if ((b & exp_mask) == exp_mask) b &= ~exp_mask;
    const uint64_t sign_bit = fmt ? 0x80000000ull : 0x8000000000000000ull;
    if ((b & ~sign_bit) == 0) b |= 1;
    if ((a & ~sign_bit) == 0) a |= 1;
    if (IsExactTie(HostOp(FP_DIV), fmt, a, b)) {
      rep->Check(false, "ties: a division was an exact tie, which the "
                        "significand argument says cannot happen");
    }
    ++checked;
  }
  rep->Check(checked == 400, "ties: division tie sweep ran " + Dec(checked));
}

void PhaseFlagsAndSubnormal(Harness* h, Dut* d, mosaic::Reporter* rep) {
  (void)rep;
  const AVec vecs[] = {
      // Subnormal operands and subnormal results, both formats.
      {"subnormal.d.add.minsub+zero", FP_ADD, false, D_MINSUB, D_PZERO},
      {"subnormal.d.add.minsub+minsub", FP_ADD, false, D_MINSUB, D_MINSUB},
      {"subnormal.d.add.maxsub+minsub", FP_ADD, false, D_MAXSUB, D_MINSUB},
      {"subnormal.d.add.minnorm+minsub", FP_ADD, false, D_MINNORM, D_MINSUB},
      {"subnormal.d.sub.minsub-minsub", FP_SUB, false, D_MINSUB, D_MINSUB},
      {"subnormal.d.sub.minnorm-maxsub", FP_SUB, false, D_MINNORM, D_MAXSUB},
      {"subnormal.d.sub.maxsub-minnorm", FP_SUB, false, D_MAXSUB, D_MINNORM},
      {"subnormal.d.mul.minsub*two", FP_MUL, false, D_MINSUB, D_TWO},
      {"subnormal.d.mul.minsub*minsub", FP_MUL, false, D_MINSUB, D_MINSUB},
      {"subnormal.d.mul.maxsub*half", FP_MUL, false, D_MAXSUB, D_HALF},
      {"subnormal.d.div.minsub/two", FP_DIV, false, D_MINSUB, D_TWO},
      {"subnormal.d.div.minsub/four", FP_DIV, false, D_MINSUB, D_FOUR},
      {"subnormal.d.div.maxsub/two", FP_DIV, false, D_MAXSUB, D_TWO},
      {"subnormal.d.div.minnorm/two", FP_DIV, false, D_MINNORM, D_TWO},
      {"subnormal.d.div.minnorm/three", FP_DIV, false, D_MINNORM, D_THREE},
      {"subnormal.d.div.rounds-to-normal", FP_DIV, false, D_ROUND_TO_NORM_A,
       D_ROUND_TO_NORM_B},
      {"subnormal.d.mul.maxfin*minsub", FP_MUL, false, D_MAXFIN, D_MINSUB},
      {"subnormal.s.add.minsub+minsub", FP_ADD, true, S_MINSUB, S_MINSUB},
      {"subnormal.s.add.maxsub+minsub", FP_ADD, true, S_MAXSUB, S_MINSUB},
      {"subnormal.s.sub.minnorm-maxsub", FP_SUB, true, S_MINNORM, S_MAXSUB},
      {"subnormal.s.mul.minsub*minsub", FP_MUL, true, S_MINSUB, S_MINSUB},
      {"subnormal.s.mul.maxsub*half", FP_MUL, true, S_MAXSUB, S_HALF},
      {"subnormal.s.div.minsub/two", FP_DIV, true, S_MINSUB, S_TWO},
      {"subnormal.s.div.maxsub/two", FP_DIV, true, S_MAXSUB, S_TWO},
      {"subnormal.s.div.rounds-to-normal", FP_DIV, true, S_ROUND_TO_NORM_A,
       S_ROUND_TO_NORM_B},
      // The five flags, each on the operation that must set it, and the
      // operations that must set nothing.
      {"fflags.dz.d.div.one/zero", FP_DIV, false, D_ONE, D_PZERO},
      {"fflags.dz.d.div.negone/zero", FP_DIV, false, D_NEGONE, D_PZERO},
      {"fflags.dz.d.div.one/negzero", FP_DIV, false, D_ONE, D_NZERO},
      {"fflags.dz.d.div.inf/zero", FP_DIV, false, D_PINF, D_PZERO},
      {"fflags.dz.s.div.one/zero", FP_DIV, true, S_ONE, S_PZERO},
      {"fflags.of.d.mul.maxfin*two", FP_MUL, false, D_MAXFIN, D_TWO},
      {"fflags.of.d.mul.maxfin*maxfin", FP_MUL, false, D_MAXFIN, D_MAXFIN},
      {"fflags.of.d.add.maxfin+maxfin", FP_ADD, false, D_MAXFIN, D_MAXFIN},
      {"fflags.of.d.sub.maxfin-negmaxfin", FP_SUB, false, D_MAXFIN, 0xFFEFFFFFFFFFFFFFull},
      {"fflags.of.s.mul.maxfin*two", FP_MUL, true, S_MAXFIN, S_TWO},
      {"fflags.uf.d.mul.minsub*minsub", FP_MUL, false, D_MINSUB, D_MINSUB},
      {"fflags.uf.d.div.minsub/three", FP_DIV, false, D_MINSUB, D_THREE},
      {"fflags.uf.s.mul.minsub*minsub", FP_MUL, true, S_MINSUB, S_MINSUB},
      {"fflags.nx.d.div.one/three", FP_DIV, false, D_ONE, D_THREE},
      {"fflags.nx.d.div.two/three", FP_DIV, false, D_TWO, D_THREE},
      {"fflags.nx.d.add.one+tie", FP_ADD, false, D_ONE, D_TIE},
      {"fflags.nx.s.div.one/three", FP_DIV, true, S_ONE, S_THREE},
      {"fflags.none.d.add.one+one", FP_ADD, false, D_ONE, D_ONE},
      {"fflags.none.d.mul.two*three", FP_MUL, false, D_TWO, D_THREE},
      {"fflags.none.d.div.six/three", FP_DIV, false, 0x4018000000000000ull, D_THREE},
      {"fflags.none.d.add.maxsub+minsub", FP_ADD, false, D_MAXSUB, D_MINSUB},
      {"fflags.none.s.div.six/three", FP_DIV, true, 0x40C00000u, S_THREE},
  };
  for (const AVec& v : vecs) {
    h->Context(v.name);
    for (uint8_t rm : RM_HOST) {
      const Expect e = ExpectArith(v.op, v.fmt, v.a, v.b, rm);
      const std::string name = std::string(v.name) + "." + RmName(rm);
      const Issued got = d->Issue(name, v.op, v.fmt, rm, false, false, v.a, v.b);
      d->Check(name, got, e);
    }
  }
}

void PhaseNaN(Harness* h, Dut* d, mosaic::Reporter* rep) {
  const AVec vecs[] = {
      {"nan.d.add.qnan+qnan", FP_ADD, false, D_QNAN, D_QNAN},
      {"nan.d.add.qnan+one", FP_ADD, false, D_QNAN, D_ONE},
      {"nan.d.add.snan+one", FP_ADD, false, D_SNAN, D_ONE},
      {"nan.d.add.one+snan", FP_ADD, false, D_ONE, D_SNAN},
      {"nan.d.sub.snan+snan", FP_SUB, false, D_SNAN, D_SNAN},
      {"nan.d.sub.inf-inf", FP_SUB, false, D_PINF, D_PINF},
      {"nan.d.sub.ninf-pinf", FP_SUB, false, D_NINF, D_PINF},
      {"nan.d.mul.zero*inf", FP_MUL, false, D_PZERO, D_PINF},
      {"nan.d.mul.inf*zero", FP_MUL, false, D_PINF, D_PZERO},
      {"nan.d.mul.snan*two", FP_MUL, false, D_SNAN, D_TWO},
      {"nan.d.div.zero/zero", FP_DIV, false, D_PZERO, D_PZERO},
      {"nan.d.div.inf/inf", FP_DIV, false, D_PINF, D_PINF},
      {"nan.d.div.snan/one", FP_DIV, false, D_SNAN, D_ONE},
      {"nan.s.add.qnan+one", FP_ADD, true, S_QNAN, S_ONE},
      {"nan.s.add.snan+one", FP_ADD, true, S_SNAN, S_ONE},
      {"nan.s.mul.zero*inf", FP_MUL, true, S_PZERO, S_PINF},
      {"nan.s.div.zero/zero", FP_DIV, true, S_PZERO, S_PZERO},
      // NaN policy beyond the arithmetic: the operations that must not
      // canonicalise, and the flag rule per operation.
      {"nan.d.min.qnan+one", FP_MIN, false, D_QNAN, D_ONE},
      {"nan.d.min.one+qnan", FP_MIN, false, D_ONE, D_QNAN},
      {"nan.d.min.snan+one", FP_MIN, false, D_SNAN, D_ONE},
      {"nan.d.min.qnan+qnan", FP_MIN, false, D_QNAN, D_QNAN},
      {"nan.d.max.qnan+one", FP_MAX, false, D_QNAN, D_ONE},
      {"nan.d.max.snan+snan", FP_MAX, false, D_SNAN, D_SNAN},
      {"nan.d.eq.qnan+qnan", FP_CMP_EQ, false, D_QNAN, D_QNAN},
      {"nan.d.eq.snan+one", FP_CMP_EQ, false, D_SNAN, D_ONE},
      {"nan.d.eq.one+snan", FP_CMP_EQ, false, D_ONE, D_SNAN},
      {"nan.d.lt.qnan+one", FP_CMP_LT, false, D_QNAN, D_ONE},
      {"nan.d.lt.snan+one", FP_CMP_LT, false, D_SNAN, D_ONE},
      {"nan.d.le.qnan+one", FP_CMP_LE, false, D_QNAN, D_ONE},
      {"nan.d.le.one+qnan", FP_CMP_LE, false, D_ONE, D_QNAN},
      {"nan.d.sgnj.qnan+one", FP_SGNJ, false, D_QNAN, D_ONE},
      {"nan.d.sgnjn.one+qnan", FP_SGNJN, false, D_ONE, D_QNAN},
      {"nan.d.sgnjx.qnan+qnan", FP_SGNJX, false, D_QNAN, D_QNAN},
      {"nan.d.mvx.qnan", FP_MV_X, false, D_QNAN, 0},
      {"nan.d.mvw.qnan", FP_MV_W, false, D_QNAN, 0},
      {"nan.d.class.qnan", FP_CLASS, false, D_QNAN, 0},
      {"nan.d.class.snan", FP_CLASS, false, D_SNAN, 0},
      {"nan.s.min.qnan+one", FP_MIN, true, S_QNAN, S_ONE},
      {"nan.s.lt.snan+one", FP_CMP_LT, true, S_SNAN, S_ONE},
      {"nan.s.sgnj.qnan+one", FP_SGNJ, true, S_QNAN, S_ONE},
      {"nan.s.class.snan", FP_CLASS, true, S_SNAN, 0},
  };
  for (const AVec& v : vecs) {
    h->Context(v.name);
    for (uint8_t rm : RM_HOST) {
      const Expect e = OracleFor(v.op, v.fmt, rm, false, false, v.a, v.b);
      const std::string name = std::string(v.name) + "." + RmName(rm);
      const Issued got = d->Issue(name, v.op, v.fmt, rm, false, false, v.a, v.b);
      d->Check(name, got, e);
      if (IsNaN64(e.bits, v.fmt)) {
        // The policy is the canonical quiet NaN, in both formats and for every
        // operation that produces a NaN. Stated as its own check so a
        // non-canonical payload is named as such.
        rep->Check(got.bits == CanonNaN(v.fmt),
                   name + " canonical NaN: got " + HexU(got.bits));
      }
    }
  }
}

void PhaseConversions(Harness* h, Dut* d, mosaic::Reporter* rep) {
  struct CVec {
    const char* name;
    uint8_t op;
    bool fmt;
    bool iw;
    bool is;
    uint64_t a;
    uint8_t rm;
  };
  const uint64_t S64 = 0xFFFFFFFFFFFFFFFFull;
  const CVec vecs[] = {
      // integer -> floating point
      {"conv.if.s.w.zero", FP_CVT_IF, true, false, true, 0, RM_RNE},
      {"conv.if.s.w.one", FP_CVT_IF, true, false, true, 1, RM_RNE},
      {"conv.if.s.w.minus1", FP_CVT_IF, true, false, true, S64, RM_RNE},
      {"conv.if.s.w.max", FP_CVT_IF, true, false, true, 0x7FFFFFFFull, RM_RNE},
      {"conv.if.s.w.min", FP_CVT_IF, true, false, true, 0x80000000ull, RM_RNE},
      {"conv.if.s.wu.max", FP_CVT_IF, true, false, false, 0xFFFFFFFFull, RM_RNE},
      {"conv.if.s.l.big", FP_CVT_IF, true, true, true, 0x7FFFFFFFFFFFFFFFull, RM_RNE},
      {"conv.if.s.l.big.rup", FP_CVT_IF, true, true, true, 0x7FFFFFFFFFFFFFFFull, RM_RUP},
      {"conv.if.s.l.big.rtz", FP_CVT_IF, true, true, true, 0x7FFFFFFFFFFFFFFFull, RM_RTZ},
      {"conv.if.s.l.32", FP_CVT_IF, true, true, true, 0x0000000080000000ull, RM_RNE},
      {"conv.if.s.lu.max", FP_CVT_IF, true, true, false, S64, RM_RNE},
      {"conv.if.d.lu.odd", FP_CVT_IF, false, true, false, 0xFFFFFFFFFFF00001ull, RM_RNE},
      {"conv.if.d.lu.odd.rup", FP_CVT_IF, false, true, false, 0xFFFFFFFFFFF00001ull, RM_RUP},
      {"conv.if.d.l.min", FP_CVT_IF, false, true, true, 0x8000000000000000ull, RM_RNE},
      {"conv.if.d.w.neg", FP_CVT_IF, false, false, true, 0xFFFFFFF0ull, RM_RNE},
      // floating point -> integer
      {"conv.fi.w.s.one", FP_CVT_FI, true, false, true, S_ONE, RM_RNE},
      {"conv.fi.w.s.minus1", FP_CVT_FI, true, false, true, S_NEGONE, RM_RNE},
      {"conv.fi.w.s.tie.half", FP_CVT_FI, true, false, true, S_HALF, RM_RNE},
      {"conv.fi.w.s.tie.half.rmm", FP_CVT_FI, true, false, true, S_HALF, RM_RMM},
      {"conv.fi.w.s.tie.half.rup", FP_CVT_FI, true, false, true, S_HALF, RM_RUP},
      {"conv.fi.w.s.tie.neg3half", FP_CVT_FI, true, false, true, 0xC0600000u, RM_RNE},
      {"conv.fi.w.s.tie.neg3half.rdn", FP_CVT_FI, true, false, true, 0xC0600000u, RM_RDN},
      {"conv.fi.w.s.inexact", FP_CVT_FI, true, false, true, 0x3FC00000u, RM_RNE},
      {"conv.fi.w.s.max", FP_CVT_FI, true, false, true, 0x4EFFFFFFu, RM_RNE},
      {"conv.fi.w.s.overflow", FP_CVT_FI, true, false, true, 0x4F000000u, RM_RNE},
      {"conv.fi.w.s.minoverflow", FP_CVT_FI, true, false, true, 0xCF000001u, RM_RNE},
      {"conv.fi.w.s.nan", FP_CVT_FI, true, false, true, S_QNAN, RM_RNE},
      {"conv.fi.w.s.snan", FP_CVT_FI, true, false, true, S_SNAN, RM_RNE},
      {"conv.fi.w.s.inf", FP_CVT_FI, true, false, true, S_PINF, RM_RNE},
      {"conv.fi.w.s.ninf", FP_CVT_FI, true, false, true, S_NINF, RM_RNE},
      {"conv.fi.w.s.minsub", FP_CVT_FI, true, false, true, S_MINSUB, RM_RNE},
      {"conv.fi.w.s.minsub.rup", FP_CVT_FI, true, false, true, S_MINSUB, RM_RUP},
      {"conv.fi.w.s.neg.minsub.rdn", FP_CVT_FI, true, false, true, 0x80000001u, RM_RDN},
      {"conv.fi.wu.s.neg", FP_CVT_FI, true, false, false, S_NEGONE, RM_RNE},
      {"conv.fi.wu.s.half", FP_CVT_FI, true, false, false, 0x3F000000u, RM_RNE},
      {"conv.fi.wu.s.max", FP_CVT_FI, true, false, false, 0x4F7FFFFFu, RM_RNE},
      {"conv.fi.wu.s.overflow", FP_CVT_FI, true, false, false, 0x4F800000u, RM_RNE},
      {"conv.fi.l.d.one", FP_CVT_FI, false, true, true, D_ONE, RM_RNE},
      {"conv.fi.l.d.tie.2p52.half", FP_CVT_FI, false, true, true, 0x4330000000000000ull, RM_RNE},
      {"conv.fi.l.d.max", FP_CVT_FI, false, true, true, 0x43DFFFFFFFFFFFFFull, RM_RNE},
      {"conv.fi.l.d.overflow", FP_CVT_FI, false, true, true, 0x43E0000000000000ull, RM_RNE},
      {"conv.fi.l.d.min", FP_CVT_FI, false, true, true, 0xC3E0000000000000ull, RM_RNE},
      {"conv.fi.l.d.minoverflow", FP_CVT_FI, false, true, true, 0xC3E0000000000001ull, RM_RNE},
      {"conv.fi.l.d.nan", FP_CVT_FI, false, true, true, D_QNAN, RM_RNE},
      {"conv.fi.lu.d.neg", FP_CVT_FI, false, true, false, D_NEGONE, RM_RNE},
      {"conv.fi.lu.d.overflow", FP_CVT_FI, false, true, false, 0x43F0000000000000ull, RM_RNE},
      {"conv.fi.lu.d.max", FP_CVT_FI, false, true, false, 0x43EFFFFFFFFFFFFFull, RM_RNE},
      {"conv.fi.w.d.sub", FP_CVT_FI, false, false, true, D_MINSUB, RM_RNE},
      {"conv.fi.l.d.sub", FP_CVT_FI, false, true, true, D_MINSUB, RM_RNE},
      // format conversion
      {"conv.fs.d.rounds", FP_CVT_FS, false, false, false, 0x3FF0000000000001ull, RM_RNE},
      {"conv.fs.d.overflow", FP_CVT_FS, false, false, false, 0x7FE0000000000000ull, RM_RNE},
      {"conv.fs.d.overflow.rtz", FP_CVT_FS, false, false, false, 0x7FE0000000000000ull, RM_RTZ},
      {"conv.fs.d.sub", FP_CVT_FS, false, false, false, D_MINSUB, RM_RNE},
      {"conv.fs.d.exact", FP_CVT_FS, false, false, false, D_ONE, RM_RNE},
      {"conv.sf.s.exact", FP_CVT_SF, true, false, false, S_ONEP, RM_RNE},
      {"conv.sf.s.sub", FP_CVT_SF, true, false, false, S_MINSUB, RM_RNE},
  };
  for (const CVec& v : vecs) {
    h->Context(v.name);
    const Expect e = OracleFor(v.op, v.fmt, v.rm, v.iw, v.is, v.a, 0);
    const Issued got = d->Issue(v.name, v.op, v.fmt, v.rm, v.iw, v.is, v.a, 0);
    d->Check(v.name, got, e);
    // The model's in-range signed answers are cross-checked against the host's
    // own round-to-integer, which rounds identically and reports NX the same
    // way. Out-of-range and NaN are excluded there because C leaves them
    // undefined; they are the vectors named above. RMM is excluded because this
    // host has no ties-away mode at all (FE_TONEARESTFROMZERO is undefined),
    // which is the same reason the arithmetic RMM coverage is a directed tie
    // table.
    if (v.op == FP_CVT_FI && v.is && v.rm != RM_RMM && !IsNaN64(v.a, v.fmt) &&
        !((v.a & (v.fmt ? 0x7F800000u : 0x7FF0000000000000ull)) ==
          (v.fmt ? 0x7F800000u : 0x7FF0000000000000ull)) &&
        !(e.flags & FL_NV)) {
      // Only the in-range answers are cross-checked: C leaves the out-of-range
      // and NaN results undefined, and those are the vectors named above,
      // adjudicated by the RISC-V saturation rule instead.
      uint64_t hv = 0;
      uint32_t hf = 0;
      HostRoundToInt64(v.a, v.fmt, v.rm, &hv, &hf);
      rep->Check(hv == e.bits && hf == e.flags,
                 std::string("conv cross-check: ") + v.name + " host llrint=" +
                     HexU(hv) + " flags=" + FlagStr(hf) + " model=" +
                     HexU(e.bits) + " flags=" + FlagStr(e.flags));
    }
  }

  // The S form reads only the low 32 bits: the same operation with a different
  // upper half must give the same answer.
  const uint64_t garbage[3] = {0xFFFFFFFF00000000ull, 0x7FF8000000000000ull,
                               0xDEADBEEF00000000ull};
  for (uint64_t hi : garbage) {
    for (const CVec& v : vecs) {
      if (!v.fmt || v.iw) continue;
      const Expect e = OracleFor(v.op, v.fmt, v.rm, v.iw, v.is, v.a, 0);
      const Issued got = d->Issue(std::string("upper-bits.") + v.name, v.op, v.fmt,
                                  v.rm, v.iw, v.is, v.a | hi, 0);
      d->Check(std::string("upper-bits.") + v.name, got, e);
    }
  }
}

void PhaseMisc(Harness* h, Dut* d, mosaic::Reporter* rep) {
  (void)h;
  (void)rep;
  const uint64_t class_ops[] = {D_PINF, D_NINF, D_MINNORM, D_ONE, D_MAXSUB,
                                D_MINSUB, D_PZERO, D_NZERO, D_QNAN, D_SNAN};
  const char* class_names[] = {"pinf", "ninf", "pnorm", "pnorm2", "submax",
                               "submin", "pzero", "nzero", "qnan", "snan"};
  for (size_t i = 0; i < sizeof(class_ops) / sizeof(class_ops[0]); ++i) {
    const std::string name = std::string("misc.class.d.") + class_names[i];
    const Expect e = ModelClass(false, class_ops[i]);
    d->Check(name, d->Issue(name, FP_CLASS, false, RM_RNE, false, false, class_ops[i], 0), e);
  }
  const uint32_t class_ops32[] = {S_PINF, S_NINF, S_MINNORM, S_MAXSUB, S_MINSUB,
                                  S_PZERO, S_NZERO, S_QNAN, S_SNAN, S_ONE};
  const char* class_names32[] = {"pinf", "ninf", "pnorm", "submax", "submin",
                                 "pzero", "nzero", "qnan", "snan", "one"};
  for (size_t i = 0; i < sizeof(class_ops32) / sizeof(class_ops32[0]); ++i) {
    const std::string name = std::string("misc.class.s.") + class_names32[i];
    const Expect e = ModelClass(true, class_ops32[i]);
    d->Check(name, d->Issue(name, FP_CLASS, true, RM_RNE, false, false, class_ops32[i], 0), e);
  }

  struct Pair {
    const char* name;
    uint64_t a;
    uint64_t b;
  };
  const Pair dpairs[] = {
      {"one_one", D_ONE, D_ONE}, {"one_two", D_ONE, D_TWO},
      {"two_one", D_TWO, D_ONE}, {"pzero_nzero", D_PZERO, D_NZERO},
      {"nzero_pzero", D_NZERO, D_PZERO}, {"inf_inf", D_PINF, D_PINF},
      {"inf_ninf", D_PINF, D_NINF}, {"one_inf", D_ONE, D_PINF},
      {"inf_one", D_PINF, D_ONE}, {"negone_one", D_NEGONE, D_ONE},
      {"sub_sub", D_MINSUB, 0x0000000000000002ull},
      {"sub_zero", D_MINSUB, D_PZERO}, {"pzero_nzero2", D_PZERO, D_NZERO},
  };
  const Pair spairs[] = {
      {"one_one", S_ONE, S_ONE}, {"one_two", S_ONE, S_TWO},
      {"pzero_nzero", S_PZERO, S_NZERO}, {"nzero_pzero", S_NZERO, S_PZERO},
      {"inf_ninf", S_PINF, S_NINF}, {"one_inf", S_ONE, S_PINF},
      {"inf_one", S_PINF, S_ONE}, {"negone_one", S_NEGONE, S_ONE},
      {"sub_sub", S_MINSUB, 0x00000002u},
  };
  const uint8_t cmps[3] = {FP_CMP_EQ, FP_CMP_LT, FP_CMP_LE};
  const uint8_t minmax[2] = {FP_MIN, FP_MAX};
  const uint8_t sgnjs[3] = {FP_SGNJ, FP_SGNJN, FP_SGNJX};
  for (const Pair& p : dpairs) {
    for (uint8_t op : cmps) {
      const std::string name = std::string("misc.") + OpName(op) + ".d." + p.name;
      const Expect e = OracleFor(op, false, RM_RNE, false, false, p.a, p.b);
      d->Check(name, d->Issue(name, op, false, RM_RNE, false, false, p.a, p.b), e);
    }
    for (uint8_t op : minmax) {
      const std::string name = std::string("misc.") + OpName(op) + ".d." + p.name;
      const Expect e = OracleFor(op, false, RM_RNE, false, false, p.a, p.b);
      d->Check(name, d->Issue(name, op, false, RM_RNE, false, false, p.a, p.b), e);
    }
    for (uint8_t op : sgnjs) {
      const std::string name = std::string("misc.") + OpName(op) + ".d." + p.name;
      const Expect e = OracleFor(op, false, RM_RNE, false, false, p.a, p.b);
      d->Check(name, d->Issue(name, op, false, RM_RNE, false, false, p.a, p.b), e);
    }
  }
  for (const Pair& p : spairs) {
    for (uint8_t op : cmps) {
      const std::string name = std::string("misc.") + OpName(op) + ".s." + p.name;
      const Expect e = OracleFor(op, true, RM_RNE, false, false, p.a, p.b);
      d->Check(name, d->Issue(name, op, true, RM_RNE, false, false, p.a, p.b), e);
    }
    for (uint8_t op : minmax) {
      const std::string name = std::string("misc.") + OpName(op) + ".s." + p.name;
      const Expect e = OracleFor(op, true, RM_RNE, false, false, p.a, p.b);
      d->Check(name, d->Issue(name, op, true, RM_RNE, false, false, p.a, p.b), e);
    }
  }

  // The moves.
  struct MV {
    const char* name;
    uint8_t op;
    bool fmt;
    uint64_t a;
  };
  const MV mvs[] = {
      {"misc.mvx.w", FP_MV_X, true, 0x80000001u},
      {"misc.mvx.w.qnan", FP_MV_X, true, S_QNAN},
      {"misc.mvw.w", FP_MV_W, true, 0xFFFFFFFF80000001ull},
      {"misc.mvx.d", FP_MV_X, false, 0x8000000000000001ull},
      {"misc.mvw.d", FP_MV_W, false, 0x0123456789ABCDEFull},
  };
  for (const MV& m : mvs) {
    const Expect e = OracleFor(m.op, m.fmt, RM_RNE, false, false, m.a, 0);
    d->Check(m.name, d->Issue(m.name, m.op, m.fmt, RM_RNE, false, false, m.a, 0), e);
  }

  // Reserved and dynamic rounding modes resolve to RNE in this unit: the
  // resolution itself is the CSR file's, and the unit states what it does with
  // a mode it should never receive rather than leaving it undefined.
  for (uint8_t rm = 5; rm <= 7; ++rm) {
    const Expect e = OracleFor(FP_ADD, false, RM_RNE, false, false, D_ONE, D_TIE);
    const std::string name = "misc.rm-reserved." + Dec(rm);
    const Issued got = d->Issue(name, FP_ADD, false, rm, false, false, D_ONE, D_TIE);
    d->Check(name, got, e);
  }
}

void PhaseLatency(Harness* h, Dut* d, mosaic::Reporter* rep) {
  (void)h;
  // The Issue() call already compares each measurement against the unit's own
  // `o_latency` declaration. This phase states the two contracts explicitly and
  // shows that the latency is a function of the operation class and not of the
  // operand values: a fast operand and a hard operand of the same class must
  // take the same number of cycles.
  struct LV {
    const char* name;
    uint8_t op;
    uint64_t a;
    uint64_t b;
  };
  const LV lv[] = {
      {"lat.add.tiny", FP_ADD, D_MINSUB, D_MINSUB},
      {"lat.add.huge", FP_ADD, D_MAXFIN, D_MAXFIN},
      {"lat.mul.normal", FP_MUL, D_ONE, D_ONE},
      {"lat.mul.denorm", FP_MUL, D_MINSUB, D_MINSUB},
      {"lat.div.easy", FP_DIV, D_FOUR, D_TWO},
      {"lat.div.hard", FP_DIV, D_THREE, D_THREE},
      {"lat.div.denorm", FP_DIV, D_MINSUB, D_THREE},
      {"lat.div.zero", FP_DIV, D_PZERO, D_TWO},
  };
  for (const LV& v : lv) {
    const std::string name = v.name;
    const Issued got = d->Issue(name, v.op, false, RM_RNE, false, false, v.a, v.b);
    rep->Check(got.latency == (v.op == FP_DIV ? 66u : 1u),
               name + ": measured " + Dec(got.latency) + " cycles");
  }
}

void PhaseBackpressure(Harness* h, Dut* d, mosaic::Reporter* rep) {
  // A result that is not taken must stay put: same value, same flags, same tag,
  // for as long as the consumer stalls.
  Ident id{7, 9, 3};
  Stim s;
  s.op = FP_DIV;
  s.fmt = false;
  s.rm = RM_RNE;
  s.a = D_ONE;
  s.b = D_THREE;
  s.id = id;
  s.res_ready = false;
  s.req_valid = true;
  const Obs acc = h->Cycle(s);
  rep->Check(acc.req_ready, "backpressure: refused a request in idle");
  uint64_t held = 0;
  uint64_t first_bits = 0;
  uint32_t first_flags = 0;
  bool seen = false;
  for (int i = 0; i < 100; ++i) {
    Stim stalled;
    stalled.res_ready = false;
    const Obs o = h->Cycle(stalled);
    if (o.res_valid) {
      if (!seen) {
        seen = true;
        first_bits = o.res_data;
        first_flags = o.res_flags;
        rep->Check(o.res_id == h->MaskId(id), "backpressure: wrong tag");
        const Expect e = ExpectArith(FP_DIV, false, D_ONE, D_THREE, RM_RNE);
        rep->Check(o.res_data == e.bits, "backpressure: wrong value");
        rep->Check(o.res_flags == e.flags, "backpressure: wrong flags");
      } else {
        rep->Check(o.res_data == first_bits, "backpressure: value changed while held");
        rep->Check(o.res_flags == first_flags, "backpressure: flags changed while held");
        rep->Check(o.res_id == h->MaskId(id), "backpressure: tag changed while held");
        ++held;
      }
    } else if (seen) {
      Stop("backpressure", "the result was withdrawn");
    }
  }
  rep->Check(seen, "backpressure: no result was offered");
  // The division declares 66 cycles, so of the 100 stalled cycles 34 hold the
  // result; the count is stated as the arithmetic rather than as a literal so a
  // latency change and a hold bug cannot cancel each other out silently.
  rep->Check(held == 100 - 66, "backpressure: held for " + Dec(held) + " cycles");
  // Take it, then prove the unit accepts again.
  Stim take;
  take.res_ready = true;
  const uint32_t completed_before = acc.completed;
  const Obs fin = h->Cycle(take);
  rep->Check(fin.completed == completed_before + 1,
             "backpressure: the result was not completed (" + Dec(fin.completed) +
                 " vs " + Dec(completed_before) + ")");
  const Issued after = d->Issue("backpressure.follow-up", FP_ADD, false, RM_RNE,
                                false, false, D_ONE, D_ONE);
  Expect e;
  e.bits = D_TWO;
  e.flags = 0;
  d->Check("backpressure.follow-up", after, e);
}

void PhaseFlush(Harness* h, Dut* d, mosaic::Reporter* rep) {
  // A division cancelled in flight.
  const uint32_t before = h->Cycle(Stim{}).cancelled;
  Stim s;
  s.op = FP_DIV;
  s.a = D_ONE;
  s.b = D_THREE;
  s.id = Ident{1, 1, 1};
  s.req_valid = true;
  const Obs acc = h->Cycle(s);
  rep->Check(acc.req_ready && acc.busy, "flush: the division was not accepted");
  Obs o{};
  for (int i = 0; i < 10; ++i) {
    Stim f;
    f.flush = true;
    o = h->Cycle(f);
  }
  rep->Check(!o.busy, "flush: still busy after the flush");
  rep->Check(!o.res_valid, "flush: a result appeared after the flush");
  rep->Check(o.cancelled == before + 1,
             "flush: cancelled counter did not move once (" + Dec(o.cancelled) + ")");
  // The cancelled operation's tag must never appear.
  const Issued after = d->Issue("flush.follow-up", FP_ADD, false, RM_RNE, false,
                                false, D_ONE, D_ONE);
  Expect e;
  e.bits = D_TWO;
  e.flags = 0;
  d->Check("flush.follow-up", after, e);

  // A *completed* result waiting to be taken, destroyed by a flush: the killed
  // case, counted separately from a partial operation.
  const Obs pre = h->Cycle(Stim{});
  Stim hold;
  hold.op = FP_ADD;
  hold.a = D_ONE;
  hold.b = D_ONE;
  hold.id = Ident{2, 2, 2};
  hold.req_valid = true;
  hold.res_ready = false;
  const Obs acc2 = h->Cycle(hold);
  rep->Check(acc2.req_ready, "flush: the second request was refused");
  Obs held{};
  for (int i = 0; i < 3; ++i) {
    Stim stalled;
    stalled.res_ready = false;
    held = h->Cycle(stalled);
  }
  rep->Check(held.res_valid, "flush: no result was held to kill");
  Stim f2;
  f2.flush = true;
  f2.res_ready = false;
  const Obs killed = h->Cycle(f2);
  rep->Check(!killed.res_valid, "flush: the held result survived the flush");
  rep->Check(killed.killed == pre.killed + 1,
             "flush: killed counter " + Dec(killed.killed));
  rep->Check(killed.cancelled == pre.cancelled + 1,
             "flush: cancelled counter " + Dec(killed.cancelled));
}

void PhaseUnimplemented(Harness* h, Dut* d, mosaic::Reporter* rep) {
  (void)h;
  (void)rep;
  // fsqrt is declared absent. It must not return a plausible number: the
  // declared answer is the canonical quiet NaN with NV set, so a later
  // integration can trap on it instead of computing with it.
  for (bool fmt : {false, true}) {
    for (uint8_t op : {static_cast<uint8_t>(FP_SQRT), static_cast<uint8_t>(20),
                       static_cast<uint8_t>(31)}) {
      const std::string name = "unimplemented.op" + Dec(op) + (fmt ? ".s" : ".d");
      const uint64_t a = fmt ? static_cast<uint64_t>(S_ONE) : D_ONE;
      const Issued got = d->Issue(name, op, fmt, RM_RNE, false, false, a, 0);
      Expect e;
      e.bits = CanonNaN(fmt);
      e.flags = FL_NV;
      d->Check(name, got, e);
    }
  }
}

void PhaseRandom(Harness* h, Dut* d, mosaic::Reporter* rep, uint32_t seed,
                 uint32_t count) {
  mosaic::Rng rng(seed ^ 0x5A5A5A5Au);
  uint64_t ops = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t roll = rng.Below(100);
    const bool fmt = rng.Chance(50);
    const uint8_t rm = static_cast<uint8_t>(rng.Below(4));
    const uint64_t a = rng.Next();
    const uint64_t b = rng.Next();
    uint8_t op;
    if (roll < 65) {
      op = static_cast<uint8_t>(FP_ADD + (rng.Below(3)));
    } else if (roll < 80) {
      op = FP_DIV;
    } else if (roll < 88) {
      op = FP_MIN + static_cast<uint8_t>(rng.Below(2));
    } else if (roll < 94) {
      op = FP_CMP_EQ + static_cast<uint8_t>(rng.Below(3));
    } else {
      op = FP_SGNJ + static_cast<uint8_t>(rng.Below(3));
    }
    const Expect e = OracleFor(op, fmt, rm, false, false, a, b);
    const std::string name = std::string("random.") + OpName(op) + "." + Dec(i);
    h->Context(name);
    const Issued got = d->Issue(name, op, fmt, rm, false, false, a, b, false);
    d->Check(name, got, e);
    ++ops;
  }
  rep->Check(ops == count, "random: issued " + Dec(ops) + " of " + Dec(count));
}

}  // namespace

int main(int argc, char** argv) {
  mosaic::Options options;
  std::string error;
  if (!mosaic::Options::Parse(argc, argv, &options, &error)) {
    std::fprintf(stderr,
                 "usage: %s --case <id> [--out <dir>] [--seed <n>] "
                 "[--max-cycles <n>] [--verbose]\n%s\n",
                 argv[0], error.c_str());
    return mosaic::kExitUsage;
  }
  Verilated::commandArgs(argc, argv);

  mosaic::Reporter reporter(options, std::string("Verilator ") +
                                        Verilated::productVersion());
  mosaic::ClockDriver clk;
  Vmosaic_fpu_tb dut;
  Harness harness(&dut, &clk, &reporter, options.max_cycles);
  Dut unit(&harness, &reporter);

  std::string detail;
  bool aborted = false;
  uint64_t divs = 0;
  try {
    harness.Reset(4);
    const uint32_t rob_w = dut.o_rob_index_w;
    const uint32_t gen_w = dut.o_rob_gen_w;
    const uint32_t uop_w = dut.o_uop_index_w;
    if (rob_w == 0 || rob_w > 32 || gen_w == 0 || gen_w > 32 || uop_w == 0 || uop_w > 32) {
      Stop("geometry", "the DUT reported an unusable identity width");
    }
    harness.Masks((1ull << rob_w) - 1ull, (1ull << gen_w) - 1ull, (1ull << uop_w) - 1ull);

    harness.Phase("reset-state");
    PhaseResetState(&harness, &unit, &reporter);

    harness.Phase("ties");
    PhaseTies(&harness, &unit, &reporter);

    harness.Phase("subnormal-and-flags");
    PhaseFlagsAndSubnormal(&harness, &unit, &reporter);

    harness.Phase("nan-policy");
    PhaseNaN(&harness, &unit, &reporter);

    harness.Phase("conversions");
    PhaseConversions(&harness, &unit, &reporter);

    harness.Phase("misc");
    PhaseMisc(&harness, &unit, &reporter);

    harness.Phase("latency");
    PhaseLatency(&harness, &unit, &reporter);

    harness.Phase("backpressure");
    PhaseBackpressure(&harness, &unit, &reporter);

    harness.Phase("flush");
    PhaseFlush(&harness, &unit, &reporter);

    harness.Phase("unimplemented");
    PhaseUnimplemented(&harness, &unit, &reporter);

    harness.Phase("random");
    PhaseRandom(&harness, &unit, &reporter, static_cast<uint32_t>(options.seed), 4000);

    const Obs fin = harness.Cycle(Stim{});
    reporter.Check(fin.accepted == fin.completed + fin.cancelled,
                   "final conservation: accepted=" + Dec(fin.accepted) +
                       " completed=" + Dec(fin.completed) + " cancelled=" +
                       Dec(fin.cancelled));
    divs = fin.accepted;
    detail = "operation matrix holds: " + Dec(fin.accepted) + " operations (" +
             Dec(divs) + " accepted, " + Dec(fin.completed) + " completed, " +
             Dec(fin.cancelled) + " cancelled), " + Dec(harness.cycles()) +
             " cycles, seed " + Dec(options.seed);
  } catch (const Abort& a) {
    reporter.Mismatch(a.what, "the campaign to finish", "it stopped");
    detail = "aborted: " + a.what;
    aborted = true;
  }

  const bool passed = !aborted && reporter.failures() == 0;
  dut.final();
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}

