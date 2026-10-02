// ============================================================================
// tb_core_fp.cpp -- CASE=fp.precise_flags_and_boxing, work package I-050.
//
// The DUT is the integrated p0 core with the I-049 floating-point datapath
// behind the I-050 integration boundary: rename's FP namespace, dispatch's FP
// class, the shared FP unit on writeback port 2, the precise-`fflags` sideband
// and NaN-boxing. What this case claims, and what it uses as the oracle for each
// claim, is stated here because a float test whose expectation came from the
// hardware under test agrees with a wrong implementation perfectly:
//
//   * ARITHMETIC AND ITS FOUR EXCEPTION FLAGS come from the HOST's own FPU
//     through C's `float`/`double` operators with `fesetround`, and the flags
//     from `fetestexcept` -- a second, independent IEEE-754 implementation on
//     this machine, the same oracle I-049 used, for both formats and every
//     rounding mode this platform's <fenv.h> defines. (RMM has no host mode
//     here -- FE_TONEARESTFROMZERO is undefined -- so it is not driven by this
//     case; CASE=fp.operation_matrix owns it, with its derived tie expectation.)
//   * NaN-BOXING is a bit-level model written from the F extension: a 32-bit
//     result is written with bits 63:32 all ones, and a single-precision operand
//     whose upper half is not all ones is the canonical quiet NaN. No arithmetic
//     is involved, so no second implementation is needed.
//   * THE NaN POLICY is the ISA's, asserted on bits: every NaN-producing
//     operation returns the canonical quiet NaN, an sNaN operand sets NV for an
//     arithmetic operation, a quiet NaN operand does not. The NV half is also
//     cross-checked against the host, which raises FE_INVALID on the same
//     inputs.
//   * CONVERSIONS fp->int follow the ISA's saturation rule, which C does not
//     have (C's cast is undefined out of range), so the out-of-range vectors use
//     a model written from the spec; the in-range ones come from the host's
//     `llrint`, which rounds identically.
//   * mstatus.FS follows the privileged spec's FS state machine, read back
//     through `csrr mstatus`: Dirty after an instruction that writes FP state,
//     unchanged after one that only reads it.
//   * PRECISE `fflags`: an FP operation contributes its flags only when it
//     *retires*. The phase that tests this needs the machine, not the host, for
//     the expectation -- but it is not circular, because it also requires
//     positive evidence that the squashed operation *executed and produced its
//     flags* (the FP unit's flag counter moved) while the architectural register
//     stayed clear. A phase in which the operation never completed would be
//     vacuous, and is failed as such.
//
// Everything is a *program*: this driver hand-encodes RV64I + F/D (the corpus
// audit forbids F/D in tests/programs), drives the core's instruction and data
// ports, and reads its observations back out of memory and the core's counters.
//
// Phases, each a fresh reset and a fresh program, so no phase can inherit state
// from another:
//
//   1. arith        arithmetic + flags against the host FPU (both formats,
//                   four rounding modes, ties, overflow, subnormal, signed zero)
//   2. box-nan      boxing, unboxing, NaN bits, signed zero
//   3. cvt          conversion overflow/saturation and NV/NX
//   4. mem          flw/fsw/fld/fsd through the memory path
//   5. wrong-path   a squashed FP operation contributes no flags and no
//                   destination. The wrong path is a synchronous trap (a
//                   misaligned load) with a completed-and-flagged multiply and
//                   an in-flight divide younger than it -- not a mispredicted
//                   branch, because dispatch's allocation barrier means nothing
//                   younger than an unresolved branch ever executes
//   6. fs-dirty     mstatus.FS dirtying on a write, not on a read
//   7. frm          a tie resolved by the instruction's rm field, and by frm
//                   when the field says "dynamic"
//
// Controls: tools/run_fp_controls.py injects exactly one defect per build,
// requires the mutant binary to differ from the shipping one and to exit 1, and
// requires the named check to appear in its log. See
// results/reports/I-050-fp-state.md.
//
// `--seed` is accepted and unused: every vector here is directed, and a random
// one would be a weaker test than the vectors below.
// ============================================================================

#include <verilated.h>

#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "Vmosaic_core_tb.h"
#include "mem_ref.h"
#include "memory_model.h"
#include "mosaic_platform.h"
#include "sim_common.h"

#pragma STDC FENV_ACCESS ON

using mosaic_ref::DataMem;

