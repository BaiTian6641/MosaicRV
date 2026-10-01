// CASE=alu.boundaries -- work package I-011.
//
// Two units under test, both purely combinational, both checked against
// reference models written independently of the RTL:
//
//   * rtl/core/mosaic_alu.sv          -- all sixteen `mosaic_pkg::alu_op_e`
//                                        encodings: add/sub/shift/compare/logic,
//                                        the five RV64 word forms and ALU_PASSB;
//   * rtl/core/mosaic_branch_target.sv -- link value, taken/not-taken target,
//                                        and "does control transfer";
//   * rtl/core/mosaic_branch_cmp.sv    -- the BEQ/BNE/BLT/BGE/BLTU/BGEU
//                                        comparison that feeds `branch_taken`.
//
// The ALU is swept over the full 2x2 matrix of a 20-value boundary operand set,
// a dedicated shift-amount sweep, and a seeded random campaign. The branch half
// is swept over a pc set, an immediate set, all eight funct3 values, both
// `branch_taken` values and both `is_jalr` values, plus the closed chain in which
// the DUT comparator drives the DUT target unit.
//
// Independence of the reference models
// -----------------------------------
// The ALU model is written from the RV64I pseudo-code in int64_t/uint64_t (and
// uint32_t for the W forms). It shares no expression with the RTL: the RTL is
// bit-selected SV with `$signed()` casts, the model is host integer arithmetic,
// and the W sign-extension -- the single easiest thing to get wrong -- is done by
// an explicit function rather than by a cast.
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
// Coverage is asserted, not assumed: a run that stops reaching an op, a condition,
// or the operand/shift/target shapes that distinguish the mutants fails instead
// of quietly passing over a shorter path. See `Bench::ReportCoverage`.

#include "sim_common.h"
#include "verilated.h"
#include "Vmosaic_alu_tb.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

// The `mosaic_pkg::alu_op_e` encodings, transcribed from rtl/core/mosaic_pkg.sv.
// I-010 verifies the same numbering from the RTL side; this file cannot import a
// SystemVerilog package, so the transcription is the one thing the two sides
// must agree on by inspection.
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

// RV64I branch funct3. funct3 = 010 and 011 name no condition and the decoder
// reports them illegal; they are carried through the sweep anyway, because a
// unit that answers something specific for them is testable and one that does
// not is not.
enum BranchFunct : uint8_t {
  kBeq = 0,
  kBne = 1,
  kFunctNoneA = 2,
  kFunctNoneB = 3,
  kBlt = 4,
  kBge = 5,
  kBltu = 6,
  kBgeu = 7,
};

const char* const kFunctName[8] = {"beq", "bne", "none", "none",
                                   "blt", "bge", "bltu", "bgeu"};

constexpr uint64_t kSignBit64 = 0x8000000000000000ull;

