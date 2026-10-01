// CASE=alu.boundaries -- work package I-011.
//
// Exercises every one of the sixteen `mosaic_pkg::alu_op_e` encodings of
// rtl/core/mosaic_alu.sv against an independent reference model, over the full
// 2x2 boundary matrix of the operand set in `kValues` x `kValues`, a dedicated
// shift-amount sweep, and a seeded random campaign over full-width operands.
//
// Independence of the reference model
// -----------------------------------
// The model below is written from the RV64I pseudo-code in int64_t/uint64_t (and
// uint32_t for the W forms). It shares no expression with the RTL: the RTL is
// bit-selected SV with `$signed()` casts, the model is host integer arithmetic,
// and the W sign-extension -- the single easiest thing to get wrong -- is done
// by an explicit function rather than by a cast.
//
// The three C++ expressions that silently compute the wrong thing here, and what
// this file does instead:
//
//   * `(int64_t)(uint32_t)(a + b)` is NOT addw. `a + b` is evaluated in 64 bits
//     before the truncation, so addw(0xffffffff80000000, 1) comes out as 0 in
//     this spelling and as 0x0000000080000000 in addw's real definition. The
//     model truncates first: `SignExtend32(aw + bw)`.
//   * `(uint64_t)(uint32_t)x` is not the W rule either -- that is zero-extension,
//     which is exactly what MOSAIC_ALU_MUTANT_1 injects into the RTL. Every W
//     answer in the model goes through `SignExtend32`, which fills bits 63:32
//     from bit 31 explicitly.
//   * `a >> n` is not `sra`. `>>>` on a signed type is implementation-defined
//     before C++20, so the model builds the sign fill explicitly with unsigned
//     shifts (`ArithmeticShiftRight*`); there is no signed right shift anywhere
//     in the reference.
//
// The signed comparison is additionally cross-checked at startup against a
// second formulation that never leaves unsigned arithmetic (`SignedLessByBias`),
// so the model's one implementation-defined conversion is not load bearing.
//
// Coverage is asserted, not assumed: a run that stops reaching an op, stops
// reaching a zero result, or stops reaching the operand/shift shapes that
// distinguish the five mutants fails instead of quietly passing over a shorter
// path. See `Bench::ReportCoverage`.

#include "sim_common.h"
#include "verilated.h"
#include "Vmosaic_alu_tb.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

// The `mosaic_pkg::alu_op_e` encodings, transcribed from rtl/core/mosaic_pkg.sv
// lines 49-66. I-010 verifies the same numbering from the RTL side; this file
// cannot import a SystemVerilog package, so the transcription is the one thing
// the two sides must agree on by inspection.
enum AluOp : uint8_t {
  kAdd = 0,
  kSub = 1,
  kSll = 2,
  kSlt = 3,
  kSltu = 4,
  kXor = 5,
  kSrl = 6,
  kSra = 7,
  kOr = 8,
  kAnd = 9,
  kAddw = 10,
  kSubw = 11,
  kSllw = 12,
  kSrlw = 13,
  kSraw = 14,
  kPassb = 15,
  kOpCount = 16,
};

const char* const kOpName[kOpCount] = {
    "add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra",
    "or",  "and", "addw", "subw", "sllw", "srlw", "sraw", "passb",
};

// The six ops whose `b` is a shift amount. They get the dedicated sweep, because
// they are the ops with a masking rule of their own.
const uint8_t kShiftOps[] = {kSll, kSrl, kSra, kSllw, kSrlw, kSraw};
constexpr size_t kShiftOpCount = sizeof(kShiftOps) / sizeof(kShiftOps[0]);

constexpr uint64_t kSignBit64 = 0x8000000000000000ull;