namespace {

constexpr int      kResetCycles  = 4;
constexpr uint64_t kProgramCycles = 200000;
constexpr uint64_t kStallCycles  = 20000;
// The data block and the observation slots. Both live in RAM (0x80000000 +
// 0x200000 on p0), far above the program text and above MOSAIC_TOHOST, and every
// access to them is `base register + signed 12-bit displacement` -- so each is
// kept under 2 KiB and checked, because a displacement that does not fit is
// silently truncated by the encoder and the access silently goes elsewhere.
constexpr uint64_t kDataBase = 0x80020000ull;
constexpr uint64_t kSlotBase = 0x80021000ull;
constexpr int      kDataSize = 2048;

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

// A phase check. `Reporter::Check` records a failure without stopping the run,
// and the verdict is the caller's flag, so main() ANDs the reporter's failure
// count into it. Without that, a phase whose comparison failed reported PASS --
// the silent pass the project's convention forbids. (This driver had exactly
// that defect for one build; the wrapper is here so there is one place to fix.)
void Check(mosaic::Reporter* reporter, bool ok, const std::string& what) {
  reporter->Check(ok, what);
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string U32(uint32_t value) { return mosaic::Hex(value, 8); }
std::string Dec(uint64_t value) { return std::to_string(value); }

// ---------------------------------------------------------------- fflags
// The RISC-V bit order, which is also mosaic_fpu's: {NV,DZ,OF,UF,NX}.
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

// The canonical quiet NaN of each format, boxed for the single. This is the one
// NaN this design produces, and the F extension permits it: an implementation
// may return the canonical NaN for any NaN-producing operation.
uint64_t CanonNaN(bool fmt) {
  return fmt ? 0xFFFF'FFFF'7FC0'0000ull : 0x7FF8'0000'0000'0000ull;
}
bool IsNaNBits(uint64_t bits, bool fmt) {
  if (fmt) return ((bits >> 23) & 0xFFu) == 0xFFu && (bits & 0x7FFFFFu) != 0;
  return ((bits >> 52) & 0x7FFull) == 0x7FFull && (bits & 0xFFFFFFFFFFFFFull) != 0;
}
// A signalling NaN is a NaN whose most significant fraction bit is clear.
bool IsSNaNBits(uint64_t bits, bool fmt) {
  if (!IsNaNBits(bits, fmt)) return false;
  if (fmt) return ((bits >> 22) & 1u) == 0;
  return ((bits >> 51) & 1ull) == 0;
}

// ---------------------------------------------------------- the host oracle
struct HostResult {
  uint64_t bits;
  uint32_t flags;
  bool is_nan;
};

int HostMode(uint8_t rm) {
  switch (rm) {
    case 1: return FE_TOWARDZERO;   // RTZ
    case 2: return FE_DOWNWARD;     // RDN
    case 3: return FE_UPWARD;       // RUP
    default: return FE_TONEAREST;   // RNE (and, for a reserved field, RNE)
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

// op: 0 add, 1 sub, 2 mul, 3 div. `fmt` true selects single precision. For a
// single, `a`/`b` carry the 32-bit pattern in their low half.
HostResult HostArith(int op, bool fmt, uint64_t a, uint64_t b, uint8_t rm) {
  const int hm = HostMode(rm);
  if (fmt) {
    return HostBinaryT<float>(op, F32From(static_cast<uint32_t>(a)),
                              F32From(static_cast<uint32_t>(b)), hm);
  }
  return HostBinaryT<double>(op, F64From(a), F64From(b), hm);
}

// The host's int->fp conversion under a mode, for the fcvt.{s,d}.w vectors --
// C's conversion is the same operation, and the flags come from fetestexcept.
HostResult HostIntToFp(bool fmt, uint64_t value, bool is_signed, uint8_t rm) {
  feclearexcept(FE_ALL_EXCEPT);
  fesetround(HostMode(rm));
  volatile long long sv = static_cast<long long>(value);
  volatile unsigned long long uv = value;
  HostResult out;
  out.is_nan = false;
  if (fmt) {
    volatile float r = is_signed ? static_cast<float>(sv) : static_cast<float>(uv);
    const float v = r;
    out.bits = F32Bits(v);
  } else {
    volatile double r = is_signed ? static_cast<double>(sv) : static_cast<double>(uv);
    const double v = r;
    out.bits = F64Bits(v);
  }
  out.flags = MapFlags(fetestexcept(FE_ALL_EXCEPT));
  fesetround(FE_TONEAREST);
  return out;
}

// The host's round-to-integer under a mode, for the in-range fp->int vectors.
bool HostRoundToInt64(uint64_t bits, bool fmt, uint8_t rm, uint64_t* out,
                      uint32_t* flags) {
  feclearexcept(FE_ALL_EXCEPT);
  fesetround(HostMode(rm));
  long long r;
  if (fmt) {
    r = llrintf(F32From(static_cast<uint32_t>(bits)));
  } else {
    r = llrint(F64From(bits));
  }
  const int f = fetestexcept(FE_ALL_EXCEPT);
  fesetround(FE_TONEAREST);
  *out = static_cast<uint64_t>(r);
  *flags = MapFlags(f);
  return true;
}

// ------------------------------------------------------------------- asm
// A very small RV64 assembler. It emits into a sparse map so a program can be
// laid out with labels; only the forms this case needs exist.
class ProgImage {
 public:
  void Put(uint64_t addr, uint32_t word) { words_[addr] = word; }
  uint32_t Word(uint64_t addr) const {
    auto it = words_.find(addr);
    return (it == words_.end()) ? 0x0000006fu : it->second;  // jal x0, 0
  }

 private:
  std::map<uint64_t, uint32_t> words_;
};

class Asm {
 public:
  explicit Asm(uint64_t base) : base_(base) {}

  uint64_t pc() const { return base_ + 4ull * words_.size(); }
  const std::vector<uint32_t>& words() const { return words_; }

  static uint32_t R(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3,
                    uint32_t rd, uint32_t op) {
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
  }
  static uint32_t I(int32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd,
                    uint32_t op) {
    return ((static_cast<uint32_t>(imm) & 0xFFFu) << 20) | (rs1 << 15) |
           (f3 << 12) | (rd << 7) | op;
  }
  static uint32_t S(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3,
                    uint32_t op) {
    const uint32_t u = static_cast<uint32_t>(imm);
    return (((u >> 5) & 0x7Fu) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
           ((u & 0x1Fu) << 7) | op;
  }
  static uint32_t U(uint32_t imm20, uint32_t rd, uint32_t op) {
    return (imm20 << 12) | (rd << 7) | op;
  }
  static uint32_t B(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3,
                    uint32_t op) {
    const uint32_t u = static_cast<uint32_t>(imm);
    return (((u >> 12) & 1u) << 31) | (((u >> 5) & 0x3Fu) << 25) | (rs2 << 20) |
           (rs1 << 15) | (f3 << 12) | (((u >> 1) & 0xFu) << 8) |
           (((u >> 11) & 1u) << 7) | op;
  }

  // A load or store displacement is a signed 12-bit field. Outside that range
  // the encoder truncates it and the access goes somewhere else -- which once
  // made a CSR case drive its operand as zero without saying so. This guard is
  // why that cannot happen silently here.
  static void CheckDisp(const char* what, int32_t imm) {
    if ((imm < -2048) || (imm > 2047)) {
      Fail("fp-case assembler", std::string(what) + " displacement " +
           std::to_string(imm) + " does not fit the signed 12-bit immediate");
    }
  }

  void Addi(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(I(imm, rs1, 0, rd, 0x13)); }
  void Add(uint32_t rd, uint32_t rs1, uint32_t rs2) { Emit(R(0x00, rs2, rs1, 0, rd, 0x33)); }
  void Sub(uint32_t rd, uint32_t rs1, uint32_t rs2) { Emit(R(0x20, rs2, rs1, 0, rd, 0x33)); }
  void Slli(uint32_t rd, uint32_t rs1, uint32_t sh) { Emit(R(0x00, sh, rs1, 1, rd, 0x13)); }
  void And(uint32_t rd, uint32_t rs1, uint32_t rs2) { Emit(R(0x00, rs2, rs1, 7, rd, 0x33)); }
  void Lui(uint32_t rd, uint32_t imm20) { Emit(U(imm20, rd, 0x37)); }
  void Ld(uint32_t rd, uint32_t rs1, int32_t imm) {
    CheckDisp("ld", imm);
    Emit(I(imm, rs1, 3, rd, 0x03));
  }
  void Lw(uint32_t rd, uint32_t rs1, int32_t imm) {
    CheckDisp("lw", imm);
    Emit(I(imm, rs1, 2, rd, 0x03));
  }
  void Sd(uint32_t rs2, uint32_t rs1, int32_t imm) {
    CheckDisp("sd", imm);
    Emit(S(imm, rs2, rs1, 3, 0x23));
  }
  void Sw(uint32_t rs2, uint32_t rs1, int32_t imm) {
    CheckDisp("sw", imm);
    Emit(S(imm, rs2, rs1, 2, 0x23));
  }
  // flw/fld, fsw/fsd.
  void Flw(uint32_t fd, uint32_t rs1, int32_t imm) {
    CheckDisp("flw", imm);
    Emit(I(imm, rs1, 2, fd, 0x07));
  }
  void Fld(uint32_t fd, uint32_t rs1, int32_t imm) {
    CheckDisp("fld", imm);
    Emit(I(imm, rs1, 3, fd, 0x07));
  }
  void Fsw(uint32_t fs, uint32_t rs1, int32_t imm) {
    CheckDisp("fsw", imm);
    Emit(S(imm, fs, rs1, 2, 0x27));
  }
  void Fsd(uint32_t fs, uint32_t rs1, int32_t imm) {
    CheckDisp("fsd", imm);
    Emit(S(imm, fs, rs1, 3, 0x27));
  }
  // beq rs1, rs2, +offset (a byte offset, as the encoding carries it).
  void Beq(uint32_t rs1, uint32_t rs2, int32_t offset) {
    Emit(B(offset, rs2, rs1, 0, 0x63));
  }
  // The M extension's `div`: a long-latency INTEGER operation, used below as a
  // delay that does not occupy the shared floating-point unit.
  void Div(uint32_t rd, uint32_t rs1, uint32_t rs2) { Emit(R(0x01, rs2, rs1, 4, rd, 0x33)); }
  // The M extension's 32-bit `divw` (OP-32): half the divide's iterations, so a
  // shorter fixed-latency integer delay than `div`.
  void Divw(uint32_t rd, uint32_t rs1, uint32_t rs2) { Emit(R(0x01, rs2, rs1, 4, rd, 0x3B)); }
  void Csrrw(uint32_t rd, uint32_t csr, uint32_t rs1) { Emit(I(csr, rs1, 1, rd, 0x73)); }
  void Csrrs(uint32_t rd, uint32_t csr, uint32_t rs1) { Emit(I(csr, rs1, 2, rd, 0x73)); }
  void Csrr(uint32_t rd, uint32_t csr) { Csrrs(rd, csr, 0); }
  void Mret() { Emit(0x30200073u); }
  void JalSelf() { Emit(0x0000006fu); }

  // OP-FP. f7 is sized by format and operation; f3 is the rm field for the
  // arithmetic forms and a selector for the others.
  void Fp(uint32_t f7, uint32_t f3, uint32_t fd, uint32_t fs1, uint32_t fs2) {
    Emit(R(f7, fs2, fs1, f3, fd, 0x53));
  }
  // op: 0 add, 1 sub, 2 mul, 3 div. fmt: 1 = single.
  void Farith(int op, bool fmt, uint32_t fd, uint32_t fs1, uint32_t fs2,
              uint32_t rm) {
    static const uint32_t kF7[4][2] = {{0x00, 0x01}, {0x04, 0x05},
                                       {0x08, 0x09}, {0x0C, 0x0D}};
    Fp(kF7[op][fmt ? 0 : 1], rm, fd, fs1, fs2);
  }
  void FmvWX(uint32_t fd, uint32_t rs1) { Fp(0x78, 0, fd, rs1, 0); }   // fmv.w.x
  void FmvDX(uint32_t fd, uint32_t rs1) { Fp(0x79, 0, fd, rs1, 0); }   // fmv.d.x
  void FmvXW(uint32_t rd, uint32_t fs1) { Fp(0x70, 0, rd, fs1, 0); }   // fmv.x.w
  void FmvXD(uint32_t rd, uint32_t fs1) { Fp(0x71, 0, rd, fs1, 0); }   // fmv.x.d
  // fcvt: `sel` is the rs2 field (0 w, 1 wu, 2 l, 3 lu).
  void FCvtFI(bool fmt, uint32_t sel, uint32_t rd, uint32_t fs1, uint32_t rm) {
    Fp(fmt ? 0x60 : 0x61, rm, rd, fs1, sel);
  }
  void FCvtIF(bool fmt, uint32_t sel, uint32_t fd, uint32_t rs1, uint32_t rm) {
    Fp(fmt ? 0x68 : 0x69, rm, fd, rs1, sel);
  }

  // Load an absolute 64-bit address PC-relative (lui alone sign-extends a
  // bit-31 address into a fault).
  void LaAbs(uint32_t rd, uint64_t target) {
    const int64_t delta = static_cast<int64_t>(target) - static_cast<int64_t>(pc());
    const uint32_t lo = static_cast<uint32_t>(delta) & 0xFFFu;
    const uint32_t hi = static_cast<uint32_t>((delta + 0x800) >> 12) & 0xFFFFFu;
    Emit(U(hi, rd, 0x17));
    Emit(I(static_cast<int32_t>(lo) - ((lo & 0x800u) ? 0x1000 : 0), rd, 0, rd, 0x13));
  }

  // The exit protocol: `sd 1, 0(tohost)`, then spin.
  void Exit() {
    Addi(5, 0, 1);
    LaAbs(6, MOSAIC_TOHOST);
    Sd(5, 6, 0);
    JalSelf();
  }

 private:
  void Emit(uint32_t w) { words_.push_back(w); }

  uint64_t base_;
  std::vector<uint32_t> words_;
};

// ---------------------------------------------------------------- geometry
struct Geometry {
  uint32_t xlen = 0;
  uint32_t retire_width = 0;
  uint32_t rob_entries = 0;
  uint64_t reset_vector = 0;
  uint32_t has_s = 0;
  uint32_t has_u = 0;
};

Geometry ReadGeometry(Vmosaic_core_tb* dut) {
  Geometry g;
  g.xlen = dut->o_geom_xlen_o;
  g.retire_width = dut->o_geom_retire_width_o;
  g.rob_entries = dut->o_geom_rob_entries_o;
  g.reset_vector = dut->o_geom_reset_vector_o;
  g.has_s = dut->o_geom_has_s_o;
  g.has_u = dut->o_geom_has_u_o;
  return g;
}

// ------------------------------------------------------------------ imem
class Imem {
 public:
  struct Request {
    uint64_t addr = 0;
    uint32_t id = 0;
    uint32_t epoch = 0;
  };
  explicit Imem(const ProgImage* img) : img_(img) {}
  bool HasResponse() const { return !ready_.empty(); }
  const Request& Response() const { return ready_.front(); }
  uint32_t ResponseWord() const { return img_->Word(ready_.front().addr); }
  void Accept(uint64_t addr, uint32_t id, uint32_t epoch) {
    inflight_.push_back(Entry{Request{addr, id, epoch}, 1});
  }
  void PopResponse() { ready_.pop_front(); }
  void Advance() {
    for (size_t i = 0; i < inflight_.size();) {
      if (--inflight_[i].left == 0) {
        ready_.push_back(inflight_[i].req);
        inflight_.erase(inflight_.begin() + static_cast<long>(i));
      } else {
        ++i;
      }
    }
  }

 private:
  struct Entry {
    Request req;
    int left = 0;
  };
  const ProgImage* img_;
  std::deque<Entry> inflight_;
  std::deque<Request> ready_;
};

// ----------------------------------------------------------------- harness
struct RunResult {
  uint64_t cycles = 0;
  uint64_t commits = 0;
  uint64_t fp_issue = 0;
  uint64_t fp_commit = 0;
  uint64_t fp_flags = 0;
  uint64_t fp_merges = 0;
  uint64_t redirects = 0;
  uint64_t wb_stale = 0;
  std::vector<uint64_t> traps;
  uint64_t csr_fflags = 0;
  uint64_t csr_fcsr = 0;
  uint64_t csr_frm = 0;
  uint64_t mstatus = 0;
  mosaic::MemoryModel mem;

  uint64_t Slot(int index) {
    uint64_t value = 0;
    mem.Read(kSlotBase + 8ull * static_cast<uint64_t>(index), 8, &value);
    return value;
  }
};

class Harness {
 public:
  Harness(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, uint64_t max_cycles,
          const ProgImage* img, mosaic::MemoryModel* mem)
      : dut_(dut), reporter_(reporter), max_cycles_(max_cycles), imem_(img),
        dmem_(mem) {}

  void Phase(const std::string& name) { phase_ = name; }

  uint64_t Cycle(bool rst) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_ + " at cycle " + Dec(cycles_),
           "max-cycles (" + Dec(max_cycles_) + ") exhausted");
    }
    mtime_ = kMtimeBase + cycles_;
    dut_->rst = rst ? 1 : 0;
    dut_->irq_soft_i = 0;
    dut_->irq_timer_i = 0;
    dut_->irq_ext_i = 0;
    dut_->mtime_i = mtime_;
    dut_->imem_req_ready_i = 1;
    dut_->imem_rsp_valid_i = imem_.HasResponse() ? 1 : 0;
    if (imem_.HasResponse()) {
      const Imem::Request& r = imem_.Response();
      dut_->imem_rsp_rdata_i = imem_.ResponseWord();
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = r.id;
      dut_->imem_rsp_epoch_i = r.epoch;
      dut_->imem_rsp_len_i = 4;
    } else {
      dut_->imem_rsp_rdata_i = 0;
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = 0;
      dut_->imem_rsp_epoch_i = 0;
      dut_->imem_rsp_len_i = 0;
    }
    dut_->dmem_req_ready_i = 1;
    if (dmem_.HasResponse()) {
      const DataMem::Rsp& r = dmem_.CurrentResponse();
      dut_->dmem_rsp_valid_i = 1;
      dut_->dmem_rsp_rdata_i = r.rdata;
      dut_->dmem_rsp_fault_i = r.fault ? 1 : 0;
    } else {
      dut_->dmem_rsp_valid_i = 0;
      dut_->dmem_rsp_rdata_i = 0;
      dut_->dmem_rsp_fault_i = 0;
    }
    dut_->arb_req_valid0_i = 0;
    dut_->arb_req_valid1_i = 0;
    dut_->arb_head_valid_i = 0;
    dut_->arb_head_retire_i = 0;
    dut_->eval();

    if (!rst) Observe();

    if ((dut_->imem_req_valid_o != 0) && (dut_->imem_req_ready_i != 0)) {
      imem_.Accept(dut_->imem_req_addr_o, dut_->imem_req_id_o, dut_->imem_req_epoch_o);
    }
    if ((dut_->imem_rsp_valid_i != 0) && (dut_->imem_rsp_ready_o != 0)) {
      imem_.PopResponse();
    }
    imem_.Advance();

    if ((dut_->dmem_req_valid_o != 0) && (dut_->dmem_req_ready_i != 0)) {
      DataMem::Request r;
      r.we = dut_->dmem_req_we_o != 0;
      r.addr = dut_->dmem_req_addr_o;
      r.size = dut_->dmem_req_size_o;
      r.wstrb = dut_->dmem_req_wstrb_o;
      r.wdata = dut_->dmem_req_wdata_o;
      dmem_.Accept(r, cycles_);
    }
    if ((dut_->dmem_rsp_valid_i != 0) && (dut_->dmem_rsp_ready_o != 0)) {
      dmem_.PopResponse();
    }
    dmem_.Advance();

    dut_->clk = 0;
    dut_->eval();
    dut_->clk = 1;
    dut_->eval();
    dut_->clk = 0;
    dut_->eval();
    return ++cycles_;
  }

  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) Cycle(true);
  }

