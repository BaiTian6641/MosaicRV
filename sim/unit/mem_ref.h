// ============================================================================
// mem_ref.h -- the two things every core case that runs a corpus program needs,
// and needs to be the *same* thing in both: a data memory for the DUT's data
// port, and an independent RV64IM interpreter to compare against.
//
// It lives in a header rather than a translation unit because the unit runner
// builds the sources the registry names and nothing else; a shared header is
// compiled into each case without a second build-system edit, and the two cases
// that use it (CASE=core.corpus_branch and CASE=core.mem_program) therefore
// cannot drift apart about what "the memory" or "the reference" is.
//
// ---------------------------------------------------------------- independence
//
// The reference interpreter shares nothing with the RTL and nothing with the
// DUT's memory: it decodes the same instruction words from the same image, from
// the ISA text, and it is given its own `mosaic::MemoryModel`. The one thing it
// does share with the DUT is that model's *class* -- which is the harness's
// model of the frozen platform map, not a model of the core, and it is
// instantiated twice so the two never read each other's state.
//
// ------------------------------------------------------ the DUT's data memory
//
// `DataMem` implements the memory system's side of the LSU endpoint's protocol
// (`mosaic_uop_pkg::mem_req_t` / `mem_rsp_t`): a byte-strobed store or a
// doubleword-aligned read, one transaction at a time, with a fixed latency.
//
// The lane convention is the endpoint's own, stated in mosaic_uop_pkg and
// implemented in mosaic_lsu_endpoint: the payload is the doubleword the address
// selects, so the byte at `addr` is lane `addr[2:0]`. A read therefore returns
// the eight bytes at `addr & ~7`, and a store applies its strobes to that same
// window. Doing it any other way would make the memory and the endpoint
// disagree about which byte a byte-store owns, which is precisely the class of
// defect this case exists to find.
//
// A fault is reported as `mem_rsp_t::fault` and nothing is written, matching
// the endpoint's contract that a faulting access has no effect.
// ============================================================================

#ifndef MOSAIC_UNIT_MEM_REF_H_
#define MOSAIC_UNIT_MEM_REF_H_

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "memory_model.h"
#include "mosaic_platform.h"

namespace mosaic_ref {

// A bounded step count for the reference: a program that never reaches its exit
// protocol is a harness error, and running for ever would hide it.
constexpr int kMaxModelSteps = 100000;

// ============================================================================
// DataMem -- the memory the DUT's data port talks to
// ============================================================================
class DataMem {
 public:
  struct Request {
    bool we = false;
    uint64_t addr = 0;
    uint32_t size = 0;
    uint32_t wstrb = 0;
    uint64_t wdata = 0;
  };
  struct Rsp {
    uint64_t rdata = 0;
    bool fault = false;
  };
  // One accepted transaction, kept so a case can say exactly what reached
  // memory, in order, and compare it with the instructions that retired.
  struct Txn {
    Request req;
    uint64_t cycle = 0;
  };

  explicit DataMem(mosaic::MemoryModel* mem, int latency = 1)
      : mem_(mem), latency_(latency < 0 ? 0 : latency) {}

  void Reset() {
    inflight_.clear();
    ready_.clear();
    txns_.clear();
  }

  bool HasResponse() const { return !ready_.empty(); }
  const Rsp& CurrentResponse() const { return ready_.front(); }
  const std::vector<Txn>& txns() const { return txns_; }
  uint64_t accepted() const { return txns_.size(); }

  // The DUT offered a transaction and the memory took it: perform it now, so
  // the model's state is the state at acceptance, exactly as the endpoint's
  // "the transaction is owned from acceptance" rule states.
  void Accept(const Request& req, uint64_t cycle) {
    Txn t;
    t.req = req;
    t.cycle = cycle;
    txns_.push_back(t);
    inflight_.push_back(Entry{Perform(req), latency_});
  }

  void PopResponse() { ready_.pop_front(); }

