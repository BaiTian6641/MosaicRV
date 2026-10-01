// ============================================================================
// trap_ref.h -- the independent expectation CASE=core.trap_csr_program compares
// against: an RV64IM_Zicsr interpreter that models the p0 platform's traps.
//
// It shares nothing with the RTL and nothing with the DUT's memory: it decodes
// the same instruction words from the same image, from the ISA text, and it is
// given its own `mosaic::MemoryModel` -- the harness's model of the frozen
// platform map, which is a model of the *platform* and not of the core. The one
// thing it deliberately reproduces is the platform's frozen policy, because a
// reference that disagreed with the platform about which accesses fault would be
// measuring the wrong machine:
//
//   * the misalignment policy is config/profiles/p0.json ->
//     misalignment.load/store = "trap", and misalignment is decided from the
//     address alone, before the memory is consulted (the precedence
//     mosaic_lsu_endpoint.sv documents), so a misaligned access to an unmapped
//     address reports 4/6 and not 5/7;
//   * a store into the read-only boot_rom is a store/AMO access fault (cause 7),
//     and a load from an address the map does not cover is a load access fault
//     (cause 5);
//   * the machine-mode CSR set and its write legality come from
//     config/csr/mode_m.json, transcribed below with the register each rule
//     belongs to. `mstatus` is WARL with the generated write mask
//     `wmask_mstatus` = 0x66aa (writable fields 1, 3, 5, 7, 10:9, 14:13) and a
//     read-only MPP of 3; mepc has its low two bits read-only zero (p0 is
//     IALIGN=32); mtvec canonicalises a reserved MODE to Direct; medeleg and
//     mideleg have no delegation target in an M-only profile and canonicalise to
//     zero; `time` and the `cycle`/`instret` shadows are read-only.
//
// The interpreter stops, like the mem_ref one, in two places: after the program
// reaches its exit protocol (the store that writes MOSAIC_TOHOST ends the run,
// and one more instruction is executed because the machine retires two per
// cycle), and at any instruction this interpreter does not implement -- which is
// a statement about the model, not about the machine.
// ============================================================================

#ifndef MOSAIC_UNIT_TRAP_REF_H_
#define MOSAIC_UNIT_TRAP_REF_H_

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "memory_model.h"
#include "mosaic_platform.h"

namespace mosaic_trap {

constexpr int kMaxModelSteps = 400000;

// --- machine-mode exception codes (mosaic_pkg.sv, from the Privileged Spec) ---
constexpr uint64_t kExcInsnAccess     = 1;
constexpr uint64_t kExcIllegalInsn    = 2;
constexpr uint64_t kExcBreakpoint     = 3;
constexpr uint64_t kExcLoadMisaligned = 4;
constexpr uint64_t kExcLoadAccess     = 5;
constexpr uint64_t kExcStoreMisaligned = 6;
constexpr uint64_t kExcStoreAccess    = 7;
constexpr uint64_t kExcEcallM         = 11;

// Interrupt causes carry mcause's bit 63. p0 implements the CLINT's software and
// timer interrupts and no PLIC, so the external cause is modelled only because
// the interrupt unit implements it; an external interrupt asserted by a harness
// has no memory-mapped origin here.
constexpr uint64_t kIrqBit = UINT64_C(1) << 63;
constexpr uint64_t kIrqMsi = kIrqBit | 3;
constexpr uint64_t kIrqMti = kIrqBit | 7;
constexpr uint64_t kIrqMei = kIrqBit | 11;

// --- architecture constants this interpreter needs ---
constexpr uint64_t kMstatusMie  = UINT64_C(0x8);
constexpr uint64_t kMstatusMpie = UINT64_C(0x80);
constexpr uint64_t kMstatusMpp  = UINT64_C(0x1800);
// config/csr/mode_m.json -> mstatus writable_fields ["1","3","5","7","10:9","14:13"]
constexpr uint64_t kMstatusWmask = UINT64_C(0x66aa);
// mip/mie bits: 3 = MSIP/MSIE, 7 = MTIP/MTIE, 11 = MEIP/MEIE
constexpr uint64_t kMipWmask = UINT64_C(0x88);
// mvendorid/marchid/mimpid/mhartid/mcounteren/medeleg/mideleg read as their
// reset values (0) and mcounteren's fields are all read-only.
constexpr uint64_t kMisaReset = UINT64_C(0x8000000000001100);

struct RefEvent {
  uint64_t pc = 0;
  bool trap = false;      // a trap entry, not a retirement
  uint32_t rd = 0;
  bool reg_we = false;
  uint64_t value = 0;
  // trap-only
  uint64_t cause = 0;
  uint64_t tval = 0;
  uint64_t epc = 0;
  bool is_irq = false;
};

struct RefResult {
  std::vector<RefEvent> trace;      // program order: retirements and traps
  std::vector<uint64_t> trap_causes;
  std::vector<uint64_t> trap_epcs;
  uint32_t traps = 0;
  uint32_t irq_traps = 0;
  uint32_t mret = 0;
  uint32_t loads = 0;
  uint32_t stores = 0;
  uint32_t csr_writes = 0;
  uint64_t stop_pc = 0;
  bool exited = false;
  uint64_t exit_pc = 0;
  std::string stop_reason;
};

// The CSR file this interpreter models, with only the registers the corpus and
// the directed programs touch, and the write legality each one's own config row
// declares.
struct CsrFile {
  uint64_t mstatus = 0x1800;  // config: reset 6144
  uint64_t mtvec = 0;
  uint64_t mepc = 0;
  uint64_t mcause = 0;
  uint64_t mtval = 0;
  uint64_t mscratch = 0;
  uint64_t mie = 0;
  uint64_t mip = 0;
  uint64_t mcycle = 0;
  uint64_t minstret = 0;