  uint64_t cycles() const { return cycles_; }
  const std::vector<uint64_t>& trap_pcs() const { return trap_pcs_; }

  void Observe() {
    Compare("the retire counter equals the retirement events published",
            dut_->o_commit_o == retires_,
            "counter=" + Dec(dut_->o_commit_o) + " events=" + Dec(retires_));
    if (dut_->o_trap_valid_o != 0) {
      trap_pcs_.push_back(dut_->o_trap_epc_o);
      if (dut_->o_trap_is_irq_o != 0) {
        Fail(phase_ + " at cycle " + Dec(cycles_),
             "an interrupt was taken; the case drives none");
      }
    }
    const uint32_t mask =
        (g_.retire_width >= 32) ? 0xFFFFFFFFu : ((1u << g_.retire_width) - 1u);
    const uint32_t got_mask = static_cast<uint32_t>(dut_->ev_valid_o) & mask;
    for (uint32_t lane = 0; lane < g_.retire_width && lane < 32; lane++) {
      if ((got_mask & (1u << lane)) == 0) continue;
      if (PackedLane(dut_->ev_trap_o, lane, 1) != 0) continue;
      retires_++;
    }
    ProgressCheck();
  }

  void Compare(const std::string& what, bool ok, const std::string& detail) {
    if (!ok) {
      Fail(phase_ + " at cycle " + Dec(cycles_), what + ": " + detail);
    }
    (void)reporter_;
  }

  void ProgressCheck() {
    const bool progressed = (dut_->o_commit_o != last_commit_) ||
                            (dut_->o_trap_valid_o != 0) ||
                            (dut_->o_dbg_alloc_ctr_o != last_alloc_);
    last_commit_ = dut_->o_commit_o;
    last_alloc_ = dut_->o_dbg_alloc_ctr_o;
    if (progressed) {
      last_progress_ = cycles_;
      return;
    }
    if (cycles_ - last_progress_ > kStallCycles) {
      Fail(phase_ + " at cycle " + Dec(cycles_), "stalled");
    }
  }

  static uint64_t PackedLane(uint64_t packed, uint32_t lane, uint32_t width) {
    const uint64_t m = (width >= 64) ? ~0ull : ((1ull << width) - 1ull);
    return (packed >> (lane * width)) & m;
  }

  void SetGeometry(const Geometry& g) {
    g_ = g;
    g_.retire_width = (g_.retire_width == 0) ? 1 : g_.retire_width;
  }

 private:
  static constexpr uint64_t kMtimeBase = 0x100000000ull;
  Vmosaic_core_tb* dut_;
  mosaic::Reporter* reporter_;
  uint64_t max_cycles_;
  Imem imem_;
  DataMem dmem_;
  Geometry g_;
  std::string phase_ = "init";
  uint64_t cycles_ = 0;
  uint64_t retires_ = 0;
  uint64_t mtime_ = 0;
  uint32_t last_commit_ = 0;
  uint32_t last_alloc_ = 0;
  uint64_t last_progress_ = 0;
  std::vector<uint64_t> trap_pcs_;
};