// Boundary operands. Every entry is a point where one of the sixteen ops, or the
// distinction between a signed and an unsigned reading of it, changes answer:
// zero and the small integers, the all-ones mask, -2, INT64_MIN/INT64_MAX, the
// values straddling bit 31 in all three possible extensions, the two 16-bit
// halves, alternating bit patterns, and pairs that differ only above bit 32.
const uint64_t kValues[] = {
    0x0000000000000000ull,  // 0
    0x0000000000000001ull,  // 1
    0x0000000000000002ull,  // 2
    0x0000000000000003ull,  // 3
    0xFFFFFFFFFFFFFFFFull,  // -1, all ones
    0xFFFFFFFFFFFFFFFEull,  // -2
    0x8000000000000000ull,  // INT64_MIN
    0x7FFFFFFFFFFFFFFFull,  // INT64_MAX
    0x000000007FFFFFFFull,  // 32-bit INT32_MAX, zero-extended
    0xFFFFFFFF7FFFFFFFull,  // the same 31-bit value, sign-extended
    0x0000000080000000ull,  // 32-bit INT32_MIN, zero-extended
    0xFFFFFFFF80000000ull,  // the same value, sign-extended
    0x00000000FFFFFFFFull,  // all ones in the low word only
    0xFFFFFFFF00000000ull,  // all ones above bit 32 only
    0x000000000000FFFFull,  // 16-bit mask, low half
    0x00000000FFFF0000ull,  // 16-bit mask, high half
    0xAAAAAAAAAAAAAAAAull,  // alternating bits, even
    0x5555555555555555ull,  // alternating bits, odd
    0x0000000100000000ull,  // differs from 0x00000000FFFFFFFF in bit 32 only
    0x0000000100000001ull,  // and the same plus a one in bit 0
};
constexpr size_t kValueCount = sizeof(kValues) / sizeof(kValues[0]);

// Shift amounts. 0 and 1 are the identity cases; 31 and 32 straddle the W
// boundary (31 is the largest W shift, 32 must wrap to 0); 63 is the largest
// 64-bit shift; 64, 65 and 127 wrap to 0, 1 and 63 respectively; and the
// all-ones mask masks to 63 for the 64-bit forms and to 31 for the W forms, so
// it separates the two masking rules by construction.
const uint64_t kShiftAmounts[] = {
    0, 1, 31, 32, 63, 64, 65, 127, 0xFFFFFFFFFFFFFFFFull,
};
constexpr size_t kShiftCount = sizeof(kShiftAmounts) / sizeof(kShiftAmounts[0]);

constexpr uint64_t kRandomVectors = 100000;
constexpr uint32_t kMaxFailures = 8;  // stop early so a mutant log stays readable

// ------------------------------------------------------------ reference model

// Sign-extend a 32-bit W answer into 64 bits. Written out bit by bit because the
// one-character spellings of this -- a cast through int32_t, a cast through
// uint64_t -- are respectively implementation-defined and wrong.
uint64_t SignExtend32(uint32_t value) {
  if ((value & 0x80000000u) == 0u) return static_cast<uint64_t>(value);
  return 0xFFFFFFFF00000000ull | static_cast<uint64_t>(value);
}

// Arithmetic (sign-replicating) right shift, expressed with unsigned operations
// only so that no implementation-defined signed shift enters the reference.
uint64_t ArithmeticShiftRight64(uint64_t value, unsigned amount) {
  if (amount == 0) return value;
  const uint64_t fill = 0xFFFFFFFFFFFFFFFFull << (64 - amount);
  if ((value >> 63) == 0) return value >> amount;
  return (value >> amount) | fill;
}

uint32_t ArithmeticShiftRight32(uint32_t value, unsigned amount) {
  if (amount == 0) return value;
  const uint32_t fill = 0xFFFFFFFFu << (32 - amount);
  if ((value >> 31) == 0) return value >> amount;
  return (value >> amount) | fill;
}

// `slt`, from the signed reading of the same two wires.
bool SignedLess(uint64_t a, uint64_t b) {
  return static_cast<int64_t>(a) < static_cast<int64_t>(b);
}

// The same predicate with no signed arithmetic at all: flipping the sign bit of
// both operands turns the signed order into the unsigned order. Used only to
// cross-check SignedLess, never as the primary formulation.
bool SignedLessByBias(uint64_t a, uint64_t b) {
  return (a ^ kSignBit64) < (b ^ kSignBit64);
}