  static bool Implemented(uint32_t addr) {
    switch (addr) {
      case 0x300: case 0x301: case 0x302: case 0x303: case 0x304: case 0x305:
      case 0x306: case 0x340: case 0x341: case 0x342: case 0x343: case 0x344:
      case 0xB00: case 0xB02: case 0xC00: case 0xC01: case 0xC02:
      case 0xF11: case 0xF12: case 0xF13: case 0xF14:
        return true;
      default:
        return false;
    }
  }
  static bool WriteLegal(uint32_t addr) {
    switch (addr) {
      case 0x300: case 0x301: case 0x302: case 0x303: case 0x304: case 0x305:
      case 0x306: case 0x340: case 0x341: case 0x342: case 0x343: case 0x344:
      case 0xB00: case 0xB02:
        return true;
      default:
        return false;   // the cycle/instret shadows, time, and the ID registers
    }
  }

  uint64_t Read(uint32_t addr) const {
    switch (addr) {
      case 0x300: return mstatus;
      case 0x301: return kMisaReset;
      case 0x302: return 0;                 // medeleg: M-only, only legal value 0
      case 0x303: return 0;                 // mideleg: same
      case 0x304: return mie;
      case 0x305: return mtvec;
      case 0x306: return 0;                 // mcounteren: all fields read-only 0
      case 0x340: return mscratch;
      case 0x341: return mepc;
      case 0x342: return mcause;
      case 0x343: return mtval;
      case 0x344: return mip & kMipWmask;
      case 0xB00: return mcycle;
      case 0xB02: return minstret;
      case 0xC00: return mcycle;            // cycle: read-only shadow
      case 0xC01: return 0;                 // time: read-only shadow of mtime
      case 0xC02: return minstret;          // instret: read-only shadow
      default:    return 0;                 // the ID registers read their reset
    }
  }

  void Write(uint32_t addr, uint64_t value) {
    switch (addr) {
      case 0x300:
        mstatus = (mstatus & ~kMstatusWmask) | (value & kMstatusWmask);
        mstatus |= kMstatusMpp;              // MPP is read-only 3 in this profile
        break;
      case 0x301: break;                     // misa: accepted, reads its reset
      case 0x302: case 0x303: break;         // delegation canonicalises to 0
      case 0x304: mie = (mie & ~kMipWmask) | (value & kMipWmask); break;
      case 0x305: mtvec = (value & ~UINT64_C(3)) |
                          ((value & 3) <= 1 ? (value & 3) : 0); break;
      case 0x306: break;                     // mcounteren: no writable field
      case 0x340: mscratch = value; break;
      case 0x341: mepc = value & ~UINT64_C(3); break;
      case 0x342: mcause = value; break;
      case 0x343: mtval = value; break;
      case 0x344: break;                     // mip is the interrupt unit's
      case 0xB00: mcycle = value; break;
      case 0xB02: minstret = value; break;
      default: break;
    }
  }