  void Advance() {
    for (size_t i = 0; i < inflight_.size();) {
      if (--inflight_[i].left <= 0) {
        ready_.push_back(inflight_[i].rsp);
        inflight_.erase(inflight_.begin() + static_cast<long>(i));
      } else {
        ++i;
      }
    }
  }

  // The data word the DUT's memory model holds now, for end-of-run comparison.
  mosaic::MemoryModel* model() { return mem_; }

 private:
  struct Entry {
    Rsp rsp;
    int left = 0;
  };

  uint64_t ReadWindow(uint64_t base, bool* ok) {
    uint64_t value = 0;
    *ok = true;
    for (unsigned i = 0; i < 8; ++i) {
      uint64_t byte = 0;
      const mosaic::AccessStatus st = mem_->Read(base + i, 1, &byte);
      if (st != mosaic::AccessStatus::kOk) {
        *ok = false;
        return 0;
      }
      value |= (byte & 0xffull) << (8 * i);
    }
    return value;
  }

  Rsp Perform(const Request& req) {
    Rsp rsp;
    const uint64_t base = req.addr & ~UINT64_C(7);
    bool ok = true;
    if (req.we) {
      // Read-modify-write over the aligned window so a byte store leaves its
      // neighbours alone and the model's byte-level view is exact.
      uint64_t window = ReadWindow(base, &ok);
      if (!ok) {
        rsp.fault = true;
        return rsp;
      }
      for (unsigned i = 0; i < 8; ++i) {
        if (((req.wstrb >> i) & 1u) != 0u) {
          window = (window & ~(UINT64_C(0xff) << (8 * i))) |
                   ((req.wdata >> (8 * i)) & UINT64_C(0xff)) << (8 * i);
        }
      }
      const mosaic::AccessStatus st = mem_->Write(base, 8, window);
      rsp.fault = (st != mosaic::AccessStatus::kOk);
      rsp.rdata = 0;
      return rsp;
    }
    rsp.rdata = ReadWindow(base, &ok);
    rsp.fault = !ok;
    rsp.rdata = ok ? rsp.rdata : 0;
    return rsp;
  }