// ------------------------------------------------------------------ program
// One program plus its data block, laid out in the two regions above.
struct Scenario {
  ProgImage image;
  std::vector<uint8_t> data = std::vector<uint8_t>(kDataSize, 0);

  void Put64(uint64_t off, uint64_t value) {
    if (off + 8 > static_cast<uint64_t>(kDataSize)) {
      Fail("scenario data", "word at offset " + Dec(off) + " leaves the block");
    }
    for (unsigned i = 0; i < 8; i++) {
      data[off + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFu);
    }
  }
  // The low 4 bytes of a word, for the 32-bit FP loads and the flw sources.
  void Put32(uint64_t off, uint32_t value) {
    if (off + 4 > static_cast<uint64_t>(kDataSize)) {
      Fail("scenario data", "word at offset " + Dec(off) + " leaves the block");
    }
    for (unsigned i = 0; i < 4; i++) {
      data[off + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFu);
    }
  }
};

RunResult Execute(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                  const Geometry& g, const std::string& name,
                  const Scenario& sc) {
  mosaic::MemoryModel mem;
  for (int i = 0; i < kDataSize; i++) {
    if (sc.data[i] == 0) continue;
    mem.Write(kDataBase + static_cast<uint64_t>(i), 1, sc.data[i]);
  }
  const ProgImage& image = sc.image;

  Harness harness(dut, reporter, kProgramCycles, &image, &mem);
  harness.SetGeometry(g);
  harness.Phase(name);
  dut->clk = 0;
  dut->rst = 1;
  dut->eval();
  harness.Reset(kResetCycles);
  while (!mem.finished() && harness.cycles() < kProgramCycles) {
    harness.Cycle(false);
  }
  for (int i = 0; i < 8; i++) harness.Cycle(false);
  if (!mem.finished()) {
    Fail(name, "the program never reached the exit protocol");
  }

  RunResult out;
  out.cycles = harness.cycles();
  out.commits = dut->o_commit_o;
  out.fp_issue = dut->o_fp_issue_ctr_o;
  out.fp_commit = dut->o_fp_commit_ctr_o;
  out.fp_flags = dut->o_fp_flags_ctr_o;
  out.fp_merges = dut->o_fp_merge_ctr_o;
  out.redirects = dut->o_redirect_o;
  out.wb_stale = dut->o_wb_stale_o;
  out.traps = harness.trap_pcs();
  out.csr_fflags = dut->o_csr_fflags_o;
  out.csr_fcsr = dut->o_csr_fcsr_o;
  out.csr_frm = dut->o_csr_frm_o;
  out.mstatus = dut->o_csr_mstatus_o;
  out.mem = mem;
  return out;
}

// ============================================================================
// Phase 1: arithmetic and its flags, against the host FPU.
// ============================================================================
struct ArithVec {
  bool fmt;        // true = single
  int op;          // 0 add, 1 sub, 2 mul, 3 div
  uint32_t rm;
  uint64_t a;
  uint64_t b;
  const char* note;
};

void PhaseArith(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                const Geometry& g, const std::string& name) {
  const uint32_t f32_one = 0x3F800000u;
  const uint32_t f32_two_pow_m24 = 0x33800000u;   // 2^-24
  const uint32_t f32_max = 0x7F7FFFFFu;           // FLT_MAX
  const uint32_t f32_min_norm = 0x00800000u;      // FLT_MIN
  const uint32_t f32_half = 0x3F000000u;
  const uint32_t f32_snan = 0x7F800001u;
  const uint32_t f32_qnan = 0x7FC00001u;
  const uint32_t f32_third = 0x3EAAAAABu;         // 1/3 as a float
  const uint32_t f32_neg_two = 0xC0000000u;
  const uint32_t f32_one_point_five = 0x3FC00000u;

  const std::vector<ArithVec> vecs = {
      {true, 0, 0, f32_one, f32_two_pow_m24, "fadd.s 1.0 + 2^-24 RNE: an exact tie to even"},
      {true, 0, 3, f32_one, f32_two_pow_m24, "fadd.s 1.0 + 2^-24 RUP: the tie goes up"},
      {true, 0, 2, f32_one, f32_two_pow_m24, "fadd.s 1.0 + 2^-24 RDN: the tie goes down"},
      {true, 1, 1, f32_one, f32_two_pow_m24, "fsub.s 1.0 - 2^-24 RTZ"},
      {false, 0, 0, F64Bits(1.0), F64Bits(std::ldexp(1.0, -53)), "fadd.d 1.0 + 2^-53 RNE: a double tie"},
      {true, 0, 0, f32_neg_two & 0x80000000u, f32_neg_two & 0x80000000u, "fadd.s -0.0 + -0.0 = -0.0"},
      {true, 0, 0, f32_one, f32_one ^ 0x80000000u, "fadd.s 1.0 + -1.0 = +0.0"},
      {true, 2, 0, f32_max, F32Bits(2.0f), "fmul.s FLT_MAX * 2 = +inf, OF|NX"},
      {false, 2, 0, F64Bits(1.7976931348623157e308), F64Bits(2.0), "fmul.d DBL_MAX * 2 = +inf, OF|NX"},
      {true, 3, 0, f32_one, 0u, "fdiv.s 1.0 / 0.0 = +inf, DZ"},
      {true, 3, 0, 0u, 0u, "fdiv.s 0.0 / 0.0 = canonical NaN, NV"},
      {true, 2, 0, f32_min_norm, f32_half, "fmul.s FLT_MIN * 0.5: subnormal result, UF|NX"},
      {true, 0, 0, f32_snan, f32_one, "fadd.s sNaN + 1.0: canonical NaN, NV"},
      {true, 0, 0, f32_qnan, f32_one, "fadd.s qNaN + 1.0: canonical NaN, no NV"},
      {false, 0, 0, F64Bits(HUGE_VAL), F64Bits(-HUGE_VAL), "fadd.d inf + -inf: canonical NaN, NV"},
      {true, 3, 0, f32_one, f32_third, "fdiv.s 1.0 / (1/3): NX"},
      {false, 3, 1, F64Bits(1.0), F64Bits(3.0), "fdiv.d 1.0 / 3.0 RTZ: NX"},
      {true, 2, 0, f32_one_point_five, f32_neg_two, "fmul.s 1.5 * -2.0 = -3.0, exact"},
      {false, 1, 3, F64Bits(1.0), F64Bits(-std::ldexp(1.0, -53)), "fsub.d 1.0 - -2^-53 RUP: a tie up"},
  };

  Scenario sc;
  Asm asm_(g.reset_vector);
  asm_.LaAbs(31, kDataBase);
  asm_.LaAbs(30, kSlotBase);
  // The FS field starts Off in p0's reset state; this case does not rely on
  // that (no profile here traps on FS=Off), but the FS phase below needs a
  // defined starting point, so it is set explicitly there.
  for (size_t i = 0; i < vecs.size(); i++) {
    const ArithVec& v = vecs[i];
    const uint64_t off = 16ull * i;
    sc.Put64(off, v.a);
    sc.Put64(off + 8, v.b);
    asm_.Ld(5, 31, static_cast<int32_t>(off));
    asm_.Ld(6, 31, static_cast<int32_t>(off + 8));
    if (v.fmt) {
      asm_.FmvWX(1, 5);
      asm_.FmvWX(2, 6);
    } else {
      asm_.FmvDX(1, 5);
      asm_.FmvDX(2, 6);
    }
    asm_.Csrrw(0, 0x001, 0);  // fflags <- 0
    asm_.Farith(v.op, v.fmt, 3, 1, 2, v.rm);
    asm_.Csrr(7, 0x001);
    asm_.Sd(7, 30, static_cast<int32_t>(16 * i));      // slot 2i: the flags
    if (v.fmt) {
      asm_.FmvXW(7, 3);
    } else {
      asm_.FmvXD(7, 3);
    }
    asm_.Sd(7, 30, static_cast<int32_t>(16 * i + 8));  // slot 2i+1: the value
  }
  asm_.Exit();
  for (size_t i = 0; i < asm_.words().size(); i++) {
    sc.image.Put(g.reset_vector + 4ull * i, asm_.words()[i]);
  }

  RunResult run = Execute(dut, reporter, g, name, sc);
  Check(reporter, run.traps.empty(),
                  name + ": no FP instruction traps");
  Check(reporter, run.fp_issue >= vecs.size(),
                  name + ": every vector reached the shared FP unit (" +
                      Dec(run.fp_issue) + " issued)");
  for (size_t i = 0; i < vecs.size(); i++) {
    const ArithVec& v = vecs[i];
    const HostResult host = HostArith(v.op, v.fmt, v.a, v.b, v.rm);
    // The ISA lets an implementation return the canonical quiet NaN for any
    // NaN-producing operation; this design documents exactly that, so a NaN
    // result is compared against the canonical NaN rather than the host's
    // payload-preserving one. Everything else is bit-for-bit the host's.
    const uint64_t want_bits = host.is_nan ? CanonNaN(v.fmt) : host.bits;
    const uint64_t host_flags = host.flags;
    uint64_t got_bits = run.Slot(static_cast<int>(2 * i + 1));
    if (v.fmt) got_bits &= 0xFFFFFFFFull;
    const uint64_t want_shown = v.fmt ? (want_bits & 0xFFFFFFFFull) : want_bits;
    Check(reporter, got_bits == want_shown,
                    name + " vector " + Dec(i) + " (" + v.note + "): bits -- got " +
                        U64(got_bits) + ", host " + U64(want_shown));
    const uint64_t got_flags = run.Slot(static_cast<int>(2 * i)) & 0x1F;
    Check(reporter, got_flags == host_flags,
                    name + " vector " + Dec(i) + " (" + v.note + "): fflags -- got " +
                        FlagStr(static_cast<uint32_t>(got_flags)) + ", host " +
                        FlagStr(static_cast<uint32_t>(host_flags)));
  }
}

