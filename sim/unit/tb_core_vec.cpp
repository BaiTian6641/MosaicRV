// ============================================================================
// tb_core_vec.cpp -- CASE=vec.integrated and CASE=rvv.lane_resize_boundary,
// work package I-059.
//
// The DUT is the integrated p0 out-of-order core with the vector engine wired
// in: the OP-V decode in the front end, the dispatch system-insert route, the
// ROB-head resolution, the I-051..I-058 units (descriptor, configuration, VRF,
// integer ALU, memory packetizer, restart controller, chaining network), the
// vector CSRs on the core's CSR path, mstatus.VS dirtying, and the runtime lane
// broker (`mosaic_lane_broker`).
//
// What this case claims, and what it uses as the oracle for each claim:
//
//   * THE ARCHITECTURAL RESULT is computed by the HOST element by element from
//     the same input arrays the program loads, and compared against the bytes
//     the vector store wrote to memory. The host never runs the DUT's arithmetic;
//     it is an independent model of `vadd.vv` over 32-bit lanes.
//   * THE VECTOR PATH WAS ACTUALLY TAKEN is required positively, not assumed:
//     the core exports the vector engine's own counters (macros allocated,
//     element completions the chaining network accepted, macros retired) and the
//     case requires each to have moved. A "vector" configuration that executed
//     no vector instruction is the silent failure this guards against.
//   * THE PRECISE FAULT AND RESTART. A unit-stride load whose third element
//     falls outside RAM raises a load access fault at element 2. The handler
//     records mcause/mepc/mtval and `vstart`, repoints the base register at a
//     mapped buffer, and returns to the *same* PC. The re-execution begins at
//     `vstart`, so the first two elements keep the values the first attempt
//     loaded and the last two come from the second attempt -- the committed
//     prefix and the restart point, both observed in the final vector register.
//   * mstatus.VS follows the privileged spec's state machine: Off at reset, so
//     the first vector instruction traps with an illegal instruction; Initial
//     once software writes it; Dirty after a vector instruction that modifies
//     vector state executes.
//   * THE ON/OFF COMPARISON. The same computation is run scalar-only, and the
//     two results are required to be identical. The cycle counts are reported
//     honestly, including if the vector path is slower.
//
// Everything is a program: this driver hand-encodes RV64I + V (the corpus audit
// forbids V in tests/programs) and drives the core's instruction and data ports.
//
// Phases, each a fresh reset and a fresh program:
//
//   1. vs-off    VS=Off: a vsetvli traps illegal; software then enables VS and
//                the same instruction commits, leaving VS Dirty
//   2. arith     load, load, vadd.vv, store -- result against the host model
//   3. restart   a load that faults at element 2, a handler that repoints the
//                base and returns, and the restarted load completing
//   4. scalar    the scalar-equivalent computation for the comparison
//   5. lane-8    one workload of eight elements at quota 8 (e16, m1)
//   6. lane-two-4 two independent four-element descriptor chains -- two
//                register-file regions standing in for two workloads,
//                interleaved by the program -- at quota 4
//   7. lane-resize a resize attempted *inside* a macro: it must land only at
//                the vector instruction boundary, both sides must be correct,
//                the old quota's state must be acknowledged, and vl/vlenb must
//                not move. The lane counts in every phase state that the quota
//                changes how many lanes work on the elements, never which
//                elements exist.
//
// Controls: tools/run_vec_integrated_controls.py injects exactly one defect per
// build for the first four phases, and tools/run_lane_resize_controls.py does
// the same for the lane quota. Each requires the mutant binary to differ from
// the shipping one and to exit 1 with the named check. See
// results/reports/I-059-vector-integration.md and
// results/reports/I-059-lane-broker.md.
//
// `--seed` is accepted and unused: every vector here is directed.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "Vmosaic_core_tb.h"
#include "mask_prefix_ref.h"
#include "mem_ref.h"
#include "memory_model.h"
#include "mosaic_platform.h"
#include "sim_common.h"

using mosaic_ref::DataMem;

namespace {

constexpr int      kResetCycles   = 4;
constexpr uint64_t kProgramCycles = 400000;
constexpr uint64_t kStallCycles   = 40000;

// The data block, the observation slots and the arrays the vector program
// reads and writes. All inside RAM (0x80000000..0x80200000).
constexpr uint64_t kDataBase  = 0x80020000ull;
constexpr uint64_t kSlotBase  = 0x80021000ull;
constexpr int      kDataSize  = 2048;

// The tail of RAM: elements 0 and 1 of a 32-bit unit-stride load at this base
// are inside RAM, element 2 is not.
constexpr uint64_t kRamEdge   = 0x801FFFF8ull;

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string Dec(uint64_t v) { return std::to_string(v); }

// ------------------------------------------------------------------- asm
class ProgImage {
 public:
  void Put(uint64_t addr, uint32_t word) { words_[addr] = word; }
  uint32_t Word(uint64_t addr) const {
    auto it = words_.find(addr);
    return (it == words_.end()) ? 0x0000006fu : it->second;   // jal x0, 0
  }

 private:
  std::map<uint64_t, uint32_t> words_;
};

class Asm {
 public:
  explicit Asm(uint64_t base) : base_(base) {}

  uint64_t pc() const { return base_ + 4ull * words_.size(); }
  const std::vector<uint32_t>& words() const { return words_; }
  size_t Here() const { return words_.size(); }
  uint64_t AddrOf(size_t index) const { return base_ + 4ull * index; }

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

  static void CheckDisp(const char* what, int32_t imm) {
    if ((imm < -2048) || (imm > 2047)) {
      Fail("vec-case assembler", std::string(what) + " displacement " +
           std::to_string(imm) + " does not fit the signed 12-bit immediate");
    }
  }