uint64_t ReferenceAlu(uint8_t op, uint64_t a, uint64_t b) {
  const uint64_t ua = a;                         // the unsigned reading
  const uint64_t ub = b;
  const uint32_t aw = static_cast<uint32_t>(a);  // the W readings
  const uint32_t bw = static_cast<uint32_t>(b);

  switch (op) {
    case kAdd:
      return ua + ub;
    case kSub:
      return ua - ub;
    case kSll:
      // RV64 masks the shift amount to 6 bits: a shift of 64 is a shift by 0.
      return ua << (ub & 63);
    case kSlt:
      return SignedLess(ua, ub) ? 1ull : 0ull;
    case kSltu:
      // Unsigned: 0xffff... is the largest value there is, so -1 < 1 is false.
      return (ua < ub) ? 1ull : 0ull;
    case kXor:
      return ua ^ ub;
    case kSrl:
      return ua >> (ub & 63);
    case kSra:
      return ArithmeticShiftRight64(ua, static_cast<unsigned>(ub & 63));
    case kOr:
      return ua | ub;
    case kAnd:
      return ua & ub;
    case kAddw:
      // Truncate to 32 bits first, then sign-extend the 32-bit answer.
      return SignExtend32(aw + bw);
    case kSubw:
      return SignExtend32(aw - bw);
    case kSllw:
      // RV64 W shifts mask to 5 bits: a shift of 32 is a shift by 0, not 0.
      return SignExtend32(aw << (bw & 31));
    case kSrlw:
      return SignExtend32(aw >> (bw & 31));
    case kSraw:
      return SignExtend32(
          ArithmeticShiftRight32(aw, static_cast<unsigned>(bw & 31)));
    case kPassb:
      return ub;
    default:
      break;
  }
  return 0xDEADBEEFDEADBEEFull;  // unreachable: the op field is 4 bits wide
}

// ------------------------------------------------------------------ the bench

class Bench {
 public:
  Bench(const mosaic::Options& options, mosaic::Reporter* reporter,
        Vmosaic_alu_tb* top)
      : options_(options), rep_(reporter), top_(top) {}

  // One stimulus vector against the model. The DUT is combinational, so a single
  // eval() settles it and there is nothing to wait for.
  void Apply(uint64_t a, uint64_t b, uint8_t op, const char* phase) {
    if (halted_) return;
    top_->a = a;
    top_->b = b;
    top_->op = op;
    top_->eval();

    const uint64_t got_result = top_->result;
    const bool got_zero = top_->zero != 0;
    const uint64_t want_result = ReferenceAlu(op, a, b);
    const bool want_zero = want_result == 0;

    ++vectors_;
    ++per_op_[op];
    if (want_result == 0) {
      ++zero_result_ops_[op];
    } else {
      ++nonzero_result_ops_[op];
    }
    if (a == 0 && b == 0) ++both_zero_ops_[op];
    NoteShape(op, a, b);

    char where[192];
    std::snprintf(where, sizeof(where), "%s %s(a=%s, b=%s) result", phase,
                  kOpName[op], mosaic::Hex(a).c_str(), mosaic::Hex(b).c_str());
    if (got_result != want_result) {
      rep_->Mismatch(where, mosaic::Hex(want_result), mosaic::Hex(got_result));
      Fail(where);
    }
    rep_->Check(got_result == want_result, where);

    std::snprintf(where, sizeof(where), "%s %s(a=%s, b=%s) zero flag", phase,
                  kOpName[op], mosaic::Hex(a).c_str(), mosaic::Hex(b).c_str());
    if (got_zero != want_zero) {
      rep_->Mismatch(where, want_zero ? "1" : "0", got_zero ? "1" : "0");
      Fail(where);
    }
    rep_->Check(got_zero == want_zero, where);
  }