  mosaic::MemoryModel* mem_;
  int latency_;
  std::deque<Entry> inflight_;
  std::deque<Rsp> ready_;
  std::vector<Txn> txns_;
};

// ============================================================================
// The reference: an RV64IM interpreter with memory
// ============================================================================
// Decoded from the same words the machine is fed, from the ISA text. Its memory
// is its own `mosaic::MemoryModel`, so a load reads what the reference's own
// stores wrote and nothing the DUT did can reach it.
//
// It stops in two places, and both are statements about the machine:
//
//   * at an instruction the machine's dispatch refuses -- an illegal encoding,
//     a CSR/system instruction, FENCE/FENCE.I. The identity of that instruction
//     is part of the prediction: a machine that executes it, or stops somewhere
//     else, disagrees.
//   * after the program reaches its exit protocol: the store that writes
//     MOSAIC_TOHOST ends the run by the frozen rule, and the reference then
//     executes *one more* instruction before stopping. That one extra
//     instruction is not padding: the machine retires two instructions per
//     cycle, so the exit store and the instruction after it can retire
//     together, and a reference that stopped at the store would call a correct
//     machine wrong. (The instruction after the exit store in every corpus
//     program is the park loop's `j 1b`; the machine never retires a second one
//     before the case stops reading.)
//
// A misaligned access stops the trace too: the frozen p0 policy traps it, and
// the trap path is not part of this machine yet, so the reference says so
// instead of inventing the architectural result.
struct RefInsn {
  uint64_t pc = 0;
  uint32_t rd = 0;
  bool reg_we = false;
  uint64_t value = 0;
  bool is_control = false;   // branch, JAL or JALR
  bool taken = false;        // the control transfer redirects the front end
  bool is_branch = false;
  bool is_jal = false;
  bool is_jalr = false;
  bool back_edge = false;    // target <= pc
  bool is_load = false;
  bool is_store = false;
  uint64_t next_pc = 0;
};

struct RefResult {
  std::vector<RefInsn> trace;
  uint32_t control_total = 0;
  uint32_t taken_total = 0;
  uint32_t branches = 0;
  uint32_t branch_taken = 0;
  uint32_t jal = 0;
  uint32_t jalr = 0;
  uint32_t back_edges = 0;
  uint32_t loads = 0;
  uint32_t stores = 0;
  uint64_t stop_pc = 0;      // where the reference stopped (or would continue)
  uint32_t stop_word = 0;
  bool stopped = false;
  bool exited = false;       // it reached the exit protocol
  uint64_t exit_pc = 0;      // the PC of the store that wrote MOSAIC_TOHOST
  std::string stop_reason;
};

// `Image` needs only `uint32_t Word(uint64_t) const` -- the same accessor the
// corpus harness's image class already provides.
template <typename Image>
RefResult RunReference(const Image& img, uint64_t start, mosaic::MemoryModel* mem) {
  RefResult out;
  uint64_t regs[32] = {};
  uint64_t pc = start;
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

    RefInsn rec;
    rec.pc = pc;
    bool supported = true;
    uint64_t value = 0;
    bool reg_we = false;
    uint64_t next = pc + 4;

    switch (opcode) {
      case 0x37u:  // LUI
        value = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(imm_u)));
        reg_we = true;
        break;
      case 0x17u:  // AUIPC
        value = pc + static_cast<uint64_t>(
                        static_cast<int64_t>(static_cast<int32_t>(imm_u)));
        reg_we = true;
        break;
      case 0x6Fu: {  // JAL
        value = pc + 4;
        reg_we = true;
        next = pc + static_cast<uint64_t>(static_cast<int64_t>(imm_j));
        rec.is_control = true;
        rec.taken = true;
        rec.is_jal = true;
        out.control_total++;
        out.taken_total++;
        out.jal++;
        if (next <= pc) out.back_edges++;
        break;
      }
      case 0x67u: {  // JALR
        if (f3 != 0u) { supported = false; break; }
        value = pc + 4;
        reg_we = true;
        next = (regs[rs1] + static_cast<uint64_t>(
                                 static_cast<int64_t>(imm_i))) & ~UINT64_C(1);
        rec.is_control = true;
        rec.taken = true;
        rec.is_jalr = true;
        out.control_total++;
        out.taken_total++;
        out.jalr++;
        if (next <= pc) out.back_edges++;
        break;
      }
      case 0x63u: {  // the six conditional branches
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
        reg_we = false;
        next = take ? (pc + static_cast<uint64_t>(static_cast<int64_t>(imm_b)))
                    : (pc + 4);
        rec.is_control = true;
        rec.taken = take;
        rec.is_branch = true;
        out.control_total++;
        out.branches++;
        if (take) {
          out.taken_total++;
          out.branch_taken++;
          if (next <= pc) out.back_edges++;
        }
        break;
      }
      case 0x03u: {  // LOAD
        const uint64_t addr = regs[rs1] + static_cast<uint64_t>(
                                             static_cast<int64_t>(imm_i));
        unsigned size = 0;
        bool sign = false;
        switch (f3) {
          case 0x0u: size = 1; sign = true; break;    // lb
          case 0x1u: size = 2; sign = true; break;    // lh
          case 0x2u: size = 4; sign = true; break;    // lw
          case 0x3u: size = 8; sign = true; break;    // ld
          case 0x4u: size = 1; sign = false; break;   // lbu
          case 0x5u: size = 2; sign = false; break;   // lhu
          case 0x6u: size = 4; sign = false; break;   // lwu
          default: supported = false; break;
        }
        if (!supported) break;
        if (addr % size != 0) {
          // The frozen p0 policy traps a misaligned load and this machine has no
          // trap path yet, so the reference stops here and says why.
          out.stop_pc = pc;
          out.stop_word = w;
          out.stop_reason = "misaligned load";
          out.stopped = true;
          return out;
        }
        uint64_t raw = 0;
        if (mem->Read(addr, size, &raw) != mosaic::AccessStatus::kOk) {
          out.stop_pc = pc;
          out.stop_word = w;
          out.stop_reason = "load access fault";
          out.stopped = true;
          return out;
        }
        if (sign && size < 8) {
          const int shift = 64 - static_cast<int>(8 * size);
          value = static_cast<uint64_t>(static_cast<int64_t>(raw << shift) >> shift);
        } else {
          value = raw;
        }
        reg_we = true;
        rec.is_load = true;
        out.loads++;
        break;
      }
      case 0x23u: {  // STORE
        const uint64_t addr = regs[rs1] + static_cast<uint64_t>(
                                             static_cast<int64_t>(imm_s));
        unsigned size = 0;
        switch (f3) {
          case 0x0u: size = 1; break;   // sb
          case 0x1u: size = 2; break;   // sh
          case 0x2u: size = 4; break;   // sw
          case 0x3u: size = 8; break;   // sd
          default: supported = false; break;
        }
        if (!supported) break;
        if (addr % size != 0) {
          out.stop_pc = pc;
          out.stop_word = w;
          out.stop_reason = "misaligned store";
          out.stopped = true;
          return out;
        }
        if (mem->Write(addr, size, regs[rs2]) != mosaic::AccessStatus::kOk) {
          out.stop_pc = pc;
          out.stop_word = w;
          out.stop_reason = "store access fault";
          out.stopped = true;
          return out;
        }
        rec.is_store = true;
        out.stores++;
        if (addr == MOSAIC_TOHOST) {
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
            // RV64 SLLI: funct6 (bits 31:26) is zero and the shift amount is
            // the six-bit field at bits 25:20.
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
        break;
      }
      case 0x33u: {  // OP
        const uint64_t a = regs[rs1];
        const uint64_t b = regs[rs2];
        if (f7 == 0x01u) { supported = false; break; }  // the M extension: not here
        switch (f3) {
          case 0x0u:
            value = (f7 == 0x20u) ? (a - b) : (a + b);
            break;
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
        if (supported && (f7 != 0x00u) && (f7 != 0x20u)) supported = false;
        reg_we = true;
        break;
      }
      case 0x1Bu: {  // OP-IMM-32 (RV64 W forms)
        const uint64_t a = regs[rs1] & 0xFFFFFFFFull;
        int32_t r = 0;
        switch (f3) {
          case 0x0u: r = static_cast<int32_t>(a) +
                         static_cast<int32_t>(imm_i); break;                        // addiw
          case 0x1u: r = static_cast<int32_t>(a << ((w >> 20) & 0x1Fu)); break;      // slliw
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
        break;
      }
      default:
        supported = false;
        break;
    }

    if (!supported) {
      out.stop_pc = pc;
      out.stop_word = w;
      out.stop_reason = "instruction dispatch refuses";
      out.stopped = true;
      break;
    }

    rec.rd = reg_we ? rd : 0u;
    rec.reg_we = reg_we && (rd != 0u);
    rec.value = value;
    rec.next_pc = next;
    if (rec.reg_we) regs[rd] = value;
    out.trace.push_back(rec);
    pc = next;

    if (one_more) {
      // The exit store was the previous instruction; this one completes the
      // prediction (see the header note on the two-wide retirement).
      out.exited = true;
      out.exit_pc = exit_pc;
      out.stop_pc = pc;
      out.stop_word = 0;
      out.stop_reason = "the program reached its exit protocol";
      out.stopped = true;
      break;
    }
    if (exit_written) one_more = true;
    if (out.trace.size() >= static_cast<size_t>(kMaxModelSteps - 2)) break;
  }
  return out;
}

}  // namespace mosaic_ref

#endif  // MOSAIC_UNIT_MEM_REF_H_