  // Trap entry, exactly as mosaic_csr's trap path implements it (its header is
  // the contract): MPIE <- MIE, MIE <- 0, MPP <- M (3), mepc <- the interrupted
  // PC with its read-only low bits forced zero, mcause and mtval from the trap.
  void TrapEnter(uint64_t pc, uint64_t cause, uint64_t tval) {
    mstatus = (mstatus & ~(kMstatusMie | kMstatusMpie)) |
              ((mstatus & kMstatusMie) ? kMstatusMpie : 0) | kMstatusMpp;
    mepc = pc & ~UINT64_C(3);
    mcause = cause;
    mtval = tval;
  }

  // MRET: MIE <- MPIE, MPIE <- 1, MPP <- M.
  void Mret() {
    mstatus = (mstatus & ~(kMstatusMie | kMstatusMpie)) |
              ((mstatus & kMstatusMpie) ? kMstatusMie : 0) |
              kMstatusMpie | kMstatusMpp;
  }

  // The vector a trap enters at: Direct mode enters at BASE; Vectored mode
  // enters at BASE + 4 * cause code for an interrupt and at BASE for everything
  // else (Table mtvec MODE).
  uint64_t Target(uint64_t cause) const {
    if ((mtvec & 3) == 1 && (cause >> 63) != 0) {
      return (mtvec & ~UINT64_C(3)) + (((cause & ~kIrqBit) & UINT64_C(0x3ffffff))
                                       << 2);
    }
    return mtvec & ~UINT64_C(3);
  }
};

// `Image` needs only `uint32_t Word(uint64_t) const`.
template <typename Image>
RefResult RunReference(const Image& img, uint64_t start, mosaic::MemoryModel* mem) {
  RefResult out;
  uint64_t regs[32] = {};
  uint64_t pc = start;
  CsrFile csr;
  bool exit_written = false;
  bool one_more = false;
  uint64_t exit_pc = 0;

  for (int step = 0; step < kMaxModelSteps; step++) {
    const uint32_t w = img.Word(pc);
    const uint32_t opcode = w & 0x7Fu;
    const uint32_t rd = (w >> 7) & 0x1Fu;
    const uint32_t f3 = (w >> 12) & 0x7u;
    const uint32_t rs1 = (w >> 15) & 0x1Fu;
    const uint32_t rs2 = (w >> 20) & 0x1Fu;
    const uint32_t f7 = (w >> 25) & 0x7Fu;
    const int32_t imm_i = static_cast<int32_t>(w) >> 20;
    const int32_t imm_s =
        (static_cast<int32_t>(w) >> 25 << 5) | static_cast<int32_t>((w >> 7) & 0x1Fu);
    const int32_t imm_b =
        ((static_cast<int32_t>(w) >> 31) << 12) |
        (((w >> 7) & 1u) << 11) | (((w >> 25) & 0x3Fu) << 5) |
        (((w >> 8) & 0xFu) << 1);
    const uint32_t imm_u = w & 0xFFFFF000u;
    const int32_t imm_j =
        ((static_cast<int32_t>(w) >> 31) << 20) | (((w >> 12) & 0xFFu) << 12) |
        (((w >> 20) & 1u) << 11) | (((w >> 21) & 0x3FFu) << 1);

    RefEvent rec;
    rec.pc = pc;
    bool supported = true;
    bool reg_we = false;
    // Whether the *decoder* defines an rd field for this instruction. The
    // machine's retire event carries the decoder's field, and the decoder leaves
    // it at zero for the arms that have no destination (stores, branches,
    // fence, ecall/ebreak/mret) -- the bits at 11:7 are a fragment of the
    // immediate there and are not an rd. The reference models that explicitly
    // rather than reporting the raw field.
    bool rd_defined = false;
    uint64_t value = 0;
    uint64_t next = pc + 4;

    // A pending trap raised *by this instruction*: taken instead of retiring it.
    bool have_trap = false;
    uint64_t trap_cause = 0, trap_tval = 0;

    switch (opcode) {
      case 0x37u:  // LUI
        value = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(imm_u)));
        reg_we = true;
        rd_defined = true;
        break;
      case 0x17u:  // AUIPC
        value = pc + static_cast<uint64_t>(
                        static_cast<int64_t>(static_cast<int32_t>(imm_u)));
        reg_we = true;
        rd_defined = true;
        break;
      case 0x6Fu: {  // JAL
        value = pc + 4;
        reg_we = true;
        rd_defined = true;
        next = pc + static_cast<uint64_t>(static_cast<int64_t>(imm_j));
        break;
      }
      case 0x67u: {  // JALR
        if (f3 != 0u) { supported = false; break; }
        value = pc + 4;
        reg_we = true;
        rd_defined = true;
        next = (regs[rs1] + static_cast<uint64_t>(static_cast<int64_t>(imm_i))) &
               ~UINT64_C(1);
        break;
      }
      case 0x63u: {  // conditional branches
        const int64_t a = static_cast<int64_t>(regs[rs1]);
        const int64_t b = static_cast<int64_t>(regs[rs2]);
        const uint64_t ua = regs[rs1];
        const uint64_t ub = regs[rs2];
        bool take = false;
        switch (f3) {
          case 0x0u: take = (ua == ub); break;
          case 0x1u: take = (ua != ub); break;
          case 0x4u: take = (a < b); break;
          case 0x5u: take = (a >= b); break;
          case 0x6u: take = (ua < ub); break;
          case 0x7u: take = (ua >= ub); break;
          default: supported = false; break;
        }
        if (!supported) break;
        next = take ? (pc + static_cast<uint64_t>(static_cast<int64_t>(imm_b)))
                    : (pc + 4);
        break;
      }
      case 0x03u: {  // LOAD
        const uint64_t addr = regs[rs1] + static_cast<uint64_t>(
                                             static_cast<int64_t>(imm_i));
        unsigned size = 0;
        bool sign = false;
        switch (f3) {
          case 0x0u: size = 1; sign = true; break;
          case 0x1u: size = 2; sign = true; break;
          case 0x2u: size = 4; sign = true; break;
          case 0x3u: size = 8; sign = true; break;
          case 0x4u: size = 1; sign = false; break;
          case 0x5u: size = 2; sign = false; break;
          case 0x6u: size = 4; sign = false; break;
          default: supported = false; break;
        }
        if (!supported) break;
        // Misalignment is decided from the address alone, so it outranks the
        // access fault (mosaic_lsu_endpoint.sv's documented precedence).
        if (addr % size != 0) {
          have_trap = true;
          trap_cause = kExcLoadMisaligned;
          trap_tval = addr;
          break;
        }
        uint64_t raw = 0;
        if (mem->Read(addr, size, &raw) != mosaic::AccessStatus::kOk) {
          have_trap = true;
          trap_cause = kExcLoadAccess;
          trap_tval = addr;
          break;
        }
        if (sign && size < 8) {
          const int shift = 64 - static_cast<int>(8 * size);
          value = static_cast<uint64_t>(static_cast<int64_t>(raw << shift) >> shift);
        } else {
          value = raw;
        }
        reg_we = true;
        rd_defined = true;
        out.loads++;
        break;
      }
      case 0x23u: {  // STORE
        const uint64_t addr = regs[rs1] + static_cast<uint64_t>(
                                             static_cast<int64_t>(imm_s));
        unsigned size = 0;
        switch (f3) {
          case 0x0u: size = 1; break;
          case 0x1u: size = 2; break;
          case 0x2u: size = 4; break;
          case 0x3u: size = 8; break;
          default: supported = false; break;
        }
        if (!supported) break;
        if (addr % size != 0) {
          have_trap = true;
          trap_cause = kExcStoreMisaligned;
          trap_tval = addr;
          break;
        }
        if (mem->Write(addr, size, regs[rs2]) != mosaic::AccessStatus::kOk) {
          have_trap = true;
          trap_cause = kExcStoreAccess;
          trap_tval = addr;
          break;
        }
        out.stores++;
        if (addr == MOSAIC_TOHOST && regs[rs2] != 0) {
          exit_written = true;
          exit_pc = pc;
        }
        break;
      }
      case 0x13u: {  // OP-IMM
        const uint64_t a = regs[rs1];
        switch (f3) {
          case 0x0u: value = a + static_cast<uint64_t>(static_cast<int64_t>(imm_i)); break;
          case 0x2u: value = static_cast<uint64_t>(static_cast<int64_t>(a) <
                                                   static_cast<int64_t>(imm_i)); break;
          case 0x3u: value = static_cast<uint64_t>(a < static_cast<uint64_t>(
                                                          static_cast<int64_t>(imm_i))); break;
          case 0x4u: value = a ^ static_cast<uint64_t>(static_cast<int64_t>(imm_i)); break;
          case 0x6u: value = a | static_cast<uint64_t>(static_cast<int64_t>(imm_i)); break;
          case 0x7u: value = a & static_cast<uint64_t>(static_cast<int64_t>(imm_i)); break;
          case 0x1u: {
            if (((w >> 26) & 0x3Fu) != 0u) { supported = false; break; }
            value = a << ((w >> 20) & 0x3Fu);
            break;
          }
          case 0x5u: {
            const uint32_t funct6 = (w >> 26) & 0x3Fu;
            const uint32_t shamt = (w >> 20) & 0x3Fu;
            if (funct6 == 0x00u) {
              value = a >> shamt;
            } else if (funct6 == 0x10u) {
              value = static_cast<uint64_t>(static_cast<int64_t>(a) >> shamt);
            } else {
              supported = false;
            }
            break;
          }
          default: supported = false; break;
        }
        reg_we = true;
        rd_defined = true;
        break;
      }
      case 0x33u: {  // OP (including the M extension)
        const uint64_t a = regs[rs1];
        const uint64_t b = regs[rs2];
        if (f7 == 0x01u) {
          const int64_t sa = static_cast<int64_t>(a);
          const int64_t sb = static_cast<int64_t>(b);
          switch (f3) {
            case 0x0u: value = a * b; break;                                   // mul
            case 0x1u: value = static_cast<uint64_t>(                              // mulh
                (static_cast<__int128>(sa) * static_cast<__int128>(sb)) >> 64); break;
            case 0x2u: value = static_cast<uint64_t>(                              // mulhsu
                (static_cast<__int128>(sa) * static_cast<__int128>(b)) >> 64); break;
            case 0x3u: value = static_cast<uint64_t>(                              // mulhu
                (static_cast<unsigned __int128>(a) *
                 static_cast<unsigned __int128>(b)) >> 64); break;
            case 0x4u:                                                             // div
              if (b == 0) value = ~UINT64_C(0);
              else if (a == (UINT64_C(1) << 63) && b == ~UINT64_C(0)) value = a;
              else value = static_cast<uint64_t>(sa / sb);
              break;
            case 0x5u: value = (b == 0) ? ~UINT64_C(0) : (a / b); break;           // divu
            case 0x6u:                                                             // rem
              if (b == 0) value = a;
              else if (a == (UINT64_C(1) << 63) && b == ~UINT64_C(0)) value = 0;
              else value = static_cast<uint64_t>(sa % sb);
              break;
            default: value = (b == 0) ? a : (a % b); break;                        // remu
          }
          reg_we = true;
          rd_defined = true;
          break;
        }
        if (f7 != 0x00u && f7 != 0x20u) { supported = false; break; }
        switch (f3) {
          case 0x0u: value = (f7 == 0x20u) ? (a - b) : (a + b); break;
          case 0x1u: value = a << (b & 0x3Fu); break;
          case 0x2u: value = static_cast<uint64_t>(static_cast<int64_t>(a) <
                                                   static_cast<int64_t>(b)); break;
          case 0x3u: value = static_cast<uint64_t>(a < b); break;
          case 0x4u: value = a ^ b; break;
          case 0x5u:
            value = (f7 == 0x20u) ? static_cast<uint64_t>(static_cast<int64_t>(a) >>
                                                          (b & 0x3Fu))
                                  : (a >> (b & 0x3Fu));
            break;
          case 0x6u: value = a | b; break;
          case 0x7u: value = a & b; break;
          default: supported = false; break;
        }
        reg_we = true;
        rd_defined = true;
        break;
      }
      case 0x1Bu: {  // OP-IMM-32
        const uint64_t a = regs[rs1] & 0xFFFFFFFFull;
        int32_t r = 0;
        switch (f3) {
          case 0x0u: r = static_cast<int32_t>(a) + static_cast<int32_t>(imm_i); break;
          case 0x1u: r = static_cast<int32_t>(a << ((w >> 20) & 0x1Fu)); break;
          case 0x5u: {
            const uint32_t funct7 = (w >> 25) & 0x7Fu;
            const uint32_t shamt = (w >> 20) & 0x1Fu;
            if (funct7 == 0x00u) {
              r = static_cast<int32_t>(static_cast<uint32_t>(a) >> shamt);
            } else if (funct7 == 0x20u) {
              r = static_cast<int32_t>(static_cast<int32_t>(a) >> shamt);
            } else {
              supported = false;
            }
            break;
          }
          default: supported = false; break;
        }
        value = static_cast<uint64_t>(static_cast<int64_t>(r));
        reg_we = true;
        rd_defined = true;
        break;
      }
      case 0x3Bu: {  // OP-32
        const uint64_t a = regs[rs1] & 0xFFFFFFFFull;
        const uint64_t b = regs[rs2] & 0xFFFFFFFFull;
        int32_t r = 0;
        if (f7 == 0x01u) {
          const int32_t sa = static_cast<int32_t>(a);
          const int32_t sb = static_cast<int32_t>(b);
          switch (f3) {
            case 0x0u: r = static_cast<int32_t>(sa * sb); break;                 // mulw
            case 0x4u:                                                           // divw
              if (sb == 0) r = -1;
              else if (sa == INT32_MIN && sb == -1) r = sa;
              else r = sa / sb;
              break;
            case 0x5u:                                                           // divuw
              r = (sb == 0) ? -1 : static_cast<int32_t>(static_cast<uint32_t>(sa) /
                                                        static_cast<uint32_t>(sb));
              break;
            case 0x6u:                                                           // remw
              if (sb == 0) r = sa;
              else if (sa == INT32_MIN && sb == -1) r = 0;
              else r = sa % sb;
              break;
            case 0x7u:                                                           // remuw
              r = (sb == 0) ? sa
                            : static_cast<int32_t>(static_cast<uint32_t>(sa) %
                                                   static_cast<uint32_t>(sb));
              break;
            default: supported = false; break;
          }
        } else if (f7 == 0x00u) {
          switch (f3) {
            case 0x0u: r = static_cast<int32_t>(a) + static_cast<int32_t>(b); break;
            case 0x1u: r = static_cast<int32_t>(a << (b & 0x1Fu)); break;
            case 0x5u: r = static_cast<int32_t>(static_cast<uint32_t>(a) >> (b & 0x1Fu)); break;
            default: supported = false; break;
          }
        } else if (f7 == 0x20u && f3 == 0x0u) {
          r = static_cast<int32_t>(a) - static_cast<int32_t>(b);
        } else if (f7 == 0x20u && f3 == 0x5u) {
          r = static_cast<int32_t>(static_cast<int32_t>(a) >> (b & 0x1Fu));
        } else {
          supported = false;
        }
        value = static_cast<uint64_t>(static_cast<int64_t>(r));
        reg_we = true;
        rd_defined = true;
        break;
      }
      case 0x0Fu:  // MISC-MEM: fence / fence.i are no-ops in this machine
        supported = (f3 == 0x0u || f3 == 0x1u);
        break;
      case 0x73u: {  // SYSTEM
        if (f3 == 0x0u) {
          const uint32_t imm12 = (w >> 20) & 0xFFFu;
          if (rd == 0 && rs1 == 0 && imm12 == 0x000u) {           // ECALL
            have_trap = true;
            trap_cause = kExcEcallM;
            trap_tval = 0;
          } else if (rd == 0 && rs1 == 0 && imm12 == 0x001u) {    // EBREAK
            have_trap = true;
            trap_cause = kExcBreakpoint;
            trap_tval = 0;
          } else if (rd == 0 && rs1 == 0 && imm12 == 0x302u) {    // MRET
            // MRET is a control transfer, not a retirement that writes a
            // register: it takes its effect and continues at mepc.
            csr.Mret();
            next = csr.mepc;
            out.mret++;
            // A RefEvent for MRET is a retirement with no destination.
            break;
          } else if (rd == 0 && rs1 == 0 && imm12 == 0x105u) {    // WFI
            // WFI is where this interpreter stops: whether it halts or completes
            // depends on the interrupt state, which is the harness's to drive,
            // and the programs this reference is compared against do not
            // execute it.
            out.stop_pc = pc;
            out.stop_reason = "WFI";
            return out;
          } else {
            supported = false;
          }
          break;
        }
        // The Zicsr forms. funct3 001/010/011 are the register forms and
        // 101/110/111 the immediate ones, which differ only in where the write
        // operand comes from; the architectural intent rules are the decoder's
        // (mosaic_decoder.sv): csrrw always writes and reads only when rd != x0,
        // csrrs/csrrc write only when the operand field is non-zero.
        const bool imm_form = (f3 & 0x4u) != 0;
        const uint32_t csr_addr = (w >> 20) & 0xFFFu;
        const uint64_t opnd = imm_form ? static_cast<uint64_t>(rs1) : regs[rs1];
        const uint32_t op = f3 & 0x3u;   // 1 = rw, 2 = rs, 3 = rc
        const bool writes = (op == 1u) || (rs1 != 0u);
        const bool reads = (op != 1u) || (rd != 0u);
        if (!CsrFile::Implemented(csr_addr)) {
          have_trap = true;
          trap_cause = kExcIllegalInsn;
          trap_tval = 0;
          break;
        }
        if (writes && !CsrFile::WriteLegal(csr_addr)) {
          have_trap = true;
          trap_cause = kExcIllegalInsn;
          trap_tval = 0;
          break;
        }
        const uint64_t old = csr.Read(csr_addr);
        uint64_t newv = old;
        if (op == 1u) newv = opnd;
        else if (op == 2u) newv = old | opnd;
        else if (op == 3u) newv = old & ~opnd;
        if (writes) {
          csr.Write(csr_addr, newv);
          out.csr_writes++;
        }
        if (reads) {
          value = old;
          reg_we = true;
        }
        rd_defined = true;   // every Zicsr form has an rd field
        break;
      }
      default:
        supported = false;
        break;
    }