  // A hand-computed expectation, checked against the RTL directly rather than
  // through the model, so a shared mistake in the model cannot make the two
  // agree with each other for the wrong reason.
  void Expect(uint64_t a, uint64_t b, uint8_t op, uint64_t want_result,
              bool want_zero, const char* what) {
    top_->a = a;
    top_->b = b;
    top_->op = op;
    top_->eval();

    const uint64_t got_result = top_->result;
    const bool got_zero = top_->zero != 0;
    if (got_result != want_result) {
      rep_->Mismatch(what, mosaic::Hex(want_result), mosaic::Hex(got_result));
      Fail(what);
    }
    rep_->Check(got_result == want_result, std::string(what) + ": result");
    if (got_zero != want_zero) {
      rep_->Mismatch(what, want_zero ? "zero=1" : "zero=0",
                     got_zero ? "zero=1" : "zero=0");
      Fail(what);
    }
    rep_->Check(got_zero == want_zero, std::string(what) + ": zero flag");
    ++handwritten_;
  }

  // Cross-check the reference against a second formulation of its one
  // implementation-defined step, and prove every op is reachable through the
  // switch rather than falling through to the unreachable sentinel.
  void CheckModelSelfConsistency() {
    bool consistent = true;
    for (size_t i = 0; i < kValueCount; ++i) {
      for (size_t j = 0; j < kValueCount; ++j) {
        if (SignedLess(kValues[i], kValues[j]) !=
            SignedLessByBias(kValues[i], kValues[j])) {
          consistent = false;
          rep_->Mismatch("slt reference cross-check",
                         "the same answer signed and unsigned-biased",
                         mosaic::Hex(kValues[i]) + " vs " + mosaic::Hex(kValues[j]));
        }
      }
    }
    for (uint8_t op = 0; op < kOpCount; ++op) {
      const uint64_t probe =
          ReferenceAlu(op, 0x0123456789ABCDEFull, 0x1111111111111111ull);
      if (probe == 0xDEADBEEFDEADBEEFull) {
        consistent = false;
        rep_->Mismatch("reference model", "every op implemented",
                       std::string(kOpName[op]) + " falls through to the sentinel");
      }
    }
    rep_->Check(consistent, "reference model cross-checks against itself");
  }

  void ReportCoverage() {
    if (halted_) {
      // The run stopped at the failure limit, so the coverage facts below would
      // report the truncation rather than anything about the stimulus.
      std::fprintf(stderr,
                   "coverage: not asserted, the run halted at the failure limit\n");
      return;
    }
    for (uint8_t op = 0; op < kOpCount; ++op) {
      rep_->Check(per_op_[op] > 0,
                  std::string("coverage: op ") + kOpName[op] + " was exercised");
      rep_->Check(zero_result_ops_[op] > 0,
                  std::string("coverage: op ") + kOpName[op] +
                      " produced a zero result");
      rep_->Check(nonzero_result_ops_[op] > 0,
                  std::string("coverage: op ") + kOpName[op] +
                      " produced a non-zero result");
      rep_->Check(both_zero_ops_[op] > 0, std::string("coverage: op ") +
                                              kOpName[op] +
                                              " was seen at a=b=0");
    }

    // The operand shapes that separate the five negative controls. If one of
    // these drops out of the stimulus the case fails, because a run that no
    // longer distinguishes signed from unsigned, 5-bit from 6-bit shift
    // masking, or arithmetic from logical shift is no longer testing anything.
    rep_->Check(saw_slt_negative_, "coverage: slt saw -1 against 1");
    rep_->Check(saw_sltu_negative_, "coverage: sltu saw -1 against 1");
    rep_->Check(saw_sra_negative_,
                "coverage: sra saw an operand with bit 63 set");
    rep_->Check(saw_sllw_amount_32_,
                "coverage: sllw saw a shift amount that masks to 32");
    rep_->Check(saw_sll_amount_64_,
                "coverage: sll saw a shift amount that masks to 0 but is not 0");
    rep_->Check(saw_addw_sign_,
                "coverage: addw produced a sign-extended negative word result");
    rep_->Check(saw_sraw_sign_,
                "coverage: sraw shifted an operand with bit 31 set");
    rep_->Check(saw_zero_from_nonzero_a_,
                "coverage: some op produced zero from a non-zero a");
    rep_->Check(vectors_ > 0, "coverage: stimulus was applied");
  }

  uint64_t vectors() const { return vectors_; }
  uint64_t handwritten() const { return handwritten_; }
  uint32_t failures() const { return failures_; }
  bool halted() const { return halted_; }
  bool out_of_budget() const { return vectors_ >= options_.max_cycles; }