  void Addi(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(I(imm, rs1, 0, rd, 0x13)); }
  void Ori(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(I(imm, rs1, 6, rd, 0x13)); }
  void Add(uint32_t rd, uint32_t rs1, uint32_t rs2) { Emit(R(0x00, rs2, rs1, 0, rd, 0x33)); }
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
  void Bne(uint32_t rs1, uint32_t rs2, int32_t offset) {
    Emit(B(offset, rs2, rs1, 1, 0x63));
  }
  void Csrrw(uint32_t rd, uint32_t csr, uint32_t rs1) { Emit(I(csr, rs1, 1, rd, 0x73)); }
  void Csrrs(uint32_t rd, uint32_t csr, uint32_t rs1) { Emit(I(csr, rs1, 2, rd, 0x73)); }
  void Csrr(uint32_t rd, uint32_t csr) { Csrrs(rd, csr, 0); }
  void Mret() { Emit(0x30200073u); }
  void JalSelf() { Emit(0x0000006fu); }
  void JalX0(uint32_t rd, int32_t offset) {
    // J-type immediate, encoding bits [20:1] of the offset.
    const uint32_t u = static_cast<uint32_t>(offset);
    Emit((((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3FFu) << 21) |
         (((u >> 11) & 1u) << 20) | (((u >> 12) & 0xFFu) << 12) | (rd << 7) | 0x6f);
  }

  // Load an absolute 64-bit address PC-relative.
  void LaAbs(uint32_t rd, uint64_t target) {
    const int64_t delta = static_cast<int64_t>(target) - static_cast<int64_t>(pc());
    const uint32_t lo = static_cast<uint32_t>(delta) & 0xFFFu;
    const uint32_t hi = static_cast<uint32_t>((delta + 0x800) >> 12) & 0xFFFFFu;
    Emit(U(hi, rd, 0x17));
    Emit(I(static_cast<int32_t>(lo) - ((lo & 0x800u) ? 0x1000 : 0), rd, 0, rd, 0x13));
  }

  // ------------------------------------------------------------- V encodings
  // vsetvli rd, rs1, vtypei. vtypei is the 11-bit argument at insn[30:20];
  // bit31 is 0 for the vsetvli form.
  void Vsetvli(uint32_t rd, uint32_t rs1, uint32_t vtypei) {
    Emit(((vtypei & 0x7FFu) << 20) | (rs1 << 15) | (7u << 12) | (rd << 7) | 0x57);
  }
  // vle32.v vd, (rs1)  -- unit-stride, width 32, unmasked (vm=1).
  void Vle32(uint32_t vd, uint32_t rs1) {
    Emit((1u << 25) | (rs1 << 15) | (6u << 12) | (vd << 7) | 0x07);
  }
  // vse32.v vs3, (rs1).
  void Vse32(uint32_t vs3, uint32_t rs1) {
    Emit((1u << 25) | (rs1 << 15) | (6u << 12) | (vs3 << 7) | 0x27);
  }
  // vle16.v / vse16.v -- funct3 101 selects EEW = 16, which is what lets an
  // e16,m1 configuration hold VLMAX = 128/16 = 8 elements, the eight-lane
  // workload the lane-quota case needs.
  void Vle16(uint32_t vd, uint32_t rs1) {
    Emit((1u << 25) | (rs1 << 15) | (5u << 12) | (vd << 7) | 0x07);
  }
  void Vse16(uint32_t vs3, uint32_t rs1) {
    Emit((1u << 25) | (rs1 << 15) | (5u << 12) | (vs3 << 7) | 0x27);
  }
  // vadd.vv vd, vs2, vs1  -- OPIVV, funct6 000000, vm=1.
  void VaddVv(uint32_t vd, uint32_t vs2, uint32_t vs1) {
    Emit((1u << 25) | (vs2 << 20) | (vs1 << 15) | (0u << 12) | (vd << 7) | 0x57);
  }
  // vle8.v / vse8.v -- EEW = 8, the width a mask register is transferred at.
  void Vle8(uint32_t vd, uint32_t rs1) {
    Emit((1u << 25) | (rs1 << 15) | (0u << 12) | (vd << 7) | 0x07);
  }
  void Vse8(uint32_t vs3, uint32_t rs1) {
    Emit((1u << 25) | (rs1 << 15) | (0u << 12) | (vs3 << 7) | 0x27);
  }
  // The three mask-prefix ops: OPMVV, funct6 010100, funct3 010. `rs1_op`
  // names the operation in the rs1 field (1 = vmsbf, 2 = vmsof, 3 = vmsif);
  // the field is not a GPR operand. `masked` selects the v0.t form (vm = 0).
  void VmaskPrefix(uint32_t vd, uint32_t vs2, uint32_t rs1_op, bool masked) {
    Emit((0x14u << 26) | ((masked ? 0u : 1u) << 25) | (vs2 << 20) |
         (rs1_op << 15) | (2u << 12) | (vd << 7) | 0x57);
  }
  // Mask-register logical: OPMVV, funct6 011xxx, always unmasked (vm = 1).
  void VmaskLog(uint32_t vd, uint32_t vs2, uint32_t vs1, uint32_t funct6) {
    Emit((funct6 << 26) | (1u << 25) | (vs2 << 20) | (vs1 << 15) |
         (2u << 12) | (vd << 7) | 0x57);
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
};

Geometry ReadGeometry(Vmosaic_core_tb* dut) {
  Geometry g;
  g.xlen = dut->o_geom_xlen_o;
  g.retire_width = dut->o_geom_retire_width_o;
  g.rob_entries = dut->o_geom_rob_entries_o;
  g.reset_vector = dut->o_geom_reset_vector_o;
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
  uint64_t vec_macro = 0;
  uint64_t vec_elem = 0;
  uint64_t vec_trap = 0;
  uint64_t vec_retire = 0;
  uint64_t vec_fault = 0;
  uint64_t vec_vtype = 0;
  uint64_t vec_vl = 0;
  uint64_t vec_vstart = 0;
  uint64_t vec_vcsr = 0;
  uint64_t vec_vlenb = 0;
  uint64_t vec_vlmax = 0;
  uint64_t vec_vill = 0;
  uint64_t vec_dbg0 = 0;
  uint64_t vec_dbg1 = 0;
  uint64_t vec_dbg2 = 0;
  uint64_t vec_lsu_req = 0;
  uint64_t vec_alu_elems = 0;
  uint64_t vec_chain_accept = 0;
  uint64_t vec_chain_refuse = 0;
  uint64_t vec_desc_alloc = 0;
  uint64_t vec_desc_release = 0;
  uint64_t vec_vrf_rd = 0;
  uint64_t vec_vrf_wr = 0;
  uint64_t vec_vrf_bad = 0;
  uint64_t vec_vrf_rows = 0;
  uint64_t vec_vrf_banks = 0;
  // The lane broker's evidence (I-059): the committed share, its generation,
  // the resize/acknowledgement counts, the per-lane element counts (lane 0 in
  // the least significant byte), and the two direct statements of the boundary
  // and acknowledgement rules.
  uint64_t lane_quota = 0;
  uint64_t lane_gen = 0;
  uint64_t lane_publish_ctr = 0;
  uint64_t lane_ack_req_ctr = 0;
  uint64_t lane_ack_ctr = 0;
  uint64_t lane_req_mid_macro_ctr = 0;
  uint64_t lane_pub_mid_macro_ctr = 0;
  uint64_t lane_abort_ctr = 0;
  uint32_t lane_elem[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  uint64_t mstatus = 0;
  uint64_t mtvec = 0;
  uint64_t mepc = 0;
  uint64_t mcause = 0;
  uint64_t mtval = 0;
  std::vector<uint64_t> traps;
  std::vector<uint64_t> trap_tvals;
  mosaic::MemoryModel mem;

  uint64_t Slot(int index) {
    uint64_t value = 0;
    mem.Read(kSlotBase + 8ull * static_cast<uint64_t>(index), 8, &value);
    return value;
  }
  uint64_t Data64(uint64_t off) {
    uint64_t value = 0;
    mem.Read(kDataBase + off, 8, &value);
    return value;
  }
  uint32_t Data32(uint64_t off) {
    uint32_t value = 0;
    uint64_t v = 0;
    mem.Read(kDataBase + off, 4, &v);
    value = static_cast<uint32_t>(v);
    return value;
  }
  uint32_t Data16(uint64_t off) {
    uint64_t v = 0;
    mem.Read(kDataBase + off, 2, &v);
    return static_cast<uint32_t>(v & 0xFFFFull);
  }
  uint32_t At32(uint64_t addr) {
    uint64_t v = 0;
    mem.Read(addr, 4, &v);
    return static_cast<uint32_t>(v);
  }
  uint32_t At16(uint64_t addr) {
    uint64_t v = 0;
    mem.Read(addr, 2, &v);
    return static_cast<uint32_t>(v & 0xFFFFull);
  }
};

class Harness {
 public:
  Harness(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, uint64_t max_cycles,
          const ProgImage* img, mosaic::MemoryModel* mem)
      : dut_(dut), max_cycles_(max_cycles), imem_(img), dmem_(mem) {
    (void)reporter;
  }

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
    dut_->lane_quota_req_i = resize_req_ ? 1 : 0;
    dut_->lane_quota_val_i = static_cast<uint8_t>(resize_val_ & 0xFu);
    dut_->eval();

    if (!rst) Observe();

    if (bus_reset_.MayAccept(rst, (dut_->imem_req_valid_o != 0) &&
                                      (dut_->imem_req_ready_i != 0))) {
      imem_.Accept(dut_->imem_req_addr_o, dut_->imem_req_id_o, dut_->imem_req_epoch_o);
    }
    if (bus_reset_.MayDeliver(rst) && (dut_->imem_rsp_valid_i != 0) &&
        (dut_->imem_rsp_ready_o != 0)) {
      imem_.PopResponse();
    }
    imem_.Advance();

    if (bus_reset_.MayAccept(rst, (dut_->dmem_req_valid_o != 0) &&
                                      (dut_->dmem_req_ready_i != 0))) {
      DataMem::Request r;
      r.we = dut_->dmem_req_we_o != 0;
      r.addr = dut_->dmem_req_addr_o;
      r.size = dut_->dmem_req_size_o;
      r.wstrb = dut_->dmem_req_wstrb_o;
      r.wdata = dut_->dmem_req_wdata_o;
      dmem_.Accept(r, cycles_);
    }
    if (bus_reset_.MayDeliver(rst) && (dut_->dmem_rsp_valid_i != 0) &&
        (dut_->dmem_rsp_ready_o != 0)) {
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
  const std::vector<uint64_t>& trap_tvals() const { return trap_tvals_; }

  void Observe() {
    if (dut_->o_commit_o != retires_) {
      // The retire counter is the core's own count of retired instructions; a
      // disagreement is a core bug, not a vector one, so it is reported.
      Fail(phase_ + " at cycle " + Dec(cycles_),
           "the retire counter equals the retirement events published: counter=" +
               Dec(dut_->o_commit_o) + " events=" + Dec(retires_));
    }
    if (dut_->o_trap_valid_o != 0) {
      trap_pcs_.push_back(dut_->o_trap_epc_o);
      trap_tvals_.push_back(dut_->o_trap_tval_o);
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

  static uint64_t PackedLane(uint64_t packed, uint32_t lane, uint32_t width) {
    const uint64_t m = (width >= 64) ? ~0ull : ((1ull << width) - 1ull);
    return (packed >> (lane * width)) & m;
  }

  void SetGeometry(const Geometry& g) {
    g_ = g;
    g_.retire_width = (g_.retire_width == 0) ? 1 : g_.retire_width;
  }

  // The lane-quota request presented to the core on the next Cycle. A held
  // request is idempotent, so the driver may simply keep it asserted until the
  // broker commits it.
  void SetResize(bool req, uint32_t val) {
    resize_req_ = req;
    resize_val_ = val;
  }

 private:
  static constexpr uint64_t kMtimeBase = 0x100000000ull;
  Vmosaic_core_tb* dut_;
  uint64_t max_cycles_;
  Imem imem_;
  DataMem dmem_;
  mosaic::BusResetGate bus_reset_;
  Geometry g_;
  std::string phase_ = "init";
  uint64_t cycles_ = 0;
  uint64_t retires_ = 0;
  uint64_t mtime_ = 0;
  uint64_t last_progress_ = 0;
  std::vector<uint64_t> trap_pcs_;
  std::vector<uint64_t> trap_tvals_;

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
  uint32_t last_commit_ = 0;
  uint32_t last_alloc_ = 0;
  bool resize_req_ = false;
  uint32_t resize_val_ = 0;
};

// ------------------------------------------------------------------ program
struct Scenario {
  ProgImage image;
  std::vector<uint8_t> data = std::vector<uint8_t>(kDataSize, 0);
  std::map<uint64_t, uint8_t> extra;

  void Put64(uint64_t off, uint64_t value) {
    if (off + 8 > static_cast<uint64_t>(kDataSize)) {
      Fail("scenario data", "word at offset " + Dec(off) + " leaves the block");
    }
    for (unsigned i = 0; i < 8; i++) {
      data[off + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFu);
    }
  }
  void Put32(uint64_t off, uint32_t value) {
    if (off + 4 > static_cast<uint64_t>(kDataSize)) {
      Fail("scenario data", "word at offset " + Dec(off) + " leaves the block");
    }
    for (unsigned i = 0; i < 4; i++) {
      data[off + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFu);
    }
  }
  void Put16(uint64_t off, uint16_t value) {
    if (off + 2 > static_cast<uint64_t>(kDataSize)) {
      Fail("scenario data", "halfword at offset " + Dec(off) + " leaves the block");
    }
    for (unsigned i = 0; i < 2; i++) {
      data[off + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFu);
    }
  }
  // A word outside the 2 KiB block (the tail of RAM), written into RAM directly.
  void PutAbs64(uint64_t addr, uint64_t value) {
    for (unsigned i = 0; i < 8; i++) {
      extra[addr + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFu);
    }
  }
  void PutAbs32(uint64_t addr, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) {
      extra[addr + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFu);
    }
  }
};

void FillRun(Vmosaic_core_tb* dut, Harness* harness, mosaic::MemoryModel* mem,
             RunResult* out);

RunResult Execute(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                  const Geometry& g, const std::string& name,
                  const Scenario& sc) {
  mosaic::MemoryModel mem;
  for (int i = 0; i < kDataSize; i++) {
    if (sc.data[i] == 0) continue;
    mem.Write(kDataBase + static_cast<uint64_t>(i), 1, sc.data[i]);
  }
  for (const auto& kv : sc.extra) {
    mem.Write(kv.first, 1, kv.second);
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
  FillRun(dut, &harness, &mem, &out);
  return out;
}

// Collect every observable the case checks, in one place so a phase that drives
// the lane-quota port and a phase that does not report identically.
void FillRun(Vmosaic_core_tb* dut, Harness* harness, mosaic::MemoryModel* mem,
             RunResult* out) {
  out->cycles = harness->cycles();
  out->commits = dut->o_commit_o;
  out->vec_macro = dut->o_vec_macro_ctr_o;
  out->vec_elem = dut->o_vec_elem_ctr_o;
  out->vec_trap = dut->o_vec_trap_ctr_o;
  out->vec_retire = dut->o_vec_retire_ctr_o;
  out->vec_fault = dut->o_vec_fault_ctr_o;
  out->vec_vtype = dut->o_vec_vtype_o;
  out->vec_vl = dut->o_vec_vl_o;
  out->vec_vstart = dut->o_vec_vstart_o;
  out->vec_vcsr = dut->o_vec_vcsr_o;
  out->vec_vlenb = dut->o_vec_vlenb_o;
  out->vec_vlmax = dut->o_vec_vlmax_o;
  out->vec_vill = dut->o_vec_vill_o;
  out->vec_dbg0 = dut->o_vec_dbg0_o;
  out->vec_dbg1 = dut->o_vec_dbg1_o;
  out->vec_dbg2 = dut->o_vec_dbg2_o;
  out->vec_lsu_req = dut->o_vec_lsu_req_ctr_o;
  out->vec_alu_elems = dut->o_vec_alu_elems_o;
  out->vec_chain_accept = dut->o_vec_chain_accept_ctr_o;
  out->vec_chain_refuse = dut->o_vec_chain_refuse_ctr_o;
  out->vec_desc_alloc = dut->o_vec_desc_alloc_ctr_o;
  out->vec_desc_release = dut->o_vec_desc_release_ctr_o;
  out->vec_vrf_rd = dut->o_vec_vrf_rd_ctr_o;
  out->vec_vrf_wr = dut->o_vec_vrf_wr_ctr_o;
  out->vec_vrf_bad = dut->o_vec_vrf_bad_ctr_o;
  out->vec_vrf_rows = dut->o_vec_vrf_rows_o;
  out->vec_vrf_banks = dut->o_vec_vrf_banks_o;
  out->lane_quota = dut->o_lane_quota_o;
  out->lane_gen = dut->o_lane_gen_o;
  out->lane_publish_ctr = dut->o_lane_publish_ctr_o;
  out->lane_ack_req_ctr = dut->o_lane_ack_req_ctr_o;
  out->lane_ack_ctr = dut->o_lane_ack_ctr_o;
  out->lane_req_mid_macro_ctr = dut->o_lane_req_mid_macro_ctr_o;
  out->lane_pub_mid_macro_ctr = dut->o_lane_pub_mid_macro_ctr_o;
  out->lane_abort_ctr = dut->o_lane_abort_ctr_o;
  for (int lane = 0; lane < 8; lane++) {
    out->lane_elem[lane] = static_cast<uint32_t>(dut->o_lane_elem_ctr_o[lane]);
  }
  out->mstatus = dut->o_csr_mstatus_o;
  out->mtvec = dut->o_csr_mtvec_o;
  out->mepc = dut->o_csr_mepc_o;
  out->mcause = dut->o_csr_mcause_o;
  out->mtval = dut->o_csr_mtval_o;
  out->traps = harness->trap_pcs();
  out->trap_tvals = harness->trap_tvals();
  out->mem = *mem;
}

// One committed quota change, with whether a vector macro was live at the
// moment it was committed. The shipping rule is that every change lands with
// `macro_live == false`; a change with a macro live is the mid-macro fail mode.
struct QuotaChange {
  uint64_t cycle = 0;
  uint32_t from = 0;
  uint32_t to = 0;
  bool macro_live = false;
  // The architectural configuration sampled in the cycle the change was
  // committed: the resize must not move either.
  uint64_t vl = 0;
  uint64_t vlenb = 0;
};

struct ResizeRun {
  RunResult run;
  std::vector<QuotaChange> changes;
};

// Run a program while driving the runtime lane-quota port. Two stimulus shapes:
//
//   * `boundary_mode == false`: present `start_quota` from the first cycle, so
//     the machine is already at that share before the first vector macro. Used
//     for the "two workloads at 4 lanes" configuration.
//   * `boundary_mode == true`: request 4 the first time a *unit* macro is live
//     (a change attempted inside a macro), and 8 again while a later macro is
//     live. Every committed change is recorded with whether a macro was live
//     when it landed, which is the boundary observation the case asserts on.
ResizeRun ExecuteResize(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                        const Geometry& g, const std::string& name,
                        const Scenario& sc, uint32_t start_quota,
                        bool boundary_mode) {
  mosaic::MemoryModel mem;
  for (int i = 0; i < kDataSize; i++) {
    if (sc.data[i] == 0) continue;
    mem.Write(kDataBase + static_cast<uint64_t>(i), 1, sc.data[i]);
  }
  for (const auto& kv : sc.extra) {
    mem.Write(kv.first, 1, kv.second);
  }
  const ProgImage& image = sc.image;

  Harness harness(dut, reporter, kProgramCycles, &image, &mem);
  harness.SetGeometry(g);
  harness.Phase(name);
  dut->clk = 0;
  dut->rst = 1;
  dut->eval();
  harness.Reset(kResetCycles);

  ResizeRun out;
  bool req = false;
  uint32_t val = start_quota;
  bool reqed1 = false;
  bool reqed2 = false;
  bool prev_live = false;
  uint32_t last_q = static_cast<uint32_t>(dut->o_lane_quota_o);
  uint32_t last_desc = static_cast<uint32_t>(dut->o_vec_desc_alloc_ctr_o);

  while (!mem.finished() && harness.cycles() < kProgramCycles) {
    if (!boundary_mode) {
      req = true;
      val = start_quota;
    } else if (!reqed1) {
      // The first unit macro is live: this is a resize attempted *inside* a
      // macro, which must land only at the boundary after it drains.
      if (prev_live && (last_desc >= 1)) {
        req = true;
        val = 4;
        reqed1 = true;
      }
    } else if (!reqed2) {
      if ((last_q == 4) && prev_live && (last_desc >= 2)) {
        val = 8;
        reqed2 = true;
      }
    }
    harness.SetResize(req, val);
    harness.Cycle(false);

    const uint32_t q = static_cast<uint32_t>(dut->o_lane_quota_o);
    if (q != last_q) {
      QuotaChange change;
      change.cycle = harness.cycles();
      change.from = last_q;
      change.to = q;
      // `prev_live` is the macro-live state observed at the end of the cycle
      // before the change was committed -- the cycle the request that caused it
      // was presented.
      change.macro_live = prev_live;
      change.vl = dut->o_vec_vl_o;
      change.vlenb = dut->o_vec_vlenb_o;
      out.changes.push_back(change);
      last_q = q;
    }
    prev_live = (dut->o_lane_macro_live_o != 0);
    last_desc = static_cast<uint32_t>(dut->o_vec_desc_alloc_ctr_o);
  }
  for (int i = 0; i < 8; i++) harness.Cycle(false);
  if (!mem.finished()) {
    Fail(name, "the program never reached the exit protocol");
  }
  FillRun(dut, &harness, &mem, &out.run);
  return out;
}

void Check(mosaic::Reporter* reporter, bool ok, const std::string& what) {
  reporter->Check(ok, what);
}

// The 32-bit data pattern of the arrays. The host model of `vadd.vv` over
// 32-bit lanes is C's uint32_t addition.
constexpr uint32_t kA[4] = {0x00000001u, 0x7FFFFFFFu, 0xFFFFFFFFu, 0x12345678u};
constexpr uint32_t kB[4] = {0x00000002u, 0x00000001u, 0x00000003u, 0x0FEDCBA9u};
constexpr uint32_t kC2Prefix[2] = {0xAAAAAAAAu, 0xBBBBBBBBu};
constexpr uint32_t kC2Tail[2]   = {0xCCCCCCCCu, 0xDDDDDDDDu};

uint32_t Vadd32(uint32_t a, uint32_t b) { return a + b; }

// ============================================================================
// Phase 1: mstatus.VS = Off traps, then software enables vector state.
// ============================================================================
void PhaseVsOff(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                const Geometry& g, const std::string& name) {
  Asm asm_(g.reset_vector);
  Scenario sc;

  // The handler first, so its address is known before the main code is emitted.
  const uint32_t csr_mstatus = 0x300, csr_mtvec = 0x305, csr_mcause = 0x342,
                 csr_mepc = 0x341, csr_mtval = 0x343;

  // word 0: jal x0, main
  asm_.JalX0(0, 0);   // patched below
  const size_t handler_at = asm_.Here();
  asm_.Csrr(5, csr_mcause);
  asm_.LaAbs(6, kSlotBase);
  asm_.Sd(5, 6, 0);
  asm_.Csrr(5, csr_mepc);
  asm_.Sd(5, 6, 8);
  asm_.Csrr(5, csr_mtval);
  asm_.Sd(5, 6, 16);
  asm_.Csrr(5, csr_mepc);
  asm_.Addi(5, 5, 4);
  asm_.Csrrw(0, csr_mepc, 5);   // skip the faulting instruction
  asm_.Mret();

  const size_t main_at = asm_.Here();
  // Install the handler.
  asm_.LaAbs(5, asm_.AddrOf(handler_at));
  asm_.Csrrw(0, csr_mtvec, 5);
  // VS is Off at reset: a vsetvli must trap with an illegal instruction.
  asm_.Vsetvli(7, 0, 0x10);   // e32, m1
  // Software enables vector state (VS = Initial, 1 << 9).
  asm_.Csrr(5, csr_mstatus);
  asm_.Ori(5, 5, 0x200);
  asm_.Csrrw(0, csr_mstatus, 5);
  // The same instruction now commits; rd (x7) receives vl.
  asm_.Vsetvli(7, 0, 0x10);
  asm_.LaAbs(6, kSlotBase);
  asm_.Sd(7, 6, 24);           // vl
  asm_.Csrr(5, 0xC21);         // vtype
  asm_.Sd(5, 6, 32);
  asm_.Csrr(5, 0xC22);         // vlenb
  asm_.Sd(5, 6, 40);
  asm_.Csrr(5, csr_mstatus);
  asm_.Sd(5, 6, 48);
  asm_.Exit();

  // Patch the entry jump to main.
  const int32_t delta = static_cast<int32_t>(asm_.AddrOf(main_at) -
                                             asm_.AddrOf(0));
  std::vector<uint32_t> words = asm_.words();
  const uint32_t u = static_cast<uint32_t>(delta);
  words[0] = (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3FFu) << 21) |
             (((u >> 11) & 1u) << 20) | (((u >> 12) & 0xFFu) << 12) | 0x6f;
  for (size_t i = 0; i < words.size(); i++) {
    sc.image.Put(g.reset_vector + 4ull * i, words[i]);
  }

  RunResult run = Execute(dut, reporter, g, name, sc);
  Check(reporter, run.traps.size() == 1, name + ": exactly one trap is taken");
  Check(reporter, run.mcause == 2,
        name + ": the VS=Off vector instruction traps illegal, mcause=" +
            Dec(run.mcause));
  Check(reporter, run.vec_trap == 1,
        name + ": the vector engine counted exactly one trap, got " +
            Dec(run.vec_trap));
  Check(reporter, run.vec_retire == 1,
        name + ": exactly one vector macro retired (the enabled vsetvli), got " +
            Dec(run.vec_retire));
  Check(reporter, run.vec_elem == 0,
        name + ": no element was executed by a vset, got " + Dec(run.vec_elem));
  Check(reporter, run.Slot(3) == 4,
        name + ": vl after vsetvli e32,m1 is VLMAX=4, got " + Dec(run.Slot(3)));
  Check(reporter, (run.Slot(4) & 0xFFull) == 0x10,
        name + ": vtype low byte is the committed argument, got " + Dec(run.Slot(4)));
  Check(reporter, run.Slot(5) == 16,
        name + ": vlenb is 16, got " + Dec(run.Slot(5)));
  Check(reporter, ((run.Slot(6) >> 9) & 3ull) == 3ull,
        name + ": mstatus.VS is Dirty after the vector instruction, got " +
            Dec((run.Slot(6) >> 9) & 3ull));
  Check(reporter, run.vec_vlenb == 16,
        name + ": the core's vlenb read-back is 16, got " + Dec(run.vec_vlenb));
}

// ============================================================================
// Phase 2: load, load, vadd.vv, store -- the architectural result against the
// host model, and positive evidence the vector path executed.
// ============================================================================
RunResult PhaseArith(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                     const Geometry& g, const std::string& name) {
  Asm asm_(g.reset_vector);
  Scenario sc;
  const uint64_t a_addr = kDataBase + 0;
  const uint64_t b_addr = kDataBase + 16;
  const uint64_t c_addr = kDataBase + 32;
  for (int i = 0; i < 4; i++) {
    sc.Put32(static_cast<uint64_t>(i) * 4, kA[i]);
    sc.Put32(16 + static_cast<uint64_t>(i) * 4, kB[i]);
  }

  const uint32_t csr_mstatus = 0x300;
  asm_.LaAbs(10, a_addr);
  asm_.LaAbs(11, b_addr);
  asm_.LaAbs(12, c_addr);
  asm_.Csrr(5, csr_mstatus);
  asm_.Ori(5, 5, 0x200);
  asm_.Csrrw(0, csr_mstatus, 5);
  asm_.Vsetvli(7, 0, 0x10);    // e32, m1 -> vl = 4
  asm_.Vle32(0, 10);           // v0 = A
  asm_.Vle32(1, 11);           // v1 = B
  asm_.VaddVv(2, 0, 1);        // v2 = v0 + v1
  asm_.Vse32(2, 12);           // C = v2
  asm_.LaAbs(6, kSlotBase);
  asm_.Sd(7, 6, 0);            // vl
  asm_.Csrr(5, 0xC20);
  asm_.Sd(5, 6, 8);            // vl CSR
  asm_.Csrr(5, csr_mstatus);
  asm_.Sd(5, 6, 16);           // mstatus
  asm_.Exit();
  for (size_t i = 0; i < asm_.words().size(); i++) {
    sc.image.Put(g.reset_vector + 4ull * i, asm_.words()[i]);
  }

  RunResult run = Execute(dut, reporter, g, name, sc);
  Check(reporter, run.traps.empty(), name + ": no trap is taken");
  for (int i = 0; i < 4; i++) {
    const uint32_t got = run.Data32(32 + static_cast<uint64_t>(i) * 4);
    const uint32_t want = Vadd32(kA[i], kB[i]);
    Check(reporter, got == want,
          name + ": C[" + std::to_string(i) + "] = A+B, expected " +
              Dec(want) + " got " + Dec(got));
  }
  Check(reporter, run.Slot(0) == 4,
        name + ": vsetvli returned VLMAX=4, got " + Dec(run.Slot(0)));
  Check(reporter, run.Slot(1) == 4,
        name + ": the vl CSR reads 4, got " + Dec(run.Slot(1)));
  Check(reporter, ((run.Slot(2) >> 9) & 3ull) == 3ull,
        name + ": mstatus.VS is Dirty, got " + Dec((run.Slot(16) >> 9) & 3ull));
  // The vector path was actually taken.
  Check(reporter, run.vec_retire == 5,
        name + ": five vector macros retired, got " + Dec(run.vec_retire));
  Check(reporter, run.vec_elem == 16,
        name + ": sixteen element completions were accepted, got " +
            Dec(run.vec_elem));
  Check(reporter, run.vec_chain_refuse == 0,
        name + ": the chaining network refused no element packet, got " +
            Dec(run.vec_chain_refuse));
  Check(reporter, run.vec_desc_alloc == 4,
        name + ": four vector descriptors were allocated (load, load, add, "
               "store), got " + Dec(run.vec_desc_alloc));
  Check(reporter, run.vec_desc_release == 4,
        name + ": every allocated descriptor was released, got " +
            Dec(run.vec_desc_release));
  Check(reporter, run.vec_vrf_bad == 0,
        name + ": the VRF refused no read, got " + Dec(run.vec_vrf_bad));
  Check(reporter, run.vec_alu_elems == 4,
        name + ": the ALU wrote four elements, got " + Dec(run.vec_alu_elems));
  return run;
}

// ============================================================================
// Phase 3: a load that faults at element 2, a precise trap, and a restart from
// `vstart` that keeps the committed prefix.
// ============================================================================
void PhaseRestart(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                  const Geometry& g, const std::string& name) {
  Asm asm_(g.reset_vector);
  Scenario sc;
  const uint64_t restart_buf = kDataBase + 64;
  const uint64_t c2_addr = kDataBase + 96;
  const uint32_t csr_mstatus = 0x300, csr_mtvec = 0x305, csr_mcause = 0x342,
                 csr_mepc = 0x341, csr_mtval = 0x343;

  // The tail of RAM holds the first two elements; the buffer the handler
  // repoints to holds all four.
  sc.PutAbs32(kRamEdge + 0, kC2Prefix[0]);
  sc.PutAbs32(kRamEdge + 4, kC2Prefix[1]);
  for (int i = 0; i < 4; i++) {
    const uint32_t v = (i < 2) ? kC2Prefix[i] : kC2Tail[i - 2];
    sc.Put32(64 + static_cast<uint64_t>(i) * 4, v);
  }

  asm_.JalX0(0, 0);   // patched below
  const size_t handler_at = asm_.Here();
  asm_.Csrr(5, csr_mcause);
  asm_.LaAbs(6, kSlotBase);
  asm_.Sd(5, 6, 0);
  asm_.Csrr(5, csr_mepc);
  asm_.Sd(5, 6, 8);
  asm_.Csrr(5, csr_mtval);
  asm_.Sd(5, 6, 16);
  asm_.Csrr(5, 0x008);          // vstart
  asm_.Sd(5, 6, 24);
  asm_.Add(10, 15, 0);          // a0 = a5 (the mapped buffer)
  asm_.Mret();                  // mepc unchanged: re-execute the load

  const size_t main_at = asm_.Here();
  asm_.LaAbs(5, asm_.AddrOf(handler_at));
  asm_.Csrrw(0, csr_mtvec, 5);
  asm_.Csrr(5, csr_mstatus);
  asm_.Ori(5, 5, 0x200);
  asm_.Csrrw(0, csr_mstatus, 5);
  asm_.Vsetvli(7, 0, 0x10);     // e32, m1 -> vl = 4
  asm_.LaAbs(10, kRamEdge);     // elements 0,1 in RAM; element 2 out of it
  asm_.LaAbs(15, restart_buf);  // the handler repoints the base here
  asm_.LaAbs(12, c2_addr);
  const size_t load_at = asm_.Here();
  asm_.Vle32(3, 10);            // faults at element 2; vstart = 2
  asm_.Vse32(3, 12);
  asm_.LaAbs(6, kSlotBase);
  asm_.Csrr(5, 0x008);
  asm_.Sd(5, 6, 32);            // vstart after the successful restart
  asm_.Sd(7, 6, 40);            // vl
  asm_.Exit();

  const int32_t delta = static_cast<int32_t>(asm_.AddrOf(main_at) - asm_.AddrOf(0));
  std::vector<uint32_t> words = asm_.words();
  const uint32_t u = static_cast<uint32_t>(delta);
  words[0] = (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3FFu) << 21) |
             (((u >> 11) & 1u) << 20) | (((u >> 12) & 0xFFu) << 12) | 0x6f;
  for (size_t i = 0; i < words.size(); i++) {
    sc.image.Put(g.reset_vector + 4ull * i, words[i]);
  }

  RunResult run = Execute(dut, reporter, g, name, sc);
  Check(reporter, run.traps.size() == 1, name + ": exactly one trap is taken");
  Check(reporter, run.mcause == 5,
        name + ": the faulting element raises a load access fault (5), got " +
            Dec(run.mcause));
  Check(reporter, run.Slot(3) == 2,
        name + ": vstart at the trap is the faulting element 2, got " +
            Dec(run.Slot(3)));
  Check(reporter, run.Slot(1) == g.reset_vector + 4ull * load_at,
        name + ": mepc names the faulting load, got " + Dec(run.Slot(1)));
  Check(reporter, run.Slot(2) == 0x80200000ull,
        name + ": mtval is the faulting element's address, got " +
            Dec(run.Slot(2)));
  Check(reporter, run.vec_fault == 1,
        name + ": the engine counted one vector fault, got " + Dec(run.vec_fault));
  Check(reporter, run.vec_elem == 8,
        name + ": the phase accepted eight element packets (two from the "
               "cancelled attempt's committed prefix, two from the restart, four "
               "from the store), got " + Dec(run.vec_elem));
  // The committed prefix survived and the restart completed from vstart.
  for (int i = 0; i < 4; i++) {
    const uint32_t got = run.Data32(96 + static_cast<uint64_t>(i) * 4);
    const uint32_t want = (i < 2) ? kC2Prefix[i] : kC2Tail[i - 2];
    Check(reporter, got == want,
          name + ": restarted v3[" + std::to_string(i) + "] expected " +
              Dec(want) + " got " + Dec(got));
  }
  Check(reporter, run.Slot(4) == 0,
        name + ": vstart is reset to 0 by the completed restart, got " +
            Dec(run.Slot(4)));
  Check(reporter, run.Slot(5) == 4,
        name + ": vl is still 4 after the restart, got " + Dec(run.Slot(5)));
  Check(reporter, run.vec_retire == 3,
        name + ": the vset, the restarted load and the store retired, got " + Dec(run.vec_retire));
}

// ============================================================================
// Phase 4: the scalar-equivalent computation, for the on/off comparison.
// ============================================================================
RunResult PhaseScalar(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                      const Geometry& g, const std::string& name) {
  Asm asm_(g.reset_vector);
  Scenario sc;
  for (int i = 0; i < 4; i++) {
    sc.Put32(static_cast<uint64_t>(i) * 4, kA[i]);
    sc.Put32(16 + static_cast<uint64_t>(i) * 4, kB[i]);
  }
  const uint64_t a_addr = kDataBase + 0;
  const uint64_t b_addr = kDataBase + 16;
  const uint64_t c_addr = kDataBase + 128;
  asm_.LaAbs(10, a_addr);
  asm_.LaAbs(11, b_addr);
  asm_.LaAbs(12, c_addr);
  for (int i = 0; i < 4; i++) {
    asm_.Lw(28, 10, i * 4);
    asm_.Lw(29, 11, i * 4);
    asm_.Add(28, 28, 29);
    asm_.Sw(28, 12, i * 4);
  }
  asm_.Exit();
  for (size_t i = 0; i < asm_.words().size(); i++) {
    sc.image.Put(g.reset_vector + 4ull * i, asm_.words()[i]);
  }
  RunResult run = Execute(dut, reporter, g, name, sc);
  Check(reporter, run.traps.empty(), name + ": no trap is taken");
  for (int i = 0; i < 4; i++) {
    const uint32_t got = run.Data32(128 + static_cast<uint64_t>(i) * 4);
    const uint32_t want = Vadd32(kA[i], kB[i]);
    Check(reporter, got == want,
          name + ": C[" + std::to_string(i) + "] = A+B, expected " +
              Dec(want) + " got " + Dec(got));
  }
  Check(reporter, run.vec_retire == 0,
        name + ": the scalar program retires no vector macro, got " +
            Dec(run.vec_retire));
  return run;
}

// ============================================================================
// Phases 5-7: the runtime lane quota (I-059).
//
// The quota changes *how many lanes work on the elements*, never which elements
// exist. The three phases below state that from both sides:
//
//   5. lane-8     one workload of eight elements at quota 8 -- the one workload
//                 across eight lanes
//   6. lane-two-4 two independent four-element descriptor chains (two register
//                 regions standing in for two workloads, interleaved by the
//                 program) at quota 4
//   7. lane-resize a resize is *attempted inside a macro*; it must land only at
//                 the boundary, both sides must compute correctly, the old
//                 quota's state must be acknowledged, and `vl`/`vlenb` must not
//                 move
//
// The host model is C's `uint16_t` addition, computed from the same arrays the
// program loads; the DUT's arithmetic is never consulted.
// ============================================================================
constexpr uint16_t kA16[8] = {0x0001u, 0x7FFFu, 0xFFFFu, 0x1234u,
                              0x0020u, 0x0100u, 0x00FFu, 0xABCDu};
constexpr uint16_t kB16[8] = {0x0002u, 0x0001u, 0x0003u, 0x0001u,
                              0x0004u, 0x0010u, 0x0001u, 0x0002u};

uint16_t Sum16(uint16_t a, uint16_t b) {
  return static_cast<uint16_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

// Phase 5: one workload of eight elements at quota 8.
RunResult PhaseLaneOne8(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                        const Geometry& g, const std::string& name) {
  Asm a(g.reset_vector);
  Scenario sc;
  for (int i = 0; i < 8; i++) {
    sc.Put16(static_cast<uint64_t>(i) * 2, kA16[i]);
    sc.Put16(32 + static_cast<uint64_t>(i) * 2, kB16[i]);
  }
  a.Csrr(5, 0x300);
  a.Ori(5, 5, 0x200);
  a.Csrrw(0, 0x300, 5);
  a.Vsetvli(7, 0, 0x08);          // e16, m1, AVL=x0 -> VLMAX = 8
  a.LaAbs(10, kDataBase + 0);
  a.LaAbs(11, kDataBase + 32);
  a.LaAbs(12, kDataBase + 64);
  a.Vle16(0, 10);
  a.Vle16(1, 11);
  a.VaddVv(2, 0, 1);
  a.Vse16(2, 12);
  a.LaAbs(6, kSlotBase);
  a.Csrr(5, 0xC20);
  a.Sd(5, 6, 0);                  // vl
  a.Csrr(5, 0xC22);
  a.Sd(5, 6, 8);                  // vlenb
  a.Exit();
  for (size_t i = 0; i < a.words().size(); i++) {
    sc.image.Put(g.reset_vector + 4ull * i, a.words()[i]);
  }

  RunResult run = Execute(dut, reporter, g, name, sc);
  Check(reporter, run.traps.empty(), name + ": no trap is taken");
  Check(reporter, run.Slot(0) == 8,
        name + ": vl after vsetvli e16,m1 is VLMAX=8, got " + Dec(run.Slot(0)));
  Check(reporter, run.Slot(1) == 16,
        name + ": vlenb is 16, got " + Dec(run.Slot(1)));
  for (int i = 0; i < 8; i++) {
    const uint32_t got = run.Data16(64 + static_cast<uint64_t>(i) * 2);
    const uint32_t want = Sum16(kA16[i], kB16[i]);
    Check(reporter, got == want,
          name + ": C8[" + std::to_string(i) + "] = A+B, expected " +
              Dec(want) + " got " + Dec(got));
  }
  Check(reporter, run.lane_quota == 8,
        name + ": the committed quota is 8, got " + Dec(run.lane_quota));
  Check(reporter, run.lane_publish_ctr == 0,
        name + ": the one-workload run performs no resize, got " +
            Dec(run.lane_publish_ctr));
  uint32_t sum = 0;
  bool all_used = true;
  for (int l = 0; l < 8; l++) {
    sum += run.lane_elem[l];
    if (run.lane_elem[l] == 0) all_used = false;
  }
  Check(reporter, sum == 32,
        name + ": 32 element completions were attributed to lanes, got " +
            Dec(sum));
  Check(reporter, all_used,
        name + ": an eight-element workload used all eight lanes");
  return run;
}

// Phase 6: two independent four-element chains at quota 4. The two register
// regions (v0..v3 and v8..v11) stand in for two workloads, and the two
// descriptor chains are interleaved by the program at macro granularity.
ResizeRun PhaseLaneTwo4(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                        const Geometry& g, const std::string& name) {
  Asm a(g.reset_vector);
  Scenario sc;
  for (int i = 0; i < 8; i++) {
    sc.Put16(static_cast<uint64_t>(i) * 2, kA16[i]);
    sc.Put16(32 + static_cast<uint64_t>(i) * 2, kB16[i]);
  }
  a.Csrr(5, 0x300);
  a.Ori(5, 5, 0x200);
  a.Csrrw(0, 0x300, 5);
  a.Addi(5, 0, 4);
  a.Vsetvli(7, 5, 0x08);          // AVL=4 -> vl=4
  a.LaAbs(10, kDataBase + 0);     // A[0..3]
  a.LaAbs(13, kDataBase + 8);     // A[4..7]
  a.LaAbs(11, kDataBase + 32);    // B[0..3]
  a.LaAbs(14, kDataBase + 40);    // B[4..7]
  a.LaAbs(12, kDataBase + 96);    // C4a
  a.LaAbs(15, kDataBase + 112);   // C4b
  // Two chains, interleaved: P is A0+B0 -> C4a, Q is A4+B4 -> C4b.
  a.Vle16(0, 10);
  a.Vle16(8, 13);
  a.Vle16(1, 11);
  a.Vle16(9, 14);
  a.VaddVv(2, 0, 1);
  a.VaddVv(10, 8, 9);
  a.Vse16(2, 12);
  a.Vse16(10, 15);
  a.LaAbs(6, kSlotBase);
  a.Csrr(5, 0xC20);
  a.Sd(5, 6, 0);                  // vl
  a.Csrr(5, 0xC22);
  a.Sd(5, 6, 8);                  // vlenb
  a.Exit();
  for (size_t i = 0; i < a.words().size(); i++) {
    sc.image.Put(g.reset_vector + 4ull * i, a.words()[i]);
  }

  ResizeRun rr = ExecuteResize(dut, reporter, g, name, sc, 4, false);
  RunResult run = rr.run;
  Check(reporter, run.traps.empty(), name + ": no trap is taken");
  Check(reporter, run.Slot(0) == 4,
        name + ": vl after vsetvli with AVL=4 is 4, got " + Dec(run.Slot(0)));
  Check(reporter, run.Slot(1) == 16,
        name + ": vlenb is 16, got " + Dec(run.Slot(1)));
  for (int i = 0; i < 4; i++) {
    const uint32_t got_a = run.Data16(96 + static_cast<uint64_t>(i) * 2);
    const uint32_t want_a = Sum16(kA16[i], kB16[i]);
    Check(reporter, got_a == want_a,
          name + ": C4a[" + std::to_string(i) + "] = A+B, expected " +
              Dec(want_a) + " got " + Dec(got_a));
    const uint32_t got_b = run.Data16(112 + static_cast<uint64_t>(i) * 2);
    const uint32_t want_b = Sum16(kA16[4 + i], kB16[4 + i]);
    Check(reporter, got_b == want_b,
          name + ": C4b[" + std::to_string(i) + "] = A+B, expected " +
              Dec(want_b) + " got " + Dec(got_b));
  }
  Check(reporter, run.lane_quota == 4,
        name + ": the committed quota is 4, got " + Dec(run.lane_quota));
  Check(reporter,
        (run.lane_publish_ctr == 1) && (run.lane_ack_req_ctr == 1) &&
            (run.lane_ack_ctr == 1) && (run.lane_abort_ctr == 0),
        name + ": every resize was acknowledged (publish=" +
            Dec(run.lane_publish_ctr) + " ack_req=" + Dec(run.lane_ack_req_ctr) +
            " ack=" + Dec(run.lane_ack_ctr) + " abort=" + Dec(run.lane_abort_ctr) +
            ")");
  uint32_t sum = 0;
  bool low_used = true;
  bool high_unused = true;
  for (int l = 0; l < 4; l++) {
    sum += run.lane_elem[l];
    if (run.lane_elem[l] == 0) low_used = false;
  }
  for (int l = 4; l < 8; l++) {
    sum += run.lane_elem[l];
    if (run.lane_elem[l] != 0) high_unused = false;
  }
  Check(reporter, sum == 32,
        name + ": 32 element completions were attributed to lanes, got " +
            Dec(sum));
  Check(reporter, low_used && high_unused,
        name + ": a four-lane share used lanes 0..3 and no lane above it");
  return rr;
}

// Phase 7: a resize attempted inside a macro. It must land only at the vector
// instruction boundary, after the macro drains; both sides must be correct; the
// old quota's state must be acknowledged; and `vl`/`vlenb` must not move.
ResizeRun PhaseLaneBoundary(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                            const Geometry& g, const std::string& name) {
  Asm a(g.reset_vector);
  Scenario sc;
  for (int i = 0; i < 8; i++) {
    sc.Put16(static_cast<uint64_t>(i) * 2, kA16[i]);
    sc.Put16(32 + static_cast<uint64_t>(i) * 2, kB16[i]);
  }
  a.Csrr(5, 0x300);
  a.Ori(5, 5, 0x200);
  a.Csrrw(0, 0x300, 5);
  a.Vsetvli(7, 0, 0x08);          // e16, m1, AVL=x0 -> vl=8
  a.LaAbs(10, kDataBase + 0);
  a.LaAbs(11, kDataBase + 32);
  a.LaAbs(12, kDataBase + 64);    // C1
  a.LaAbs(16, kDataBase + 128);   // C2
  // Workload 1, then workload 2 (the same computation, so C1 == C2 == host).
  a.Vle16(0, 10);
  a.Vle16(1, 11);
  a.VaddVv(2, 0, 1);
  a.Vse16(2, 12);
  a.Vle16(4, 10);
  a.Vle16(5, 11);
  a.VaddVv(6, 4, 5);
  a.Vse16(6, 16);
  a.LaAbs(6, kSlotBase);
  a.Csrr(5, 0xC20);
  a.Sd(5, 6, 0);                  // vl
  a.Csrr(5, 0xC22);
  a.Sd(5, 6, 8);                  // vlenb
  a.Exit();
  for (size_t i = 0; i < a.words().size(); i++) {
    sc.image.Put(g.reset_vector + 4ull * i, a.words()[i]);
  }

  ResizeRun rr = ExecuteResize(dut, reporter, g, name, sc, 8, true);
  RunResult run = rr.run;
  Check(reporter, run.traps.empty(), name + ": no trap is taken");

  // 1. Every committed change landed with no macro live: the boundary rule.
  bool at_boundary = true;
  std::string offending;
  for (const QuotaChange& c : rr.changes) {
    if (c.macro_live) {
      at_boundary = false;
      offending = " (cycle " + Dec(c.cycle) + ": " + Dec(c.from) + "->" +
                  Dec(c.to) + " with a macro live)";
    }
  }
  Check(reporter, at_boundary,
        name + ": every quota change lands at a macro boundary" + offending);

  // 1b. The visible configuration did not move at any change: sampled in the
  //     cycle each change was committed.
  bool config_stable = true;
  std::string config_detail;
  for (const QuotaChange& c : rr.changes) {
    if ((c.vl != 8) || (c.vlenb != 16)) {
      config_stable = false;
      config_detail = " (cycle " + Dec(c.cycle) + ": vl=" + Dec(c.vl) +
                      " vlenb=" + Dec(c.vlenb) + ")";
    }
  }
  Check(reporter, config_stable,
        name + ": vl and vlenb are unchanged across every change" + config_detail);

  // 2. Both requested changes were committed.
  Check(reporter, rr.changes.size() == 2,
        name + ": two quota changes were committed, got " +
            Dec(static_cast<uint64_t>(rr.changes.size())));

  // 3. The old quota's state was acknowledged for every change, and at least
  //    one request was accepted while a macro was live -- the mid-macro
  //    attempt this phase exists to make.
  Check(reporter,
        (run.lane_publish_ctr == 2) && (run.lane_ack_req_ctr == 2) &&
            (run.lane_ack_ctr == 2) && (run.lane_abort_ctr == 0) &&
            (run.lane_pub_mid_macro_ctr == 0) &&
            (run.lane_req_mid_macro_ctr >= 1),
        name + ": every resize was acknowledged (publish=" +
            Dec(run.lane_publish_ctr) + " ack_req=" + Dec(run.lane_ack_req_ctr) +
            " ack=" + Dec(run.lane_ack_ctr) + " abort=" + Dec(run.lane_abort_ctr) +
            " mid_macro_publish=" + Dec(run.lane_pub_mid_macro_ctr) +
            " mid_macro_request=" + Dec(run.lane_req_mid_macro_ctr) + ")");

  // 4. Both sides of every change computed correctly.
  for (int i = 0; i < 8; i++) {
    const uint32_t want = Sum16(kA16[i], kB16[i]);
    const uint32_t got1 = run.Data16(64 + static_cast<uint64_t>(i) * 2);
    Check(reporter, got1 == want,
          name + ": C1[" + std::to_string(i) + "] = A+B, expected " +
              Dec(want) + " got " + Dec(got1));
    const uint32_t got2 = run.Data16(128 + static_cast<uint64_t>(i) * 2);
    Check(reporter, got2 == want,
          name + ": C2[" + std::to_string(i) + "] = A+B, expected " +
              Dec(want) + " got " + Dec(got2));
  }

  // 5. The visible vector length did not move with the quota.
  Check(reporter, run.Slot(0) == 8,
        name + ": vl is unchanged across the resize, got " + Dec(run.Slot(0)));
  Check(reporter, run.Slot(1) == 16,
        name + ": vlenb is 16, got " + Dec(run.Slot(1)));
  Check(reporter, run.lane_quota == 8,
        name + ": the second change committed quota 8, got " +
            Dec(run.lane_quota));
  return rr;
}

// ============================================================================
// CASE=vec.mask_prefix_at_core (task I-054).
//
// The unit-level evidence for the mask families is delivered; the core's ALU
// capability word is what decides whether the machine dispatches them. This
// case drives the operations through fetch, decode, rename, issue and the
// vector engine -- it never pokes the ALU -- and reads the results back as
// architectural state: the destination mask register is stored to memory with
// `vse8.v` and compared against the shared host model in
// sim/unit/mask_prefix_ref.h, the same model `rvv.integer_mask_permute`,
// `rvv.mask_prefix_semantics`, `rvv.mask_prefix_masked` and
// `rvv.mask_prefix_vstart` use.
//
// Phases, each a fresh reset and a fresh program:
//
//   1. plain    vmsbf/vmsif/vmsof (unmasked) over eight source patterns, the
//               result against the shared model
//   2. masked   the v0.t forms, including the 45 cells where a masked-off set
//               bit precedes an active one -- the cells the unit-level masked
//               evidence calls interesting
//   3. masklog  the eight mask-register logical ops, so the MASKLOG family's
//               advertisement is proven at the core level too
//   4. vstart   a non-zero vstart makes the mask-prefix instruction an illegal
//               instruction: mcause = 2, the trap is not an element fault, the
//               destination is untouched, and vstart is not modified (the
//               architectural rule: "vstart is not modified by vector
//               instructions that raise illegal-instruction exceptions")
// ============================================================================

// The CSRs a vector trap can report through.
constexpr uint32_t kCsrMstatus = 0x300, kCsrMtvec = 0x305, kCsrMepc = 0x341,
                   kCsrMcause = 0x342, kCsrMtval = 0x343, kCsrVstart = 0x008;

// Emit a trap handler: record mcause/mepc/mtval/vstart into four slots, then
// return to mepc + 4. Every phase installs one, so an unexpected trap is
// recorded and the phase names it rather than wandering off.
void EmitTrapHandler(Asm* a, uint64_t slot_base) {
  a->Csrr(5, kCsrMcause);
  a->LaAbs(6, slot_base);
  a->Sd(5, 6, 0);
  a->Csrr(5, kCsrMepc);
  a->Sd(5, 6, 8);
  a->Csrr(5, kCsrMtval);
  a->Sd(5, 6, 16);
  a->Csrr(5, kCsrVstart);
  a->Sd(5, 6, 24);
  a->Csrr(5, kCsrMepc);
  a->Addi(5, 5, 4);
  a->Csrrw(0, kCsrMepc, 5);
  // Software's own rule after an illegal vector instruction: clear vstart so
  // the resumed code is not restarted from the value that made the instruction
  // illegal. The value the core left is recorded above, before this write.
  a->Csrrw(0, kCsrVstart, 0);
  a->Mret();
}

// Patch the word-0 entry jump of an `Asm` to `main_at`, and copy the words
// into the scenario image.
void PlaceProgram(Asm* a, Scenario* sc, const Geometry& g, size_t main_at) {
  const int64_t delta = static_cast<int64_t>(a->AddrOf(main_at) - a->AddrOf(0));
  std::vector<uint32_t> words = a->words();
  const uint32_t u = static_cast<uint32_t>(delta);
  words[0] = (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3FFu) << 21) |
             (((u >> 11) & 1u) << 20) | (((u >> 12) & 0xFFu) << 12) | 0x6f;
  for (size_t i = 0; i < words.size(); i++) {
    sc->image.Put(g.reset_vector + 4ull * i, words[i]);
  }
}

// Enable vector state and set e8/m1 (VLMAX = 16), then install the handler.
void VecPrologue(Asm* a, size_t handler_at) {
  a->LaAbs(5, a->AddrOf(handler_at));
  a->Csrrw(0, kCsrMtvec, 5);
  a->Csrr(5, kCsrMstatus);
  a->Ori(5, 5, 0x200);            // mstatus.VS = Initial
  a->Csrrw(0, kCsrMstatus, 5);
  a->Vsetvli(7, 0, 0x00);         // vtypei e8/m1 -> vl = VLMAX = 16
}

// The eight unmasked source patterns: a first set bit at 0, 1, 7, 8 and 15,
// the all-zero boundary, and two dense patterns.
constexpr uint64_t kPfxSrc[8] = {0x0001ull, 0x0002ull, 0x0080ull, 0x0100ull,
                                 0x8000ull, 0x0000ull, 0xAAAAull, 0x00FFull};

// Phase 1: unmasked vmsbf/vmsif/vmsof over the eight patterns.
RunResult PhaseMaskPrefixPlain(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                               const Geometry& g, const std::string& name) {
  Asm asm_(g.reset_vector);
  Scenario sc;
  const int kPatterns = 8;
  const uint64_t src_base = kDataBase + 0;
  const uint64_t out_base = kDataBase + 256;
  for (int p = 0; p < kPatterns; ++p) {
    sc.Put16(static_cast<uint64_t>(16 * p), static_cast<uint16_t>(kPfxSrc[p]));
  }

  asm_.JalX0(0, 0);
  const size_t handler_at = asm_.Here();
  EmitTrapHandler(&asm_, kSlotBase);
  const size_t main_at = asm_.Here();
  VecPrologue(&asm_, handler_at);
  asm_.LaAbs(6, kSlotBase);
  asm_.Sd(7, 6, 32);              // vl
  for (int p = 0; p < kPatterns; ++p) {
    asm_.LaAbs(10, src_base + 16ull * static_cast<uint64_t>(p));
    asm_.Vle8(16, 10);            // v16 = the source mask
    for (int op = 0; op < 3; ++op) {
      const uint32_t rs1 = (op == 0) ? 1u : (op == 1) ? 3u : 2u;
      asm_.VmaskPrefix(8, 16, rs1, false);
      asm_.LaAbs(11, out_base + 16ull * static_cast<uint64_t>(p * 3 + op));
      asm_.Vse8(8, 11);           // store the destination mask register
    }
  }
  asm_.Exit();
  PlaceProgram(&asm_, &sc, g, main_at);

  RunResult run = Execute(dut, reporter, g, name, sc);
  Check(reporter, run.traps.empty(), name + ": no trap is taken");
  Check(reporter, run.Slot(4) == 16,
        name + ": vl after vsetvli e8/m1 is VLMAX=16, got " + Dec(run.Slot(4)));
  int cells = 0;
  for (int p = 0; p < kPatterns; ++p) {
    for (int op = 0; op < 3; ++op) {
      const uint32_t want = static_cast<uint32_t>(
          mosaic_maskpfx::ExpectedBits(op, kPfxSrc[p], 16));
      const uint32_t got = run.At16(out_base + 16ull * static_cast<uint64_t>(p * 3 + op));
      Check(reporter, got == want,
            name + ": " + std::string(op == 0 ? "vmsbf" : op == 1 ? "vmsif" : "vmsof") +
                " pattern" + std::to_string(p) + " mask=" + Dec(want) + " got " + Dec(got));
      ++cells;
    }
  }
  // Positive evidence the vector path was taken, not assumed.
  Check(reporter, run.vec_retire == 57,
        name + ": 57 vector macros retired (vset + 8 loads + 24 ops + 24 stores), got " +
            Dec(run.vec_retire));
  Check(reporter, run.vec_desc_alloc == 56 && run.vec_desc_release == 56,
        name + ": every vector descriptor was allocated and released (the vset "
               "does not allocate one), got alloc=" +
            Dec(run.vec_desc_alloc) + " release=" + Dec(run.vec_desc_release));
  Check(reporter, run.vec_alu_elems == 16,
        name + ": the last mask-prefix op wrote 16 elements, got " +
            Dec(run.vec_alu_elems));
  Check(reporter, run.vec_vrf_bad == 0,
        name + ": the VRF refused no read, got " + Dec(run.vec_vrf_bad));
  Check(reporter, cells == 24, name + ": 24 plain cells ran, got " + Dec(cells));
  return run;
}

// Phase 2: the masked (v0.t) forms. The 45 masked-off-before-active cells are
// the unit-level masked case's interesting cells: the source has a set bit at
// element 0, which the mask turns off, and the first ACTIVE set bit is k. A
// search over all elements would find 0; the rule finds k.
RunResult PhaseMaskPrefixMasked(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                                const Geometry& g, const std::string& name) {
  Asm asm_(g.reset_vector);
  Scenario sc;
  const uint64_t src_base = kDataBase + 0;       // 45 x 16 bytes
  const uint64_t out_base = kDataBase + 768;     // 45 x 16 bytes
  const uint64_t mask_addr = kDataBase + 1536;   // one 16-byte v0 operand
  const uint64_t seed_addr = kDataBase + 1552;   // one 16-byte destination seed
  const uint64_t kSeed = 0x5555ull;
  const uint64_t kMask = 0xFFFEull;              // element 0 masked off
  sc.Put16(mask_addr - kDataBase, static_cast<uint16_t>(kMask));
  sc.Put16(seed_addr - kDataBase, static_cast<uint16_t>(kSeed));

  int cell = 0;
  for (int op = 0; op < 3; ++op) {
    for (int k = 1; k < 16; ++k) {
      const uint64_t src = 1ull | (1ull << k);   // set at 0 (off) and at k (on)
      sc.Put16(static_cast<uint64_t>(16 * cell), static_cast<uint16_t>(src));
      ++cell;
    }
  }

  asm_.JalX0(0, 0);
  const size_t handler_at = asm_.Here();
  EmitTrapHandler(&asm_, kSlotBase);
  const size_t main_at = asm_.Here();
  VecPrologue(&asm_, handler_at);

  cell = 0;
  for (int op = 0; op < 3; ++op) {
    const uint32_t rs1 = (op == 0) ? 1u : (op == 1) ? 3u : 2u;
    for (int k = 1; k < 16; ++k) {
      asm_.LaAbs(10, src_base + 16ull * static_cast<uint64_t>(cell));
      asm_.Vle8(16, 10);                       // v16 = source
      asm_.LaAbs(11, mask_addr);
      asm_.Vle8(0, 11);                        // v0 = the mask operand
      asm_.LaAbs(12, seed_addr);
      asm_.Vle8(8, 12);                        // v8 = the destination seed
      asm_.VmaskPrefix(8, 16, rs1, true);      // vmsbf.m v8, v16, v0.t
      asm_.LaAbs(13, out_base + 16ull * static_cast<uint64_t>(cell));
      asm_.Vse8(8, 13);
      ++cell;
    }
  }
  asm_.Exit();
  PlaceProgram(&asm_, &sc, g, main_at);

  RunResult run = Execute(dut, reporter, g, name, sc);
  Check(reporter, run.traps.empty(), name + ": no trap is taken");
  cell = 0;
  int masked_before = 0;
  for (int op = 0; op < 3; ++op) {
    for (int k = 1; k < 16; ++k) {
      const uint64_t src = 1ull | (1ull << k);
      const uint32_t want = static_cast<uint32_t>(mosaic_maskpfx::MaskedExpectedBits(
          op, src, kMask, kSeed, 16, 0, 0));
      const uint32_t got =
          run.At16(out_base + 16ull * static_cast<uint64_t>(cell));
      Check(reporter, got == want,
            name + ": masked-off-before-active k=" + std::to_string(k) + " " +
                std::string(op == 0 ? "vmsbf" : op == 1 ? "vmsif" : "vmsof") +
                " mask=" + Dec(want) + " got " + Dec(got));
      ++cell;
      ++masked_before;
    }
  }
  Check(reporter, masked_before == 45,
        name + ": 45 masked-off-before-active cells ran, got " + Dec(masked_before));
  Check(reporter, run.vec_retire == 1 + 45 * 5,
        name + ": 226 vector macros retired (vset + 45 x (source load + mask load "
               "+ seed load + op + store)), got " +
            Dec(run.vec_retire));
  Check(reporter, run.vec_desc_release == run.vec_desc_alloc,
        name + ": every vector descriptor was released, got alloc=" +
            Dec(run.vec_desc_alloc) + " release=" + Dec(run.vec_desc_release));
  Check(reporter, run.vec_alu_elems == 15,
        name + ": the last masked op wrote 15 elements (element 0 is masked "
               "off), got " +
            Dec(run.vec_alu_elems));
  Check(reporter, run.vec_vrf_bad == 0,
        name + ": the VRF refused no read, got " + Dec(run.vec_vrf_bad));
  return run;
}

// Phase 3: the eight mask-register logical ops, over two source-mask pairs.
RunResult PhaseMaskLog(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                       const Geometry& g, const std::string& name) {
  Asm asm_(g.reset_vector);
  Scenario sc;
  const uint64_t a_base = kDataBase + 0;
  const uint64_t b_base = kDataBase + 64;
  const uint64_t out_base = kDataBase + 128;
  // funct6 per ALU op: 0 vmand, 1 vmnand, 2 vmor, 3 vmnor, 4 vmxor, 5 vmxnor,
  // 6 vmandn, 7 vmorn.
  const uint32_t kFunct6[8] = {0x19u, 0x1du, 0x1au, 0x1eu,
                               0x1bu, 0x1fu, 0x18u, 0x1cu};
  const uint64_t kMaskA[2] = {0xAAAAull, 0x8001ull};
  const uint64_t kMaskB[2] = {0x00FFull, 0xFFFFull};
  for (int p = 0; p < 2; ++p) {
    sc.Put16(static_cast<uint64_t>(16 * p), static_cast<uint16_t>(kMaskA[p]));
    sc.Put16(64 + static_cast<uint64_t>(16 * p), static_cast<uint16_t>(kMaskB[p]));
  }

  asm_.JalX0(0, 0);
  const size_t handler_at = asm_.Here();
  EmitTrapHandler(&asm_, kSlotBase);
  const size_t main_at = asm_.Here();
  VecPrologue(&asm_, handler_at);
  for (int p = 0; p < 2; ++p) {
    asm_.LaAbs(10, a_base + 16ull * static_cast<uint64_t>(p));
    asm_.Vle8(16, 10);            // v16 = vs2
    asm_.LaAbs(11, b_base + 16ull * static_cast<uint64_t>(p));
    asm_.Vle8(24, 11);            // v24 = vs1
    for (int op = 0; op < 8; ++op) {
      asm_.VmaskLog(8, 16, 24, kFunct6[op]);
      asm_.LaAbs(12, out_base + 16ull * static_cast<uint64_t>(p * 8 + op));
      asm_.Vse8(8, 12);
    }
  }
  asm_.Exit();
  PlaceProgram(&asm_, &sc, g, main_at);

  RunResult run = Execute(dut, reporter, g, name, sc);
  Check(reporter, run.traps.empty(), name + ": no trap is taken");
  int cells = 0;
  for (int p = 0; p < 2; ++p) {
    for (int op = 0; op < 8; ++op) {
      const uint32_t want = static_cast<uint32_t>(
          mosaic_maskpfx::MaskLogExpectedBits(op, kMaskA[p], kMaskB[p], 16));
      const uint32_t got = run.At16(out_base + 16ull * static_cast<uint64_t>(p * 8 + op));
      Check(reporter, got == want,
            name + ": masklog op" + std::to_string(op) + " pair" + std::to_string(p) +
                " mask=" + Dec(want) + " got " + Dec(got));
      ++cells;
    }
  }
  Check(reporter, cells == 16, name + ": 16 masklog cells ran, got " + Dec(cells));
  Check(reporter, run.vec_retire == 1 + 2 * (2 + 8 + 8),
        name + ": 37 vector macros retired, got " + Dec(run.vec_retire));
  Check(reporter, run.vec_alu_elems == 16,
        name + ": the last masklog op wrote 16 elements, got " + Dec(run.vec_alu_elems));
  Check(reporter, run.vec_vrf_bad == 0,
        name + ": the VRF refused no read, got " + Dec(run.vec_vrf_bad));
  return run;
}

// Phase 4: the vstart illegal routing. With a non-zero vstart the mask-prefix
// instruction is refused as an illegal instruction (the unit rule
// `rvv.mask_prefix_vstart` states); at the core level the refusal must route to
// an illegal-instruction trap -- mcause = 2, the PC of the instruction, no
// element fault -- and, because the architectural rule is that vstart is not
// modified by a vector instruction that raises an illegal-instruction
// exception, the vstart the handler reads is the value software wrote.
RunResult PhaseMaskPrefixVstart(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                                const Geometry& g, const std::string& name) {
  Asm asm_(g.reset_vector);
  Scenario sc;
  const uint64_t src_addr = kDataBase + 0;
  const uint64_t seed_addr = kDataBase + 16;
  const uint64_t out_addr = kDataBase + 32;
  const uint64_t kSeed = 0x5A5Aull;
  const uint64_t kSrc = 0x8001ull;
  sc.Put16(src_addr - kDataBase, static_cast<uint16_t>(kSrc));
  sc.Put16(seed_addr - kDataBase, static_cast<uint16_t>(kSeed));

  asm_.JalX0(0, 0);
  const size_t handler_at = asm_.Here();
  EmitTrapHandler(&asm_, kSlotBase);
  const size_t main_at = asm_.Here();
  VecPrologue(&asm_, handler_at);
  asm_.LaAbs(10, src_addr);
  asm_.Vle8(16, 10);                 // v16 = source
  asm_.LaAbs(11, seed_addr);
  asm_.Vle8(8, 11);                  // v8 = destination seed
  asm_.Addi(5, 0, 2);
  asm_.Csrrw(0, kCsrVstart, 5);      // vstart = 2 (after the load that clears it)
  const uint64_t prefix_pc = asm_.AddrOf(asm_.Here());
  asm_.VmaskPrefix(8, 16, 1, false); // vmsbf.m v8, v16 -- illegal at vstart=2
  asm_.LaAbs(12, out_addr);
  asm_.Vse8(8, 12);                  // store the destination (must be unchanged)
  asm_.Exit();
  PlaceProgram(&asm_, &sc, g, main_at);

  RunResult run = Execute(dut, reporter, g, name, sc);
  Check(reporter, run.traps.size() == 1,
        name + ": exactly one trap is taken, got " +
            Dec(static_cast<uint64_t>(run.traps.size())));
  Check(reporter, run.Slot(0) == 2,
        name + ": the refusal is an illegal instruction (mcause=2), got " +
            Dec(run.Slot(0)));
  Check(reporter, run.Slot(1) == prefix_pc,
        name + ": the trap reports the instruction's own PC " + Dec(prefix_pc) +
            ", got " + Dec(run.Slot(1)));
  Check(reporter, run.Slot(2) == 0,
        name + ": mtval is 0 (no element address), got " + Dec(run.Slot(2)));
  Check(reporter, run.Slot(3) == 2,
        name + ": vstart is not modified by the illegal-instruction exception, got " +
            Dec(run.Slot(3)));
  Check(reporter, run.At16(out_addr) == static_cast<uint32_t>(kSeed),
        name + ": the refused instruction left the destination unchanged, expected " +
            Dec(kSeed) + " got " + Dec(run.At16(out_addr)));
  Check(reporter, run.vec_trap == 1,
        name + ": the vector engine counted exactly one trap, got " + Dec(run.vec_trap));
  Check(reporter, run.vec_fault == 0,
        name + ": no element fault was reported, got " + Dec(run.vec_fault));
  Check(reporter, run.vec_retire == 4,
        name + ": four vector macros retired (vset + 2 loads + the store; the "
               "refused op did not), got " +
            Dec(run.vec_retire));
  return run;
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
                           Dec(geometry.reset_vector));
    }
    if (geometry.xlen != 64) Fail("geometry", "the profile is not 64-bit");

    if (options.case_id == "vec.mask_prefix_at_core") {
      RunResult plain = PhaseMaskPrefixPlain(&dut, &reporter, geometry, "prefix-plain");
      RunResult masked = PhaseMaskPrefixMasked(&dut, &reporter, geometry, "prefix-masked");
      RunResult masklog = PhaseMaskLog(&dut, &reporter, geometry, "masklog");
      RunResult vstart = PhaseMaskPrefixVstart(&dut, &reporter, geometry, "vstart");
      detail = "checks=" + Dec(static_cast<uint64_t>(reporter.checks())) +
               " phases=4 seed=" + Dec(options.seed) +
               " plain_cycles=" + Dec(plain.cycles) +
               " masked_cycles=" + Dec(masked.cycles) +
               " masklog_cycles=" + Dec(masklog.cycles) +
               " vstart_cycles=" + Dec(vstart.cycles) +
               " plain_retire=" + Dec(plain.vec_retire) +
               " masked_retire=" + Dec(masked.vec_retire) +
               " masklog_retire=" + Dec(masklog.vec_retire);
    } else {
    PhaseVsOff(&dut, &reporter, geometry, "vs-off");
    RunResult vec = PhaseArith(&dut, &reporter, geometry, "arith");
    PhaseRestart(&dut, &reporter, geometry, "restart");
    RunResult scalar = PhaseScalar(&dut, &reporter, geometry, "scalar");
    // The on/off comparison: the same computation run both ways must produce
    // identical bytes, and the cycle counts are reported honestly -- including
    // when the vector path is slower, which the barrier and the per-macro
    // overhead make it for four elements.
    for (int i = 0; i < 4; i++) {
      const uint64_t off = static_cast<uint64_t>(i) * 4;
      Check(&reporter, vec.Data32(32 + off) == scalar.Data32(128 + off),
            std::string("on/off: C[") + std::to_string(i) +
                "] is identical vector vs scalar");
    }

    // The runtime lane quota: one workload at 8 lanes, two workloads at 4 lanes,
    // and a resize attempted inside a macro. The lane quota changes how many
    // lanes work on the elements, never which elements exist, so the two
    // workloads' outputs must reproduce the single workload's output exactly.
    RunResult lane8 = PhaseLaneOne8(&dut, &reporter, geometry, "lane-8");
    ResizeRun lane4 = PhaseLaneTwo4(&dut, &reporter, geometry, "lane-two-4");
    ResizeRun lane_resize = PhaseLaneBoundary(&dut, &reporter, geometry, "lane-resize");
    for (int i = 0; i < 4; i++) {
      Check(&reporter,
            lane8.Data16(64 + static_cast<uint64_t>(i) * 2) ==
                lane4.run.Data16(96 + static_cast<uint64_t>(i) * 2),
            std::string("lane equivalence: C[") + std::to_string(i) +
                "] of the one-workload-at-8 run equals the first workload at 4");
      Check(&reporter,
            lane8.Data16(64 + static_cast<uint64_t>(4 + i) * 2) ==
                lane4.run.Data16(112 + static_cast<uint64_t>(i) * 2),
            std::string("lane equivalence: C[") + std::to_string(4 + i) +
                "] of the one-workload-at-8 run equals the second workload at 4");
    }

    uint64_t lane8_sum = 0;
    uint64_t lane4_sum = 0;
    uint64_t lane4_high = 0;
    for (int l = 0; l < 8; l++) {
      lane8_sum += lane8.lane_elem[l];
      lane4_sum += lane4.run.lane_elem[l];
      if (l >= 4) lane4_high += lane4.run.lane_elem[l];
    }

    detail = "checks=" + Dec(static_cast<uint64_t>(reporter.checks())) +
             " phases=7 seed=" + Dec(options.seed) +
             " vec_cycles=" + Dec(vec.cycles) +
             " scalar_cycles=" + Dec(scalar.cycles) +
             " lane8_quota=" + Dec(lane8.lane_quota) +
             " lane8_elems=" + Dec(lane8_sum) +
             " lane4_quota=" + Dec(lane4.run.lane_quota) +
             " lane4_elems=" + Dec(lane4_sum) +
             " lane4_high_elems=" + Dec(lane4_high) +
             " resize_quota=" + Dec(lane_resize.run.lane_quota) +
             " resizes=" + Dec(lane_resize.run.lane_publish_ctr) +
             " acks=" + Dec(lane_resize.run.lane_ack_ctr);
    }
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "every vector-integration claim holds on this machine",
                      "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  passed = passed && (reporter.failures() == 0);
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