// ============================================================================
// Phase 2: NaN-boxing, unboxing, the NaN policy (bits), signed zero.
// ============================================================================
void PhaseBoxNaN(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                 const Geometry& g, const std::string& name) {
  Scenario sc;
  Asm asm_(g.reset_vector);
  asm_.LaAbs(31, kDataBase);
  asm_.LaAbs(30, kSlotBase);
  // The data block: three 64-bit patterns.
  sc.Put64(0, 0x0000'0000'3F80'0000ull);   // an UNBOXED single 1.0
  sc.Put32(8, 0x40490FDBu);                // pi as a float, in a word load
  sc.Put64(16, 0xFFFF'FFFF'3F80'0000ull);  // the same 1.0, boxed
  sc.Put64(24, 0x8000'0000'0000'0000ull);  // -0.0 as a double

  sc.Put64(32, 0x0000'0000'7FC0'0001ull);  // an unboxed signalling NaN payload
  sc.Put64(40, 0xFFFF'FFFF'7F80'0001ull);  // a boxed signalling NaN
  sc.Put64(48, 0xFFFF'FFFF'7FC0'0001ull);  // a boxed quiet NaN
  sc.Put64(56, 0xFFFF'FFFF'FF80'0000ull);  // -inf as a boxed single

  // (1) fmv.w.x boxes: the destination's upper half is all ones, even though the
  //     integer source's upper half is not.
  asm_.Ld(5, 31, 0);
  asm_.FmvWX(1, 5);
  asm_.FmvXD(7, 1);
  asm_.Sd(7, 30, 0);                       // slot 0: 0xFFFFFFFF_3F800000

  // (2) ... and the register now holds a *number*: fadd.s of it with itself is
  //     2.0, not a NaN. This is the positive half of the unboxing rule.
  asm_.Csrrw(0, 0x001, 0);
  asm_.Farith(0, true, 3, 1, 1, 0);
  asm_.FmvXD(7, 3);
  asm_.Sd(7, 30, 8);                       // slot 1: 0xFFFFFFFF_40000000
  asm_.Csrr(7, 0x001);
  asm_.Sd(7, 30, 16);                      // slot 2: no flags

  // (3) an UNBOXED operand is the canonical quiet NaN: fld of 0x00000000_3F800000
  //     then fadd.s with itself. The unboxing raises no NV by itself; the add of
  //     two quiet NaNs raises none either.
  asm_.Fld(4, 31, 0);
  asm_.Csrrw(0, 0x001, 0);
  asm_.Farith(0, true, 5, 4, 4, 0);
  asm_.FmvXD(7, 5);
  asm_.Sd(7, 30, 24);                      // slot 3: the boxed canonical qNaN
  asm_.Csrr(7, 0x001);
  asm_.Sd(7, 30, 32);                      // slot 4: no flags

  // (4) flw boxes a word load; fsw stores the low word back through memory.
  asm_.Flw(6, 31, 8);
  asm_.FmvXD(7, 6);
  asm_.Sd(7, 30, 40);                      // slot 5: 0xFFFFFFFF_40490FDB
  asm_.Fsw(6, 31, 64);                     // memory word at data+64
  asm_.Fsd(6, 30, 48);                     // slot 6: the full boxed register

  // (5) fld of a boxed single is unchanged; a double keeps its bits.
  asm_.Fld(8, 31, 16);
  asm_.FmvXD(7, 8);
  asm_.Sd(7, 30, 56);                      // slot 7: identical to the loaded bits

  // (6) signed zero: fsgnj.d of -0.0 with itself is -0.0 (a bit-level op).
  asm_.Fld(9, 31, 24);
  asm_.Fp(0x11, 0, 10, 9, 9);              // fsgnj.d
  asm_.FmvXD(7, 10);
  asm_.Sd(7, 30, 64);                      // slot 8: 0x8000000000000000

  // (7) an sNaN operand sets NV: fadd.s with a boxed signalling NaN.
  asm_.Fld(11, 31, 40);
  asm_.Csrrw(0, 0x001, 0);
  asm_.Farith(0, true, 12, 11, 11, 0);
  asm_.FmvXD(7, 12);
  asm_.Sd(7, 30, 72);                      // slot 9: canonical qNaN
  asm_.Csrr(7, 0x001);
  asm_.Sd(7, 30, 80);                      // slot 10: NV

  // (8) a quiet NaN operand does not.
  asm_.Fld(13, 31, 48);
  asm_.Csrrw(0, 0x001, 0);
  asm_.Farith(0, true, 14, 13, 13, 0);
  asm_.FmvXD(7, 14);
  asm_.Sd(7, 30, 88);                      // slot 11: canonical qNaN
  asm_.Csrr(7, 0x001);
  asm_.Sd(7, 30, 96);                      // slot 12: no flags

  // (9) a NaN operand of fsgnj is a move, not an arithmetic NaN: the bits pass
  //     through and no flag is raised.
  asm_.Fld(15, 31, 56);                    // -inf, boxed
  asm_.Csrrw(0, 0x001, 0);
  asm_.Fp(0x10, 1, 16, 15, 15);            // fsgnjn.s f16 = -(-inf) = +inf
  asm_.FmvXD(7, 16);
  asm_.Sd(7, 30, 104);                     // slot 13: 0xFFFFFFFF_7F800000
  asm_.Csrr(7, 0x001);
  asm_.Sd(7, 30, 112);                     // slot 14: no flags
  asm_.Exit();
  for (size_t i = 0; i < asm_.words().size(); i++) {
    sc.image.Put(g.reset_vector + 4ull * i, asm_.words()[i]);
  }

  RunResult run = Execute(dut, reporter, g, name, sc);
  Check(reporter, run.traps.empty(), name + ": no FP instruction traps");

  struct Expect {
    int slot;
    uint64_t want;
    const char* note;
  };
  const std::vector<Expect> expects = {
      {0, 0xFFFF'FFFF'3F80'0000ull, "fmv.w.x boxes the destination"},
      {1, 0xFFFF'FFFF'4000'0000ull, "the boxed register holds 1.0, so 1.0+1.0=2.0"},
      {2, 0, "a boxed operand is not a NaN: no flag"},
      {3, CanonNaN(true), "an unboxed operand is the canonical quiet NaN"},
      {4, 0, "unboxing a non-NaN raises no flag"},
      {5, 0xFFFF'FFFF'4049'0FDBull, "flw boxes its word load"},
      {6, 0xFFFF'FFFF'4049'0FDBull, "fsd stores the register's own bits"},
      {7, 0xFFFF'FFFF'3F80'0000ull, "fld loads 64 bits unchanged"},
      {8, 0x8000'0000'0000'0000ull, "fsgnj.d of -0.0 with itself is -0.0"},
      {9, CanonNaN(true), "an sNaN operand yields the canonical NaN"},
      {10, FL_NV, "an sNaN operand sets NV"},
      {11, CanonNaN(true), "a quiet NaN operand yields the canonical NaN"},
      {12, 0, "a quiet NaN operand sets no flag"},
      {13, 0xFFFF'FFFF'7F80'0000ull, "fsgnjn.s of -inf gives +inf: a move, not arithmetic"},
      {14, 0, "fsgnj raises no flag"},
  };
  for (const Expect& e : expects) {
    const uint64_t got = run.Slot(e.slot);
    Check(reporter, got == e.want,
                    name + " slot " + Dec(static_cast<uint64_t>(e.slot)) + " (" +
                        e.note + "): got " + U64(got) + ", want " + U64(e.want));
  }
  // The fsw's effect on memory, read through the model and not through the
  // register file.
  uint64_t stored = 0;
  run.mem.Read(kDataBase + 64, 4, &stored);
  Check(reporter, stored == 0x40490FDBull,
                  name + " fsw stores the register's low word: got " + U64(stored));
}

// ============================================================================
// Phase 3: conversions -- the ISA's saturation rule, NV and NX.
// ============================================================================
void PhaseCvt(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
              const Geometry& g, const std::string& name) {
  Scenario sc;
  Asm asm_(g.reset_vector);
  asm_.LaAbs(31, kDataBase);
  asm_.LaAbs(30, kSlotBase);
  // A pool of source values, each in the low 32 bits of a word (single format).
  const std::vector<uint64_t> src = {
      0x0000'0000'5015'02F9ull,  // 1e10f
      0x0000'0000'D015'02F9ull,  // -1e10f
      0x0000'0000'7FC0'0001ull,  // a quiet NaN
      0x0000'0000'3FC0'0000ull,  // 1.5f
      0x0000'0000'7F80'0000ull,  // +inf
      0x0000'0000'BF00'0000ull,  // -0.5f
      0x0000'0000'BF80'0000ull,  // -1.0f
  };
  for (size_t i = 0; i < src.size(); i++) sc.Put64(8ull * i, src[i]);
  // 16777217 as an int32 (a value single precision cannot hold exactly).
  sc.Put64(64, 16777217ull);

  struct Case {
    int kind;      // 0 = fp->int, 1 = int->fp
    uint32_t sel;  // the rs2 field (0 w, 1 wu, 2 l, 3 lu)
    int src_index;
    uint32_t rm;
    uint64_t want;
    uint32_t flags;
    const char* note;
  };
  // The saturation values and their NV are the ISA's rule, which C does not
  // have (a cast out of range is undefined in C), so these expectations are
  // written from the spec: a signed destination saturates to its extreme, an
  // unsigned destination saturates to 0 for a negative value and to UINT_MAX
  // above the range, and the rounded value must be nonzero for a negative
  // unsigned result to raise NV. The one in-range int->fp vector uses the host.
  const std::vector<Case> cases = {
      {0, 0, 0, 1, 0x0000'0000'7FFF'FFFFull, FL_NV, "fcvt.w.s 1e10 saturates to INT32_MAX with NV"},
      {0, 0, 1, 1, 0xFFFF'FFFF'8000'0000ull, FL_NV, "fcvt.w.s -1e10 saturates to INT32_MIN with NV"},
      {0, 0, 2, 1, 0x0000'0000'7FFF'FFFFull, FL_NV, "fcvt.w.s NaN saturates and sets NV"},
      {0, 1, 4, 1, 0x0000'0000'FFFF'FFFFull, FL_NV, "fcvt.wu.s +inf saturates to UINT32_MAX with NV"},
      {0, 1, 6, 1, 0x0000'0000'0000'0000ull, FL_NV, "fcvt.wu.s -1.0 saturates to 0 with NV"},
      {0, 1, 5, 1, 0x0000'0000'0000'0000ull, 0, "fcvt.wu.s -0.5 RTZ rounds to 0 with no exception"},
      {0, 0, 3, 1, 0x0000'0000'0000'0001ull, FL_NX, "fcvt.w.s 1.5 RTZ = 1 with NX"},
      {1, 0, 0, 1, 0xFFFF'FFFF'4B80'0000ull, FL_NX, "fcvt.s.w 16777217 rounds to 2^24, NX"},
  };

  for (size_t i = 0; i < cases.size(); i++) {
    const Case& c = cases[i];
    const int flag_slot = static_cast<int>(2 * i);
    const int value_slot = static_cast<int>(2 * i + 1);
    if (c.kind == 1) {
      asm_.Ld(5, 31, 64);
      asm_.Csrrw(0, 0x001, 0);
      asm_.FCvtIF(true, c.sel, 1, 5, c.rm);
      asm_.FmvXD(7, 1);
      asm_.Sd(7, 30, static_cast<int32_t>(8 * value_slot));
      asm_.Csrr(7, 0x001);
      asm_.Sd(7, 30, static_cast<int32_t>(8 * flag_slot));
      continue;
    }
    asm_.Ld(5, 31, static_cast<int32_t>(8 * c.src_index));
    asm_.FmvWX(1, 5);
    asm_.Csrrw(0, 0x001, 0);
    asm_.FCvtFI(true, c.sel, 7, 1, c.rm);
    asm_.Sd(7, 30, static_cast<int32_t>(8 * value_slot));
    asm_.Csrr(7, 0x001);
    asm_.Sd(7, 30, static_cast<int32_t>(8 * flag_slot));
  }
  asm_.Exit();
  for (size_t i = 0; i < asm_.words().size(); i++) {
    sc.image.Put(g.reset_vector + 4ull * i, asm_.words()[i]);
  }

  RunResult run = Execute(dut, reporter, g, name, sc);
  Check(reporter, run.traps.empty(), name + ": no conversion traps");
  for (size_t i = 0; i < cases.size(); i++) {
    const Case& c = cases[i];
    // The int->fp vector's expectation is the host's, not a transcribed number.
    uint64_t want = c.want;
    uint32_t want_flags = c.flags;
    if (c.kind == 1) {
      const HostResult host = HostIntToFp(true, 16777217ull, true, c.rm);
      want = host.bits & 0xFFFFFFFFull;
      want_flags = host.flags;
    }
    uint64_t got_value = run.Slot(static_cast<int>(2 * i + 1));
    // An int->fp result is NaN-boxed in the register, as every single-precision
    // result is; the value under test is its low half.
    if (c.kind == 1) got_value &= 0xFFFFFFFFull;
    const uint64_t got_flags = run.Slot(static_cast<int>(2 * i)) & 0x1F;
    Check(reporter, got_value == want,
                    name + " " + c.note + ": got " + U64(got_value) + ", want " +
                        U64(want));
    Check(reporter, got_flags == want_flags,
                    name + " " + c.note + ": fflags -- got " +
                        FlagStr(static_cast<uint32_t>(got_flags)) + ", want " +
                        FlagStr(want_flags));
  }
}

// ============================================================================
// Phase 4: flw/fsw/fld/fsd through the memory path.
// ============================================================================
void PhaseMem(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
              const Geometry& g, const std::string& name) {
  Scenario sc;
  Asm asm_(g.reset_vector);
  asm_.LaAbs(31, kDataBase);
  asm_.LaAbs(30, kSlotBase);
  sc.Put32(0, 0x40490FDBu);                       // pi, as a word
  sc.Put64(8, 0x4009'21FB'5444'2D18ull);          // pi, as a double

  // flw of a word: a boxed single.
  asm_.Flw(1, 31, 0);
  // fsw of it into a second word.
  asm_.Fsw(1, 31, 16);
  // ... read the word back with an integer load and store the 32-bit pattern.
  asm_.Lw(7, 31, 16);
  asm_.Sd(7, 30, 0);                              // slot 0: 0x0000000040490FDB
  // flw of the stored word: the same boxed single.
  asm_.Flw(2, 31, 16);
  asm_.FmvXD(7, 2);
  asm_.Sd(7, 30, 8);                              // slot 1: boxed pi
  // fld of the double: unboxed, exact bits.
  asm_.Fld(3, 31, 8);
  asm_.FmvXD(7, 3);
  asm_.Sd(7, 30, 16);                             // slot 2: the double's bits
  // fsd of the double into a scratch word, then read it back as 64 bits.
  asm_.Fsd(3, 31, 24);
  asm_.Ld(7, 31, 24);
  asm_.Sd(7, 30, 24);                             // slot 3: the same bits
  // A double fadd through the memory path: 1.0 + 2.0 = 3.0, from memory to
  // memory, with no integer register carrying the value in between.
  sc.Put64(32, F64Bits(1.0));
  sc.Put64(40, F64Bits(2.0));
  asm_.Fld(4, 31, 32);
  asm_.Fld(5, 31, 40);
  asm_.Farith(0, false, 6, 4, 5, 0);
  asm_.Fsd(6, 30, 32);                            // slot 4: 3.0 as a double
  asm_.Exit();
  for (size_t i = 0; i < asm_.words().size(); i++) {
    sc.image.Put(g.reset_vector + 4ull * i, asm_.words()[i]);
  }

  RunResult run = Execute(dut, reporter, g, name, sc);
  Check(reporter, run.traps.empty(), name + ": no memory-path FP instruction traps");
  Check(reporter, run.Slot(0) == 0x40490FDBull,
                  name + " fsw wrote the register's low word: got " +
                      U64(run.Slot(0)));
  Check(reporter, run.Slot(1) == 0xFFFF'FFFF'4049'0FDBull,
                  name + " flw of the stored word is boxed: got " + U64(run.Slot(1)));
  Check(reporter, run.Slot(2) == 0x4009'21FB'5444'2D18ull,
                  name + " fld loads 64 bits unboxed: got " + U64(run.Slot(2)));
  Check(reporter, run.Slot(3) == 0x4009'21FB'5444'2D18ull,
                  name + " fsd/ld round-trip the double's bits: got " +
                      U64(run.Slot(3)));
  Check(reporter, run.Slot(4) == F64Bits(3.0),
                  name + " a double add through memory is exact: got " +
                      U64(run.Slot(4)));
}

// ============================================================================
// Phase 5: a squashed FP operation contributes nothing.
// ============================================================================
// How the wrong path is constructed, and why it is a *trap* and not a branch.
//
// The obvious construction -- an FP operation fetched behind a mispredicted
// branch -- cannot execute in this machine, and that is a property of the
// machine, not of the stimulus. `mosaic_dispatch`'s `barrier` is driven by
// `br_inflight` (mosaic_core.sv: `.barrier(br_inflight | wfi_halt)`), and the
// barrier holds allocation for every instruction younger than an unresolved
// branch. So while a branch is in flight *nothing younger than it is allocated*,
// which means nothing younger than it can issue, execute, complete or flag. The
// first version of this phase drove exactly that and observed zero flagged
// completions: not a stimulus that was too short, but a machine in which a
// branch mispredict can only ever discard *fetched-but-unallocated* work.
//
// A synchronous trap has no such barrier. The faulting instruction is
// allocated, issued and (for a load fault) carries its exception on its ROB
// entry; younger instructions allocate, issue and execute normally, and the
// trap is taken when the faulting instruction reaches the head. Everything
// younger is then squashed by the same `rob_flush_pulse` a redirect uses. So the
// wrong path here is:
//
//   * an OLDER load to an unmapped address (0), whose precise access fault is
//     taken at retire;
//   * an OLDER, 64-iteration integer divide in front of it, so the faulting load
//     cannot reach the head -- and therefore cannot trap -- for ~65 cycles;
//   * a YOUNGER `fmul.s f5, f1, f2` (FLT_MAX * 2 = +inf, OF|NX) that is
//     allocated, issued, executed and COMPLETES, producing and recording its
//     flags, well inside that window;
//   * the trap at the load's retire, which squashes the multiply and clears the
//     recorded flags before its slot can retire.
//
// The phase then proves, in the trap handler, that the architectural `fflags`
// and `f5` are exactly as they were before the multiply -- and the anti-vacuity
// check requires the multiply to have *completed with flags*, so a machine that
// merely never ran it cannot pass.
void PhaseWrongPath(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                    const Geometry& g, const std::string& name) {
  Scenario sc;
  Asm asm_(g.reset_vector);
  // The trap handler lives at a fixed offset past the main body, so the
  // `LaAbs` that loads its address into `mtvec` can name it before it is
  // emitted. The main body ends in its own exit spin and never falls into it.
  const uint64_t handler_pc = g.reset_vector + 0x400;
  Asm handler_(handler_pc);
  asm_.LaAbs(31, kDataBase);
  asm_.LaAbs(30, kSlotBase);
  asm_.LaAbs(28, handler_pc);
  asm_.Csrrw(0, 0x305, 28);                 // mtvec <- handler (direct mode)
  // FLT_MAX and 2.0 as *boxed* singles, and the two integer operands of the
  // gating divide. The single operands must be boxed: an unboxed value is not a
  // single-precision number, it is the canonical NaN.
  sc.Put64(0, 0xFFFF'FFFF'7F7F'FFFFull);   // FLT_MAX
  sc.Put64(8, 0xFFFF'FFFF'4000'0000ull);   // 2.0
  sc.Put64(16, 0);                          // the integer zero
  sc.Put64(24, 1);                          // the integer one

  asm_.Fld(1, 31, 0);       // f1 = FLT_MAX
  asm_.Fld(2, 31, 8);       // f2 = 2.0
  asm_.Csrrw(0, 0x001, 0);  // fflags <- 0
  asm_.Csrr(7, 0x001);
  asm_.Sd(7, 30, 0);        // slot 0: fflags before the wrong path, must be 0
  asm_.FmvXW(7, 5);
  asm_.Sd(7, 30, 8);        // slot 1: f5 before the wrong path
  // The delay: a 32-iteration `divw`. It is an *integer* operation, so it does
  // not occupy the shared FP unit, and it is older than the faulting load, so
  // retirement order keeps the load -- and its trap -- behind it. Its latency is
  // chosen so the trap lands after the multiply below has completed but well
  // before the long FP divide below that has completed: the window is where the
  // squash has something completed to discard *and* something in flight to
  // cancel.
  asm_.Ld(2, 31, 16);
  asm_.Ld(3, 31, 24);
  asm_.Divw(1, 2, 3);                       // x1 = 0, after the divide latency
  // The faulting instruction: a *misaligned* load. The core decides a load's
  // misalignment from the address alone, before the memory map is consulted, so
  // the fault is deterministic and does not depend on the harness's map or on
  // the device path. (Address 0 would not do: it is the boot ROM, which is
  // readable, so a load there does not fault.)
  const uint64_t fault_pc = asm_.pc();
  asm_.Ld(5, 31, 1);        // ld x5, 1(x31): misaligned -> precise load fault
  // The wrong-path FP operations, younger, and free to allocate and execute
  // because a load is not an allocation barrier.
  //
  //   1. `fmul.s f5, f1, f2` is short. It issues first, COMPLETES inside the
  //      divide's window and produces OF|NX -- the completed-and-flagged
  //      operation the phase's anti-vacuity check requires.
  //   2. `fdiv.s f6, f1, f2` is 65 iterative steps. It issues behind the
  //      multiply and is still IN FLIGHT when the trap fires, so the shipping
  //      machine cancels it: no writeback is published for it. The
  //      FP_SQUASH_WRITES control removes that cancellation, and the operation
  //      then completes after the squash and publishes a stale writeback.
  //
  // The multiply's result is deliberately not the canonical NaN, so "the
  // squashed operation wrote its destination" is distinguishable from "the
  // destination was never written" (the latter reads as the canonical NaN).
  asm_.Farith(2, true, 5, 1, 2, 0);         // fmul.s f5 = FLT_MAX * 2 = +inf
  asm_.Farith(3, true, 6, 1, 2, 0);         // fdiv.s f6, in flight at the trap
  // The no-trap path: reachable only if the load above did NOT trap. It writes
  // the same two slots the handler does, so the claim checks below read a real
  // observation either way -- and a machine that never trapped fails them
  // explicitly (its fflags would be OF|NX and its f5 would be +inf) rather than
  // passing on unwritten memory.
  asm_.Csrr(7, 0x001);
  asm_.Sd(7, 30, 16);       // slot 2
  asm_.FmvXW(7, 5);
  asm_.Sd(7, 30, 24);       // slot 3
  asm_.Exit();
  // The handler observes the architectural state *after* the trap's squash.
  handler_.Csrr(7, 0x001);
  handler_.Sd(7, 30, 16);   // slot 2: fflags after the trap, must be 0
  handler_.FmvXW(7, 5);
  handler_.Sd(7, 30, 24);   // slot 3: f5 after the trap
  handler_.Exit();
  for (size_t i = 0; i < asm_.words().size(); i++) {
    sc.image.Put(g.reset_vector + 4ull * i, asm_.words()[i]);
  }
  for (size_t i = 0; i < handler_.words().size(); i++) {
    sc.image.Put(handler_pc + 4ull * i, handler_.words()[i]);
  }

  RunResult run = Execute(dut, reporter, g, name, sc);
  const uint64_t unwritten_single = 0x0000'0000'7FC0'0000ull;  // f5 as a single
  // The trap happened, and it is the load's -- not a stray trap from the
  // handler or the reset path.
  Check(reporter, run.traps.size() == 1 && run.traps[0] == fault_pc,
                  name + ": exactly the faulting load trapped at " + U64(fault_pc) +
                      " (got " + Dec(run.traps.size()) + " trap(s))");
  // The state the phase starts from.
  Check(reporter, run.Slot(0) == 0, name + ": fflags are clear before the wrong path");
  Check(reporter, run.Slot(1) == unwritten_single,
                  name + ": f5 is architecturally unwritten before the wrong path "
                         "(reading as the canonical NaN single): got " +
                      U64(run.Slot(1)));
  // The anti-vacuity checks. These are what make the phase evidence: the
  // squashed operation must have EXECUTED and produced its OF|NX, and the trap
  // must really have redirected. "No flags appeared" is otherwise also true of a
  // machine that never ran the instruction.
  Check(reporter, run.fp_flags >= 1,
                  name + ": the wrong-path operation executed and produced its "
                         "flags (" + Dec(run.fp_flags) + " flagged completions); "
                         "without this the phase would prove nothing");
  Check(reporter, run.redirects >= 1,
                  name + ": the trap redirected (" + Dec(run.redirects) + ")");
  // Anti-vacuity for the cancellation half: the long FP divide must have been
  // ACCEPTED by the FP unit (issued) but never completed -- so at the flush the
  // unit really held an operation in flight. If it had completed too, the
  // FP_SQUASH_WRITES control below would have nothing to catch.
  Check(reporter, run.fp_issue >= run.fp_commit + 1,
                  name + ": an FP operation was in flight at the squash (" +
                      Dec(run.fp_issue) + " issued, " + Dec(run.fp_commit) +
                      " completed)");
  // The claim.
  Check(reporter, run.Slot(2) == 0,
                  name + ": a squashed FP operation contributes no fflags -- got " +
                      FlagStr(static_cast<uint32_t>(run.Slot(2) & 0x1F)));
  Check(reporter, run.Slot(3) == unwritten_single,
                  name + ": a squashed FP operation writes no destination -- f5 "
                         "reads " + U64(run.Slot(3)) + ", and +inf would mean the "
                         "squashed multiply had installed its result");
  // The mechanism: the squashed operation's result must not be handed to the
  // register file at all. The trap cancels the unit's in-flight operation and
  // invalidates its latched destination, so no stale writeback is published.
  Check(reporter, run.wb_stale == 0,
                  name + ": no writeback is published for a squashed operation "
                         "(stale " + Dec(run.wb_stale) + ")");
}

// ============================================================================
// Phase 6: mstatus.FS dirtying.
// ============================================================================
void PhaseFsDirty(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                  const Geometry& g, const std::string& name) {
  Scenario sc;
  Asm asm_(g.reset_vector);
  asm_.LaAbs(31, kDataBase);
  asm_.LaAbs(30, kSlotBase);
  sc.Put64(0, 0xFFFF'FFFF'3F80'0000ull);   // boxed 1.0
  sc.Put64(8, 0x0000'0000'0000'2000ull);   // mstatus with FS = Initial (01)

  asm_.Fld(1, 31, 0);                      // an FP write: dirties FS
  asm_.Ld(5, 31, 8);
  asm_.Csrrw(0, 0x300, 5);                 // mstatus <- FS=Initial (MPP preserved)
  asm_.Csrr(7, 0x300);
  asm_.Sd(7, 30, 0);                       // slot 0: FS = 01
  asm_.FmvXW(7, 1);                        // an FP *read* -- it must not dirty
  asm_.Csrr(7, 0x300);
  asm_.Sd(7, 30, 8);                       // slot 1: FS still 01
  asm_.Farith(0, true, 3, 1, 1, 0);        // an FP write
  asm_.Csrr(7, 0x300);
  asm_.Sd(7, 30, 16);                      // slot 2: FS = 11 (Dirty)
  asm_.Csrrw(0, 0x300, 5);                 // back to Initial
  asm_.Fsw(3, 31, 16);                     // an FP *store* -- it must not dirty
  asm_.Csrr(7, 0x300);
  asm_.Sd(7, 30, 24);                      // slot 3: FS still 01
  asm_.Exit();
  for (size_t i = 0; i < asm_.words().size(); i++) {
    sc.image.Put(g.reset_vector + 4ull * i, asm_.words()[i]);
  }

  RunResult run = Execute(dut, reporter, g, name, sc);
  Check(reporter, run.traps.empty(), name + ": no trap");
  const uint64_t fs_initial = 0x0000'0000'0000'3800ull;  // FS=01 | MPP=M (0x1800)
  const uint64_t fs_dirty = 0x0000'0000'0000'7800ull;    // FS=11 | MPP=M
  Check(reporter, run.Slot(0) == fs_initial,
                  name + ": a software write of FS=Initial lands: mstatus reads " +
                      U64(run.Slot(0)));
  Check(reporter, run.Slot(1) == fs_initial,
                  name + ": fmv.x.w does not dirty mstatus.FS: reads " +
                      U64(run.Slot(1)));
  Check(reporter, run.Slot(2) == fs_dirty,
                  name + ": an FP write sets mstatus.FS = Dirty: reads " +
                      U64(run.Slot(2)));
  Check(reporter, run.Slot(3) == fs_initial,
                  name + ": fsw does not dirty mstatus.FS: reads " + U64(run.Slot(3)));
}

// ============================================================================
// Phase 7: the rounding mode -- the instruction's rm field, and frm when the
// field says "dynamic".
// ============================================================================
void PhaseFrm(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
              const Geometry& g, const std::string& name) {
  // The tie vector: 1.0 + 2^-24 is exactly between two singles. RNE and RDN and
  // RTZ give 1.0; RUP gives the next single up.
  const uint32_t f32_one = 0x3F800000u;
  const uint32_t f32_tie = 0x33800000u;

  Scenario sc;
  Asm asm_(g.reset_vector);
  asm_.LaAbs(31, kDataBase);
  asm_.LaAbs(30, kSlotBase);
  // (a) the instruction's own rm field, with frm left at RNE.
  sc.Put64(0, 0xFFFF'FFFF'0000'0000ull | f32_one);
  sc.Put64(8, 0xFFFF'FFFF'0000'0000ull | f32_tie);
  sc.Put64(16, 3);    // frm <- RUP (dynamic)
  sc.Put64(24, 0);    // frm <- RNE (dynamic)
  for (int mode = 0; mode < 4; mode++) {
    // One copy of the operand pair per mode, so the dynamic test can index the
    // pool by the mode the CSR returned -- which is what forces the FP operation
    // to be ordered behind the CSR write that sets it.
    sc.Put64(32ull + 16ull * mode, 0xFFFF'FFFF'0000'0000ull | f32_one);
    sc.Put64(40ull + 16ull * mode, 0xFFFF'FFFF'0000'0000ull | f32_tie);
  }

  asm_.Fld(1, 31, 0);
  asm_.Fld(2, 31, 8);
  for (uint32_t rm = 0; rm < 4; rm++) {
    asm_.Csrrw(0, 0x001, 0);
    asm_.Farith(0, true, 3, 1, 2, rm);
    asm_.Csrr(7, 0x001);
    asm_.Sd(7, 30, static_cast<int32_t>(8 * rm));
    asm_.FmvXW(7, 3);
    asm_.Sd(7, 30, static_cast<int32_t>(32 + 8 * rm));
  }
  // (b) the dynamic mode. `frm` is written, read back and used as the pool
  //     index, so the FP operation cannot issue before the CSR write has
  //     retired: the dependency is what makes this ordering deterministic
  //     rather than a race against a CSR that is not renamed.
  asm_.Ld(5, 31, 16);              // the mode to install
  asm_.Csrrw(0, 0x002, 5);         // frm <- 3 (RUP)
  asm_.Csrr(9, 0x002);             // read it back
  asm_.Sd(9, 30, 64);              // slot 8: frm reads back as written
  asm_.Slli(11, 9, 4);             // 16 * mode
  asm_.Add(11, 11, 31);            // the pool entry for this mode
  asm_.Fld(4, 11, 0);
  asm_.Fld(5, 11, 8);
  asm_.Csrrw(0, 0x001, 0);
  asm_.Farith(0, true, 6, 4, 5, 7);   // rm = 111: dynamic
  asm_.Csrr(7, 0x001);
  asm_.Sd(7, 30, 72);
  asm_.FmvXW(7, 6);
  asm_.Sd(7, 30, 80);              // slot 10: the RUP tie result
  asm_.Exit();
  for (size_t i = 0; i < asm_.words().size(); i++) {
    sc.image.Put(g.reset_vector + 4ull * i, asm_.words()[i]);
  }

  RunResult run = Execute(dut, reporter, g, name, sc);
  Check(reporter, run.traps.empty(), name + ": no trap");
  for (uint32_t rm = 0; rm < 4; rm++) {
    const HostResult host = HostArith(0, true, f32_one, f32_tie, rm);
    const uint64_t got_flags = run.Slot(static_cast<int>(rm)) & 0x1F;
    const uint64_t got_value = run.Slot(static_cast<int>(4 + rm)) & 0xFFFFFFFFull;
    Check(reporter, got_value == (host.bits & 0xFFFFFFFFull),
                    name + " rm=" + Dec(rm) + ": the tie result is the host's -- got " +
                        U64(got_value) + ", host " + U64(host.bits & 0xFFFFFFFFull));
    Check(reporter, got_flags == host.flags,
                    name + " rm=" + Dec(rm) + ": flags -- got " +
                        FlagStr(static_cast<uint32_t>(got_flags)) + ", host " +
                        FlagStr(static_cast<uint32_t>(host.flags)));
  }
  const HostResult dyn = HostArith(0, true, f32_one, f32_tie, 3);
  Check(reporter, (run.Slot(10) & 0xFFFFFFFFull) == (dyn.bits & 0xFFFFFFFFull),
                  name + " frm=111 resolves to frm(RUP): got " +
                      U64(run.Slot(10) & 0xFFFFFFFFull) + ", want " +
                      U64(dyn.bits & 0xFFFFFFFFull));
  Check(reporter, run.Slot(8) == 3, name + " frm reads back as written: got " +
                                         U64(run.Slot(8)));
  Check(reporter, (run.Slot(9) & 0x1F) == dyn.flags,
                  name + " a dynamic-mode tie has the host's flags: got " +
                      FlagStr(static_cast<uint32_t>(run.Slot(9) & 0x1F)));
}

}  // namespace

int main(int argc, char** argv) {
  mosaic::Options options;
  std::string error;
  if (!mosaic::Options::Parse(argc, argv, &options, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return mosaic::kExitUsage;
  }
  Verilated::commandArgs(argc, argv);

  mosaic::Reporter reporter(options,
                            std::string("Verilator ") + Verilated::productVersion());
  Vmosaic_core_tb dut;

  std::string detail;
  bool passed = true;
  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();
    const Geometry geometry = ReadGeometry(&dut);
    if (geometry.reset_vector != MOSAIC_RESET_VECTOR) {
      Fail("geometry", "the reset vector is not the profile's: " +
                           U64(geometry.reset_vector));
    }
    if (geometry.xlen != 64) Fail("geometry", "the profile is not 64-bit");

    PhaseArith(&dut, &reporter, geometry, "arith");
    PhaseBoxNaN(&dut, &reporter, geometry, "box-nan");
    PhaseCvt(&dut, &reporter, geometry, "cvt");
    PhaseMem(&dut, &reporter, geometry, "mem");
    PhaseWrongPath(&dut, &reporter, geometry, "wrong-path");
    PhaseFsDirty(&dut, &reporter, geometry, "fs-dirty");
    PhaseFrm(&dut, &reporter, geometry, "frm");

    detail = "checks=" + Dec(static_cast<uint64_t>(reporter.checks())) +
             " phases=7 seed=" + Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "every FP-state claim holds on this machine",
                      "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  // A phase comparison that failed is a failure of the case, whatever the
  // control flow did: the verdict is not the local flag alone.
  passed = passed && (reporter.failures() == 0);
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