  std::string CoverageSummary() const {
    std::string text;
    for (uint8_t op = 0; op < kOpCount; ++op) {
      if (op != 0) text += " ";
      text += std::string(kOpName[op]) + "=" + std::to_string(per_op_[op]);
    }
    return text;
  }

 private:
  void Fail(const std::string& what) {
    ++failures_;
    if (failures_ >= kMaxFailures && !halted_) {
      halted_ = true;
      std::fprintf(stderr,
                   "ABORT: %u checks failed (last: %s); stopping to keep the "
                   "log readable\n",
                   failures_, what.c_str());
    }
  }

  void NoteShape(uint8_t op, uint64_t a, uint64_t b) {
    const uint64_t result = ReferenceAlu(op, a, b);
    if (op == kSlt && a == 0xFFFFFFFFFFFFFFFFull && b == 1) saw_slt_negative_ = true;
    if (op == kSltu && a == 0xFFFFFFFFFFFFFFFFull && b == 1) {
      saw_sltu_negative_ = true;
    }
    if (op == kSra && (a >> 63) != 0) saw_sra_negative_ = true;
    if (op == kSllw && (b & 63) == 32) saw_sllw_amount_32_ = true;
    if (op == kSll && (b & 63) == 0 && b != 0) saw_sll_amount_64_ = true;
    if (op == kAddw && (result & 0xFFFFFFFF80000000ull) == 0xFFFFFFFF80000000ull) {
      saw_addw_sign_ = true;
    }
    if (op == kSraw && (a & 0x80000000ull) != 0) saw_sraw_sign_ = true;
    if (result == 0 && a != 0) saw_zero_from_nonzero_a_ = true;
  }

  mosaic::Options options_;
  mosaic::Reporter* rep_;
  Vmosaic_alu_tb* top_;

  uint64_t vectors_ = 0;
  uint64_t handwritten_ = 0;
  uint32_t failures_ = 0;
  bool halted_ = false;

  uint64_t per_op_[kOpCount] = {};
  uint32_t zero_result_ops_[kOpCount] = {};
  uint32_t nonzero_result_ops_[kOpCount] = {};
  uint32_t both_zero_ops_[kOpCount] = {};

  bool saw_slt_negative_ = false;
  bool saw_sltu_negative_ = false;
  bool saw_sra_negative_ = false;
  bool saw_sllw_amount_32_ = false;
  bool saw_sll_amount_64_ = false;
  bool saw_addw_sign_ = false;
  bool saw_sraw_sign_ = false;
  bool saw_zero_from_nonzero_a_ = false;
};

}  // namespace