    if (!supported) {
      out.stop_pc = pc;
      out.stop_reason = "instruction the model does not implement";
      return out;
    }

    if (have_trap) {
      RefEvent t;
      t.pc = pc;
      t.trap = true;
      t.cause = trap_cause;
      t.tval = trap_tval;
      t.epc = pc;
      out.trace.push_back(t);
      if (std::getenv("TRAP_DEBUG") != nullptr && out.traps >= 18 && out.traps < 30) {
        std::printf("    [ref-trap] pc=%llx cause=%llu tval=%llx\n",
                    static_cast<unsigned long long>(pc),
                    static_cast<unsigned long long>(trap_cause),
                    static_cast<unsigned long long>(trap_tval));
      }
      out.trap_causes.push_back(trap_cause);
      out.trap_epcs.push_back(pc);
      out.traps++;
      csr.TrapEnter(pc, trap_cause, trap_tval);
      csr.mcycle++;
      pc = csr.Target(trap_cause);
      continue;
    }

    rec.reg_we = reg_we && rd != 0;
    if (reg_we && rd != 0) {
      regs[rd] = value;
      rec.value = value;
    }
    // A write to x0 retires and reports a destination of x0 with no write, which
    // is what the machine's event stream carries.
    rec.rd = rd_defined ? rd : 0u;
    out.trace.push_back(rec);
    csr.mcycle++;
    csr.minstret++;
    pc = next;

    if (exit_written) {
      if (one_more) break;
      one_more = true;
    }
  }

  out.exited = exit_written;
  out.exit_pc = exit_pc;
  if (!exit_written) {
    out.stop_pc = pc;
    if (out.stop_reason.empty()) out.stop_reason = "the model ran out of steps";
  }
  return out;
}

}  // namespace mosaic_trap

#endif  // MOSAIC_UNIT_TRAP_REF_H_