// Boundary operands. Every entry is a point where one of the sixteen ops, or the
// distinction between a signed and an unsigned reading of it, changes answer:
// zero and the small integers, the all-ones mask, -2, INT64_MIN/INT64_MAX, the
// values straddling bit 31 in all three possible extensions, the two 16-bit
// halves, alternating bit patterns, and pairs that differ only above bit 32.
// The same set is reused as the comparator's operand set, where the sign-crossing
// entries are what separates blt from bltu.
const uint64_t kValues[] = {
    0x0000000000000000ull,  // 0, and x0
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

// Branch program counters. `0xFFFFFFFFFFFFFFFC` makes `pc + 4` wrap to zero,
// which is the only way a broken link rule can still look right, and the odd
// values make an odd pc explicit rather than implied.
const uint64_t kBranchPcs[] = {
    0x0000000000000000ull, 0x0000000000000002ull, 0x0000000000000004ull,
    0x0000000000000008ull, 0x0000000000001000ull, 0x00000000FFFFFFFCull,
    0x0000000100000000ull, 0x7FFFFFFFFFFFFFFCull, 0xFFFFFFFF00000000ull,
    0xFFFFFFFFFFFFFFF8ull, 0xFFFFFFFFFFFFFFFCull,
};
constexpr size_t kBranchPcCount = sizeof(kBranchPcs) / sizeof(kBranchPcs[0]);

// Branch immediates, already sign-extended the way the decoder delivers them.
// The set deliberately contains offsets whose low bits are 1 and 2, so that the
// difference between "JALR clears bit 0" and "JALR clears bits 1 and 0" is
// visible, and offsets that make `pc + imm` misaligned in bit 0, in bit 1, and in
// both.
const uint64_t kBranchImms[] = {
    0x0000000000000000ull, 0x0000000000000004ull, 0x0000000000000008ull,
    0xFFFFFFFFFFFFFFFCull, 0xFFFFFFFFFFFFFFF8ull, 0x0000000000000001ull,
    0x0000000000000002ull, 0x0000000000000006ull, 0xFFFFFFFFFFFFFFFFull,
    0x00000000000007F8ull, 0xFFFFFFFFFFFFF800ull, 0x0000000000000800ull,
    0x000000007FFFFFFCull, 0xFFFFFFFF80000004ull, 0x0000000000000003ull,
    0x0000000000000005ull,
};
constexpr size_t kBranchImmCount = sizeof(kBranchImms) / sizeof(kBranchImms[0]);

constexpr uint64_t kRandomVectors = 100000;
constexpr uint64_t kRandomBranchVectors = 20000;
constexpr uint32_t kMaxFailures = 8;  // stop early so a mutant log stays readable

// ------------------------------------------------------------ reference models

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

bool ReferenceBranchCmp(uint8_t funct, uint64_t rs1, uint64_t rs2) {
  const int64_t s1 = static_cast<int64_t>(rs1);
  const int64_t s2 = static_cast<int64_t>(rs2);
  switch (funct) {
    case kBeq:  return rs1 == rs2;
    case kBne:  return rs1 != rs2;
    case kBlt:  return s1 < s2;
    case kBge:  return s1 >= s2;
    case kBltu: return rs1 < rs2;  // unsigned: -1 is the largest value
    case kBgeu: return rs1 >= rs2;
    default:    break;
  }
  return false;  // funct3 010 / 011 name no condition
}

struct BranchResult {
  uint64_t link;
  uint64_t target;
  bool is_taken;
};

BranchResult ReferenceBranchTarget(uint64_t pc, uint64_t imm, bool is_jalr,
                                   uint8_t funct, bool branch_taken) {
  BranchResult out;
  out.link = pc + 4;  // wraps, like every other XLEN arithmetic in this model
  const uint64_t sum = pc + imm;
  // JALR clears bit 0 and nothing else. Bit 1 survives on purpose: the
  // misaligned-target report belongs to the core, not to this unit.
  const uint64_t aligned = is_jalr ? (sum & ~1ull) : sum;
  const bool names_condition = (funct != kFunctNoneA) && (funct != kFunctNoneB);
  out.is_taken = is_jalr || !names_condition || branch_taken;
  // A not-taken branch falls through to pc + 4, which is exactly the link value.
  out.target = out.is_taken ? aligned : out.link;
  return out;
}

// ------------------------------------------------------------------ the bench

class Bench {
 public:
  Bench(const mosaic::Options& options, mosaic::Reporter* reporter,
        Vmosaic_alu_tb* top)
      : options_(options), rep_(reporter), top_(top) {}

  // ---------------------------------------------------------------- ALU ------

  // One ALU stimulus vector against the model. The DUT is combinational, so a
  // single eval() settles it and there is nothing to wait for.
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
    CheckField(where, mosaic::Hex(want_result), mosaic::Hex(got_result));
    std::snprintf(where, sizeof(where), "%s %s(a=%s, b=%s) zero flag", phase,
                  kOpName[op], mosaic::Hex(a).c_str(), mosaic::Hex(b).c_str());
    CheckFlag(where, want_zero, got_zero);
  }

  // A hand-computed ALU expectation, checked against the RTL directly rather
  // than through the model, so a shared mistake in the model cannot make the two
  // agree with each other for the wrong reason.
  void Expect(uint64_t a, uint64_t b, uint8_t op, uint64_t want_result,
              bool want_zero, const char* what) {
    top_->a = a;
    top_->b = b;
    top_->op = op;
    top_->eval();

    CheckField(std::string(what) + ": result", mosaic::Hex(want_result),
               mosaic::Hex(top_->result));
    CheckFlag(std::string(what) + ": zero flag", want_zero, top_->zero != 0);
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

  // -------------------------------------------------------------- branch ------

  // One branch vector. The comparator and the target unit are each checked
  // against their own model in the same vector, with `branch_taken` driven from
  // the stimulus rather than from the DUT, so a wrong condition cannot hide
  // behind a wrong address or the other way round. The closed chain -- DUT
  // comparator into DUT target unit -- is checked separately, in `ApplyChain`.
  void ApplyBranch(uint64_t pc, uint64_t imm, bool is_jalr, uint8_t funct,
                   bool branch_taken, uint64_t rs1, uint64_t rs2,
                   const char* phase) {
    if (halted_) return;
    Drive(pc, imm, is_jalr, funct, branch_taken, rs1, rs2);

    const BranchResult want =
        ReferenceBranchTarget(pc, imm, is_jalr, funct, branch_taken);
    const bool want_cmp = ReferenceBranchCmp(funct, rs1, rs2);

    ++branch_vectors_;
    ++per_funct_[funct];
    if (want.is_taken) {
      ++taken_vectors_;
    } else {
      ++not_taken_vectors_;
      if (top_->bt_target == top_->bt_link) ++not_taken_link_vectors_;
    }
    const uint64_t sum = pc + imm;
    if (is_jalr) {
      ++jalr_vectors_;
      if ((sum & 1ull) != 0 && (top_->bt_target & 1ull) == 0) {
        ++jalr_clears_bit0_vectors_;
      }
      // The target is still misaligned after JALR: bit 1 survived, which is
      // what "clears bit 0 only" means, and the core will raise the trap.
      if ((sum & 3ull) == 2 && (top_->bt_target & 3ull) == 2) {
        ++jalr_keeps_bit1_vectors_;
      }
    } else {
      ++jal_vectors_;
    }
    if (want_cmp) {
      ++cmp_true_vectors_;
    } else {
      ++cmp_false_vectors_;
    }
    if (rs1 == 0 || rs2 == 0) ++cmp_x0_vectors_;

    char where[224];
    std::snprintf(where, sizeof(where),
                  "%s %s pc=%s imm=%s jalr=%d taken=%d", phase,
                  kFunctName[funct], mosaic::Hex(pc).c_str(),
                  mosaic::Hex(imm).c_str(), is_jalr ? 1 : 0,
                  branch_taken ? 1 : 0);

    CheckField(std::string(where) + ": link", mosaic::Hex(want.link),
               mosaic::Hex(top_->bt_link));
    CheckField(std::string(where) + ": target", mosaic::Hex(want.target),
               mosaic::Hex(top_->bt_target));
    CheckFlag(std::string(where) + ": is_taken", want.is_taken,
              top_->bt_is_taken != 0);
    CheckFlag(std::string(where) + ": compare", want_cmp,
              top_->bt_cmp_taken != 0);
  }

  // The closed chain: the comparator's own answer drives the target unit, and the
  // pair is compared against the model chain. This is how the two units are
  // checked in the configuration the core will actually wire them in.
  void ApplyChain(uint64_t pc, uint64_t imm, bool is_jalr, uint8_t funct,
                  uint64_t rs1, uint64_t rs2, const char* phase) {
    if (halted_) return;
    Drive(pc, imm, is_jalr, funct, false, rs1, rs2);
    const bool dut_taken = top_->bt_cmp_taken != 0;
    top_->bt_branch_taken = dut_taken ? 1 : 0;
    top_->eval();

    const bool model_taken = ReferenceBranchCmp(funct, rs1, rs2);
    const BranchResult want =
        ReferenceBranchTarget(pc, imm, is_jalr, funct, model_taken);
    ++chain_vectors_;

    char where[224];
    std::snprintf(where, sizeof(where), "%s chain %s pc=%s imm=%s jalr=%d",
                  phase, kFunctName[funct], mosaic::Hex(pc).c_str(),
                  mosaic::Hex(imm).c_str(), is_jalr ? 1 : 0);
    CheckField(std::string(where) + ": target", mosaic::Hex(want.target),
               mosaic::Hex(top_->bt_target));
    CheckFlag(std::string(where) + ": is_taken", want.is_taken,
              top_->bt_is_taken != 0);
  }

  // A hand-computed branch expectation, checked against the RTL directly.
  void ExpectBranch(uint64_t pc, uint64_t imm, bool is_jalr, uint8_t funct,
                    bool branch_taken, uint64_t want_link, uint64_t want_target,
                    bool want_is_taken, const char* what) {
    Drive(pc, imm, is_jalr, funct, branch_taken, 0, 0);
    CheckField(std::string(what) + ": link", mosaic::Hex(want_link),
               mosaic::Hex(top_->bt_link));
    CheckField(std::string(what) + ": target", mosaic::Hex(want_target),
               mosaic::Hex(top_->bt_target));
    CheckFlag(std::string(what) + ": is_taken", want_is_taken,
              top_->bt_is_taken != 0);
    ++handwritten_;
  }

  // A hand-computed comparison expectation, checked against the DUT comparator.
  void ExpectCompare(uint8_t funct, uint64_t rs1, uint64_t rs2, bool want,
                     const char* what) {
    Drive(0, 0, false, funct, false, rs1, rs2);
    CheckFlag(what, want, top_->bt_cmp_taken != 0);
    ++handwritten_;
  }

  void Drive(uint64_t pc, uint64_t imm, bool is_jalr, uint8_t funct,
             bool branch_taken, uint64_t rs1, uint64_t rs2) {
    top_->bt_pc = pc;
    top_->bt_imm = imm;
    top_->bt_is_jalr = is_jalr ? 1 : 0;
    top_->bt_branch_funct = funct;
    top_->bt_branch_taken = branch_taken ? 1 : 0;
    top_->bt_rs1_value = rs1;
    top_->bt_rs2_value = rs2;
    top_->eval();
  }

  // ------------------------------------------------------------- coverage ----

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

    // The operand shapes that separate the ALU mutants. If one of these drops
    // out of the stimulus the case fails, because a run that no longer
    // distinguishes signed from unsigned, 5-bit from 6-bit shift masking, or
    // arithmetic from logical shift is no longer testing anything.
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
    rep_->Check(vectors_ > 0, "coverage: ALU stimulus was applied");

    // The same for the branch half: every funct3, both directions of every
    // condition, both is_jalr values, a not-taken branch whose target fell back
    // to pc + 4, a JALR that cleared bit 0, and a JALR whose target stayed
    // misaligned in bit 1.
    for (uint8_t funct = 0; funct < 8; ++funct) {
      rep_->Check(per_funct_[funct] > 0,
                  std::string("coverage: funct3 ") + kFunctName[funct] +
                      " was exercised");
    }
    rep_->Check(taken_vectors_ > 0, "coverage: a taken transfer was exercised");
    rep_->Check(not_taken_vectors_ > 0,
                "coverage: a not-taken branch was exercised");
    rep_->Check(not_taken_link_vectors_ == not_taken_vectors_,
                "coverage: every not-taken branch fell back to pc + 4");
    rep_->Check(jalr_vectors_ > 0, "coverage: JALR was exercised");
    rep_->Check(jal_vectors_ > 0, "coverage: the non-JALR path was exercised");
    rep_->Check(jalr_clears_bit0_vectors_ > 0,
                "coverage: JALR cleared a bit 0 that was set");
    rep_->Check(jalr_keeps_bit1_vectors_ > 0,
                "coverage: JALR left a misaligned bit 1 in the target");
    rep_->Check(cmp_true_vectors_ > 0 && cmp_false_vectors_ > 0,
                "coverage: both outcomes of every condition were seen");
    rep_->Check(cmp_x0_vectors_ > 0,
                "coverage: a comparison against a zero (x0) operand was made");
    rep_->Check(chain_vectors_ > 0,
                "coverage: the comparator-to-target chain was exercised");
  }

  uint64_t vectors() const { return vectors_; }
  uint64_t branch_vectors() const { return branch_vectors_ + chain_vectors_; }
  uint64_t handwritten() const { return handwritten_; }
  uint32_t failures() const { return failures_; }
  bool halted() const { return halted_; }
  bool out_of_budget() const { return branch_vectors() + vectors_ >=
                              options_.max_cycles; }

  std::string CoverageSummary() const {
    std::string text = "alu_ops=";
    for (uint8_t op = 0; op < kOpCount; ++op) {
      if (op != 0) text += " ";
      text += std::string(kOpName[op]) + "=" + std::to_string(per_op_[op]);
    }
    text += "; branch=" + std::to_string(branch_vectors_) + " vectors, chain=" +
            std::to_string(chain_vectors_);
    return text;
  }

 private:
  void CheckField(const std::string& where, const std::string& want,
                  const std::string& got) {
    const bool ok = want == got;
    if (!ok) {
      rep_->Mismatch(where, want, got);
      Fail(where);
    }
    rep_->Check(ok, where);
  }

  void CheckFlag(const std::string& where, bool want, bool got) {
    if (got != want) {
      rep_->Mismatch(where, want ? "1" : "0", got ? "1" : "0");
      Fail(where);
    }
    rep_->Check(got == want, where);
  }

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
  uint64_t branch_vectors_ = 0;
  uint64_t chain_vectors_ = 0;
  uint64_t handwritten_ = 0;
  uint32_t failures_ = 0;
  bool halted_ = false;

  uint64_t per_op_[kOpCount] = {};
  uint32_t zero_result_ops_[kOpCount] = {};
  uint32_t nonzero_result_ops_[kOpCount] = {};
  uint32_t both_zero_ops_[kOpCount] = {};

  uint64_t per_funct_[8] = {};
  uint64_t taken_vectors_ = 0;
  uint64_t not_taken_vectors_ = 0;
  uint64_t not_taken_link_vectors_ = 0;
  uint64_t jalr_vectors_ = 0;
  uint64_t jal_vectors_ = 0;
  uint64_t jalr_clears_bit0_vectors_ = 0;
  uint64_t jalr_keeps_bit1_vectors_ = 0;
  uint64_t cmp_true_vectors_ = 0;
  uint64_t cmp_false_vectors_ = 0;
  uint64_t cmp_x0_vectors_ = 0;

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

  // ---- hand-written ALU invariants, straight from the RV64I pseudo-code -----
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

  // ---- hand-written branch and comparison invariants ------------------------
  {
    const uint64_t kMinusOne = 0xFFFFFFFFFFFFFFFFull;
    const uint64_t kMinusFour = 0xFFFFFFFFFFFFFFFCull;

    // link is always pc + 4, including when that wraps and when the pc is odd.
    bench.ExpectBranch(0, 0, false, kBeq, true, 4, 0, true,
                       "link at pc=0 is pc+4 and the target is pc+imm");
    bench.ExpectBranch(0xFFFFFFFFFFFFFFFCull, 0, true, kBne, false, 0,
                       0xFFFFFFFFFFFFFFFCull, true,
                       "link wraps to zero at the top of the address space");
    bench.ExpectBranch(0xFFFFFFFFFFFFFFFCull, 4, true, kBne, true, 0,
                       0x0000000000000000ull, true,
                       "a JALR at the top of the address space links to zero");
    bench.ExpectBranch(2, 0, false, kBeq, true, 6, 2, true,
                       "an odd pc links to pc+4, unaligned");

    // JALR clears bit 0 and nothing else.
    bench.ExpectBranch(0, 1, true, kBne, true, 4, 0, true,
                       "JALR clears bit 0 of the target");
    bench.ExpectBranch(0, 2, true, kBne, true, 4, 2, true,
                       "JALR keeps bit 1: a target 2 bytes past pc");
    bench.ExpectBranch(0, 6, true, kBne, true, 4, 6, true,
                       "JALR keeps bit 1 with a 6-byte offset");
    bench.ExpectBranch(4, 2, true, kBne, true, 8, 6, true,
                       "JALR clears bit 0 of pc+imm, not of imm");
    bench.ExpectBranch(0, 0x000000007FFFFFFCull, true, kBne, true, 4,
                       0x000000007FFFFFFCull, true,
                       "JALR keeps bit 1 of a large misaligned target");

    // A JAL does not clear anything: bit 1 of the target must survive so the
    // core can raise the misaligned-target trap.
    bench.ExpectBranch(0, 2, false, kBne, true, 4, 2, true,
                       "JAL leaves a 2-byte-aligned target for the trap logic");
    bench.ExpectBranch(0, 2, false, kBne, false, 4, 4, false,
                       "a branch with bit 1 set in imm falls through to pc+4");

    // A not-taken branch still produces pc + 4, so the sequential next PC never
    // needs a second path.
    bench.ExpectBranch(0x1000, 0x7F8, false, kBne, false, 0x1004, 0x1004, false,
                       "an untaken forward branch yields pc+4");
    bench.ExpectBranch(0x1000, kMinusFour, false, kBne, false, 0x1004, 0x1004,
                       false, "an untaken backward branch yields pc+4");
    bench.ExpectBranch(0x1000, 0x7F8, false, kBne, true, 0x1004, 0x17F8, true,
                       "a taken forward branch yields pc+imm");

    // funct3 010 and 011 name no condition; the unit still has to answer, and
    // an answer of "transfers" is what lets a JAL reach a port list that has
    // is_jalr but no is_jal.
    bench.ExpectBranch(0x1000, 4, false, kFunctNoneA, false, 0x1004, 0x1004, true,
                       "funct3 010 names no condition and still transfers");
    bench.ExpectBranch(0x1000, 4, false, kFunctNoneB, false, 0x1004, 0x1004, true,
                       "funct3 011 names no condition and still transfers");

    // The six conditions. blt and bltu on (-1, 1) are the pair that separates
    // the signed reading from the unsigned one in both directions, and the x0
    // cases show the comparator reading a zero register as zero.
    bench.ExpectCompare(kBeq, 7, 7, true, "beq(7,7) is true");
    bench.ExpectCompare(kBeq, 7, 8, false, "beq(7,8) is false");
    bench.ExpectCompare(kBne, 7, 7, false, "bne(7,7) is false");
    bench.ExpectCompare(kBlt, kMinusOne, 1, true, "blt(-1,1) is true, signed");
    bench.ExpectCompare(kBlt, 1, kMinusOne, false, "blt(1,-1) is false");
    bench.ExpectCompare(kBge, kMinusOne, 1, false, "bge(-1,1) is false");
    bench.ExpectCompare(kBge, 1, kMinusOne, true, "bge(1,-1) is true");
    bench.ExpectCompare(kBltu, kMinusOne, 1, false,
                        "bltu(-1,1) is false, -1 is the largest unsigned");
    bench.ExpectCompare(kBgeu, kMinusOne, 1, true, "bgeu(-1,1) is true");
    bench.ExpectCompare(kBlt, 0, kMinusOne, false,
                        "blt(x0,-1) is false: x0 reads as zero");
    bench.ExpectCompare(kBltu, 0, kMinusOne, true,
                        "bltu(x0,-1) is true: x0 reads as zero");
    bench.ExpectCompare(kBeq, 0, 0, true, "beq(x0,x0) is true");
    bench.ExpectCompare(kBlt, 0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull, true,
                        "blt(INT64_MIN,INT64_MAX) is true");
    bench.ExpectCompare(kBltu, 0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull, false,
                        "bltu(INT64_MIN,INT64_MAX) is false");
    bench.ExpectCompare(kFunctNoneA, 1, 2, false,
                        "funct3 010 compares as no condition");
    bench.ExpectCompare(kFunctNoneB, 1, 2, false,
                        "funct3 011 compares as no condition");
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

  // ---- the branch sweep ----------------------------------------------------
  // pc x imm x all eight funct3 x both is_jalr x both branch_taken, with a
  // sign-crossing comparator operand pair so that blt and bltu disagree on some
  // of them.
  const uint64_t kCmpPairA[] = {0, 1, 0xFFFFFFFFFFFFFFFFull,
                                0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull};
  const uint64_t kCmpPairB[] = {0, 1, 0xFFFFFFFFFFFFFFFFull,
                                0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull};
  constexpr size_t kCmpPairCount = sizeof(kCmpPairA) / sizeof(kCmpPairA[0]);

  for (size_t f = 0; f < 8 && !bench.halted() && !bench.out_of_budget(); ++f) {
    const uint8_t funct = static_cast<uint8_t>(f);
    for (size_t p = 0;
         p < kBranchPcCount && !bench.halted() && !bench.out_of_budget(); ++p) {
      for (size_t m = 0;
           m < kBranchImmCount && !bench.halted() && !bench.out_of_budget(); ++m) {
        for (int jalr = 0; jalr < 2 && !bench.halted() && !bench.out_of_budget();
             ++jalr) {
          for (int taken = 0; taken < 2 && !bench.halted() &&
                                !bench.out_of_budget();
               ++taken) {
            bench.ApplyBranch(kBranchPcs[p], kBranchImms[m], jalr != 0, funct,
                              taken != 0, kCmpPairA[p % kCmpPairCount],
                              kCmpPairB[m % kCmpPairCount], "branch-sweep");
          }
        }
      }
    }
  }

  // ---- the comparator over the full boundary operand set -------------------
  for (uint8_t funct = 0; funct < 8 && !bench.halted() && !bench.out_of_budget();
       ++funct) {
    for (size_t i = 0; i < kValueCount && !bench.halted() && !bench.out_of_budget();
         ++i) {
      for (size_t j = 0;
           j < kValueCount && !bench.halted() && !bench.out_of_budget(); ++j) {
        bench.ApplyBranch(0x1000, 0x7F8, false, funct, false, kValues[i],
                          kValues[j], "cmp-sweep");
      }
    }
  }

  // ---- the closed chain: DUT comparator into DUT target unit ---------------
  for (uint8_t funct = 0; funct < 8 && !bench.halted() && !bench.out_of_budget();
       ++funct) {
    for (size_t i = 0; i < kValueCount && !bench.halted() && !bench.out_of_budget();
         ++i) {
      for (size_t p = 0;
           p < kBranchPcCount && !bench.halted() && !bench.out_of_budget(); ++p) {
        for (int jalr = 0; jalr < 2 && !bench.halted() && !bench.out_of_budget();
             ++jalr) {
          bench.ApplyChain(kBranchPcs[p], kBranchImms[i % kBranchImmCount],
                           jalr != 0, funct, kValues[i], kValues[(i + 1) % kValueCount],
                           "chain");
        }
      }
    }
  }

  // ---- seeded random campaigns ---------------------------------------------
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

  for (uint64_t n = 0; n < kRandomBranchVectors && !bench.halted() &&
                       !bench.out_of_budget();
       ++n) {
    uint64_t pc = rng.Next();
    uint64_t imm = rng.Next();
    // Real immediates are small and mostly aligned; full-width randomness here
    // would almost never produce the bit-0/bit-1 cases the rule is about.
    if (rng.Chance(70)) imm = static_cast<uint64_t>(rng.Below(8192));
    if (rng.Chance(70)) pc &= ~3ull;
    const uint8_t funct = static_cast<uint8_t>(rng.Below(8));
    const bool jalr = rng.Chance(25);
    const bool taken = rng.Chance(50);
    const uint32_t shape = rng.Below(100);
    uint64_t rs1 = rng.Next();
    uint64_t rs2 = rng.Next();
    if (shape < 30) {
      rs1 = kValues[rng.Below(static_cast<uint32_t>(kValueCount))];
      rs2 = kValues[rng.Below(static_cast<uint32_t>(kValueCount))];
    } else if (shape < 60) {
      // Sign-crossing pairs equal in magnitude, which is the blt/bltu split.
      const uint64_t low = rng.Next() & 0xFFFFFFFFull;
      rs1 = low | (rng.Chance(50) ? 0xFFFFFFFF00000000ull : 0ull);
      rs2 = low | (rng.Chance(50) ? 0xFFFFFFFF00000000ull : 0ull);
    } else if (shape < 75) {
      // One side is x0.
      if (rng.Chance(50)) {
        rs1 = 0;
      } else {
        rs2 = 0;
      }
    }
    bench.ApplyBranch(pc, imm, jalr, funct, taken, rs1, rs2, "branch-random");
  }

  bench.ReportCoverage();
  top->final();
  delete top;

  if (bench.failures() > 0) {
    const std::string detail = std::to_string(bench.failures()) +
                               " failed checks over " +
                               std::to_string(bench.vectors()) + " ALU and " +
                               std::to_string(bench.branch_vectors()) +
                               " branch vectors";
    return rep.Finish("FAIL", detail);
  }

  const std::string detail =
      std::to_string(bench.vectors()) + " ALU and " +
      std::to_string(bench.branch_vectors()) + " branch stimulus vectors (" +
      std::to_string(bench.handwritten()) +
      " hand-written invariants, seed " + std::to_string(opt.seed) + "); " +
      bench.CoverageSummary();
  return rep.Finish("PASS", detail);
}