int main(int argc, char** argv) {
  mosaic::Options opt;
  std::string error;
  if (!mosaic::Options::Parse(argc, argv, &opt, &error)) {
    std::fprintf(stderr,
                 "usage: %s --case <id> [--out <dir>] [--seed <n>] "
                 "[--max-cycles <n>] [--verbose]\n%s\n",
                 argv[0], error.c_str());
    return mosaic::kExitUsage;
  }

  Verilated::commandArgs(argc, argv);
  mosaic::Reporter rep(opt,
                       std::string("Verilator ") + Verilated::productVersion());
  Vmosaic_alu_tb* top = new Vmosaic_alu_tb;
  Bench bench(opt, &rep, top);

  // ---- the reference model agrees with itself -------------------------------
  bench.CheckModelSelfConsistency();

  // ---- hand-written invariants, straight from the RV64I pseudo-code ----------
  // These do not go through the model at all.
  {
    const uint64_t kMinusOne = 0xFFFFFFFFFFFFFFFFull;
    bench.Expect(0, 0, kAdd, 0, true, "add(0,0) is zero and sets zero");
    bench.Expect(1, kMinusOne, kAdd, 0, true, "add(1,-1) is zero while a != 0");
    bench.Expect(0x7FFFFFFFFFFFFFFFull, 1, kAdd, 0x8000000000000000ull, false,
                 "add(INT64_MAX,1) wraps to INT64_MIN");
    bench.Expect(0, 1, kSub, kMinusOne, false, "sub(0,1) is -1");
    bench.Expect(kMinusOne, 1, kSlt, 1, false, "slt(-1,1) is 1, signed");
    bench.Expect(kMinusOne, 1, kSltu, 0, true,
                 "sltu(-1,1) is 0, -1 is the largest unsigned value");
    bench.Expect(0x8000000000000000ull, 1, kSlt, 1, false,
                 "slt(INT64_MIN,1) is 1");
    bench.Expect(0x8000000000000000ull, 1, kSltu, 0, true,
                 "sltu(INT64_MIN,1) is 0");
    bench.Expect(0x8000000000000000ull, 1, kSra, 0xC000000000000000ull, false,
                 "sra(INT64_MIN,1) replicates the sign bit");
    bench.Expect(0x8000000000000000ull, 1, kSrl, 0x4000000000000000ull, false,
                 "srl(INT64_MIN,1) shifts a zero in");
    bench.Expect(0x123456789ABCDEF1ull, 64, kSll, 0x123456789ABCDEF1ull, false,
                 "sll by 64 is a shift by 0");
    bench.Expect(0x123456789ABCDEF1ull, kMinusOne, kSll, 0x8000000000000000ull,
                 false, "sll by the all-ones mask is a shift by 63");
    bench.Expect(kMinusOne, 0x0000000080000020ull, kSll, 0xFFFFFFFF00000000ull,
                 false, "sll masks the shift amount to six bits, ignoring bit 31");
    bench.Expect(0x000000007FFFFFFFull, 1, kAddw, 0xFFFFFFFF80000000ull, false,
                 "addw sign-extends its 32-bit answer");
    bench.Expect(0xFFFFFFFF80000000ull, 0, kAddw, 0xFFFFFFFF80000000ull, false,
                 "addw ignores the bits above bit 31 of its operands");
    bench.Expect(kMinusOne, 1, kSubw, 0xFFFFFFFFFFFFFFFEull, false,
                 "subw wraps at 32 bits and sign-extends");
    bench.Expect(kMinusOne, 32, kSllw, kMinusOne, false,
                 "sllw by 32 is a shift by 0, not a shift to zero");
    bench.Expect(kMinusOne, kMinusOne, kSllw, 0xFFFFFFFF80000000ull, false,
                 "sllw by the all-ones mask is a shift by 31");
    bench.Expect(0xFFFFFFFF80000000ull, 1, kSraw, 0xFFFFFFFFC0000000ull, false,
                 "sraw replicates bit 31 of its operand");
    bench.Expect(0xFFFFFFFF80000000ull, 1, kSrlw, 0x0000000040000000ull, false,
                 "srlw shifts a zero in above bit 31");
    bench.Expect(0xFFFFFFFF80000000ull, 1, kAddw, 0xFFFFFFFF80000001ull, false,
                 "addw of a sign-extended operand ignores the upper bits");
    bench.Expect(0x0F0F0F0F0F0F0F0Full, 0xF0F0F0F0F0F0F0F0ull, kOr,
                 0xFFFFFFFFFFFFFFFFull, false, "or of complementary patterns");
    bench.Expect(0x0F0F0F0F0F0F0F0Full, 0xF0F0F0F0F0F0F0F0ull, kAnd, 0, true,
                 "and of complementary patterns is zero");
    bench.Expect(0x0F0F0F0F0F0F0F0Full, 0xF0F0F0F0F0F0F0F0ull, kXor,
                 0xFFFFFFFFFFFFFFFFull, false, "xor of complementary patterns");
    bench.Expect(0xDEADBEEFCAFEBABEull, 0x0123456789ABCDEFull, kPassb,
                 0x0123456789ABCDEFull, false, "passb returns b unchanged");
  }

  // ---- the full 2x2 operand matrix, every op -------------------------------
  for (uint8_t op = 0;
       op < kOpCount && !bench.halted() && !bench.out_of_budget(); ++op) {
    for (size_t i = 0;
         i < kValueCount && !bench.halted() && !bench.out_of_budget(); ++i) {
      for (size_t j = 0;
           j < kValueCount && !bench.halted() && !bench.out_of_budget(); ++j) {
        bench.Apply(kValues[i], kValues[j], op, "matrix");
      }
    }
  }

  // ---- the shift-amount sweep ----------------------------------------------
  // Every boundary operand as the shifted value against every shift amount,
  // which is where the 5-bit and 6-bit masking rules separate.
  for (size_t s = 0;
       s < kShiftOpCount && !bench.halted() && !bench.out_of_budget(); ++s) {
    const uint8_t op = kShiftOps[s];
    for (size_t i = 0;
         i < kValueCount && !bench.halted() && !bench.out_of_budget(); ++i) {
      for (size_t k = 0;
           k < kShiftCount && !bench.halted() && !bench.out_of_budget(); ++k) {
        bench.Apply(kValues[i], kShiftAmounts[k], op, "shift-sweep");
        // ...and the operand order the other way round, so an implementation
        // that only looks at one side of the ALU cannot slip past.
        bench.Apply(kShiftAmounts[k], kValues[i], op, "shift-sweep-rev");
      }
    }
  }

  // ---- seeded random campaign over full-width operands ----------------------
  mosaic::Rng rng(opt.seed);
  for (uint64_t n = 0;
       n < kRandomVectors && !bench.halted() && !bench.out_of_budget(); ++n) {
    uint64_t a = rng.Next();
    uint64_t b = rng.Next();
    uint8_t op = static_cast<uint8_t>(rng.Below(kOpCount));

    // Bias half the vectors onto the shapes that matter. Uniform noise almost
    // never produces b == 32, an all-ones shift amount, or two operands that
    // differ only above bit 32.
    const uint32_t shape = rng.Below(100);
    if (shape < 15) {
      a = kValues[rng.Below(static_cast<uint32_t>(kValueCount))];
    } else if (shape < 30) {
      b = kValues[rng.Below(static_cast<uint32_t>(kValueCount))];
    } else if (shape < 45) {
      // A small b, which is what a real shift amount looks like.
      b = rng.Below(256);
      op = kShiftOps[rng.Below(static_cast<uint32_t>(kShiftOpCount))];
    } else if (shape < 60) {
      // A b whose bits are all set above bit 5, which separates the 5-bit and
      // 6-bit masking rules from each other.
      b = 0xFFFFFFFFFFFFFFC0ull | rng.Below(64);
      op = kShiftOps[rng.Below(static_cast<uint32_t>(kShiftOpCount))];
    } else if (shape < 75) {
      // Operands that differ only above bit 32, where a W form must not see the
      // difference and a 64-bit form must.
      a = 0x0000000100000000ull | rng.Below(8);
      b = 0x0000000200000000ull | rng.Below(8);
    } else if (shape < 85) {
      // Full width but low entropy: mostly zeros, or mostly ones.
      const uint64_t fill = rng.Chance(50) ? 0ull : ~0ull;
      a = fill & rng.Next();
      b = rng.Next() & rng.Next();
    } else if (shape < 95) {
      // Sign-crossing pairs: the operands may differ in sign but not in
      // magnitude, which is where a signed/unsigned confusion shows up.
      const uint64_t low = rng.Next() & 0xFFFFFFFFull;
      a = low | (rng.Chance(50) ? 0xFFFFFFFF00000000ull : 0ull);
      b = low | (rng.Chance(50) ? 0xFFFFFFFF00000000ull : 0ull);
    }

    bench.Apply(a, b, op, "random");
  }

  bench.ReportCoverage();
  top->final();
  delete top;

  if (bench.failures() > 0) {
    const std::string detail = std::to_string(bench.failures()) +
                               " failed checks over " +
                               std::to_string(bench.vectors()) + " vectors";
    return rep.Finish("FAIL", detail);
  }

  const std::string detail = std::to_string(bench.vectors()) +
                             " stimulus vectors (" +
                             std::to_string(bench.handwritten()) +
                             " hand-written invariants, seed " +
                             std::to_string(opt.seed) + "); " +
                             bench.CoverageSummary();
  return rep.Finish("PASS", detail);
}