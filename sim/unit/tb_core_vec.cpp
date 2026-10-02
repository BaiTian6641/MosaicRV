// ============================================================================
// tb_core_vec.cpp -- CASE=vec.integrated, work package I-059.
//
// The DUT is the integrated p0 out-of-order core with the vector engine wired
// in: the OP-V decode in the front end, the dispatch system-insert route, the
// ROB-head resolution, the I-051..I-058 units (descriptor, configuration, VRF,
// integer ALU, memory packetizer, restart controller, chaining network), the
// vector CSRs on the core's CSR path, and mstatus.VS dirtying.
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
//
// Controls: tools/run_vec_integrated_controls.py injects exactly one defect per
// build, requires the mutant binary to differ from the shipping one and to exit
// 1 with the named check. See results/reports/I-059-vector-integration.md.
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
  // vadd.vv vd, vs2, vs1  -- OPIVV, funct6 000000, vm=1.
  void VaddVv(uint32_t vd, uint32_t vs2, uint32_t vs1) {
    Emit((1u << 25) | (vs2 << 20) | (vs1 << 15) | (0u << 12) | (vd << 7) | 0x57);
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
  uint64_t mstatus = 0;
  uint64_t mtvec = 0;
  uint64_t mepc = 0;
  uint64_t mcause = 0;
  uint64_t mtval = 0;
  std::vector<uint64_t> traps;
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
  uint32_t At32(uint64_t addr) {
    uint64_t v = 0;
    mem.Read(addr, 4, &v);
    return static_cast<uint32_t>(v);
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

 private:
  static constexpr uint64_t kMtimeBase = 0x100000000ull;
  Vmosaic_core_tb* dut_;
  mosaic::Reporter* reporter_;
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
  out.cycles = harness.cycles();
  out.commits = dut->o_commit_o;
  out.vec_macro = dut->o_vec_macro_ctr_o;
  out.vec_elem = dut->o_vec_elem_ctr_o;
  out.vec_trap = dut->o_vec_trap_ctr_o;
  out.vec_retire = dut->o_vec_retire_ctr_o;
  out.vec_fault = dut->o_vec_fault_ctr_o;
  out.vec_vtype = dut->o_vec_vtype_o;
  out.vec_vl = dut->o_vec_vl_o;
  out.vec_vstart = dut->o_vec_vstart_o;
  out.vec_vcsr = dut->o_vec_vcsr_o;
  out.vec_vlenb = dut->o_vec_vlenb_o;
  out.vec_vlmax = dut->o_vec_vlmax_o;
  out.vec_vill = dut->o_vec_vill_o;
  out.vec_dbg0 = dut->o_vec_dbg0_o;
  out.vec_dbg1 = dut->o_vec_dbg1_o;
  out.vec_dbg2 = dut->o_vec_dbg2_o;
  out.vec_lsu_req = dut->o_vec_lsu_req_ctr_o;
  out.vec_alu_elems = dut->o_vec_alu_elems_o;
  out.vec_chain_accept = dut->o_vec_chain_accept_ctr_o;
  out.vec_chain_refuse = dut->o_vec_chain_refuse_ctr_o;
  out.vec_desc_alloc = dut->o_vec_desc_alloc_ctr_o;
  out.vec_desc_release = dut->o_vec_desc_release_ctr_o;
  out.vec_vrf_rd = dut->o_vec_vrf_rd_ctr_o;
  out.vec_vrf_wr = dut->o_vec_vrf_wr_ctr_o;
  out.vec_vrf_bad = dut->o_vec_vrf_bad_ctr_o;
  out.vec_vrf_rows = dut->o_vec_vrf_rows_o;
  out.vec_vrf_banks = dut->o_vec_vrf_banks_o;
  out.mstatus = dut->o_csr_mstatus_o;
  out.mtvec = dut->o_csr_mtvec_o;
  out.mepc = dut->o_csr_mepc_o;
  out.mcause = dut->o_csr_mcause_o;
  out.mtval = dut->o_csr_mtval_o;
  out.traps = harness.trap_pcs();
  out.mem = mem;
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
  asm_.Vsetvli(7, 0, 0x28);   // e32, m1
  // Software enables vector state (VS = Initial, 1 << 9).
  asm_.Csrr(5, csr_mstatus);
  asm_.Ori(5, 5, 0x200);
  asm_.Csrrw(0, csr_mstatus, 5);
  // The same instruction now commits; rd (x7) receives vl.
  asm_.Vsetvli(7, 0, 0x28);
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
  Check(reporter, run.Slot(24) == 4,
        name + ": vl after vsetvli e32,m1 is VLMAX=4, got " + Dec(run.Slot(24)));
  Check(reporter, (run.Slot(32) & 0xFFull) == 0x28,
        name + ": vtype low byte is the committed argument, got " + Dec(run.Slot(32)));
  Check(reporter, run.Slot(40) == 16,
        name + ": vlenb is 16, got " + Dec(run.Slot(40)));
  Check(reporter, ((run.Slot(48) >> 9) & 3ull) == 3ull,
        name + ": mstatus.VS is Dirty after the vector instruction, got " +
            Dec((run.Slot(48) >> 9) & 3ull));
  Check(reporter, run.vec_vlenb == 16,
        name + ": the core's vlenb read-back is 16, got " + Dec(run.vec_vlenb));
}

// ============================================================================
// Phase 2: load, load, vadd.vv, store -- the architectural result against the
// host model, and positive evidence the vector path executed.
// ============================================================================
void PhaseArith(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
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
  asm_.Vsetvli(7, 0, 0x28);    // e32, m1 -> vl = 4
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
  Check(reporter, run.Slot(8) == 4,
        name + ": the vl CSR reads 4, got " + Dec(run.Slot(8)));
  Check(reporter, ((run.Slot(16) >> 9) & 3ull) == 3ull,
        name + ": mstatus.VS is Dirty, got " + Dec((run.Slot(16) >> 9) & 3ull));
  // The vector path was actually taken.
  Check(reporter, run.vec_retire == 4,
        name + ": four vector macros retired, got " + Dec(run.vec_retire));
  Check(reporter, run.vec_lsu_req == 8,
        name + ": the packetizer issued 8 memory requests (two loads + one "
               "store of four elements), got " + Dec(run.vec_lsu_req));
  Check(reporter, run.vec_elem == 8,
        name + ": eight element completions were accepted, got " +
            Dec(run.vec_elem));
  Check(reporter, run.vec_chain_refuse == 0,
        name + ": the chaining network refused no element packet, got " +
            Dec(run.vec_chain_refuse));
  Check(reporter, run.vec_desc_alloc == 3,
        name + ": three vector descriptors were allocated (load, load, add; a "
               "store has no destination group), got " + Dec(run.vec_desc_alloc));
  Check(reporter, run.vec_desc_release == 3,
        name + ": every allocated descriptor was released, got " +
            Dec(run.vec_desc_release));
  Check(reporter, run.vec_vrf_bad == 0,
        name + ": the VRF refused no read, got " + Dec(run.vec_vrf_bad));
  Check(reporter, run.vec_alu_elems == 4,
        name + ": the ALU wrote four elements, got " + Dec(run.vec_alu_elems));
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
  asm_.Vsetvli(7, 0, 0x28);     // e32, m1 -> vl = 4
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
  Check(reporter, run.Slot(24) == 2,
        name + ": vstart at the trap is the faulting element 2, got " +
            Dec(run.Slot(24)));
  Check(reporter, run.Slot(8) == g.reset_vector + 4ull * load_at,
        name + ": mepc names the faulting load, got " + Dec(run.Slot(8)));
  Check(reporter, run.Slot(16) == 0x80200000ull,
        name + ": mtval is the faulting element's address, got " +
            Dec(run.Slot(16)));
  Check(reporter, run.vec_fault == 1,
        name + ": the engine counted one vector fault, got " + Dec(run.vec_fault));
  // The committed prefix survived and the restart completed from vstart.
  for (int i = 0; i < 4; i++) {
    const uint32_t got = run.Data32(96 + static_cast<uint64_t>(i) * 4);
    const uint32_t want = (i < 2) ? kC2Prefix[i] : kC2Tail[i - 2];
    Check(reporter, got == want,
          name + ": restarted v3[" + std::to_string(i) + "] expected " +
              Dec(want) + " got " + Dec(got));
  }
  Check(reporter, run.Slot(32) == 0,
        name + ": vstart is reset to 0 by the completed restart, got " +
            Dec(run.Slot(32)));
  Check(reporter, run.Slot(40) == 4,
        name + ": vl is still 4 after the restart, got " + Dec(run.Slot(40)));
  Check(reporter, run.vec_retire == 2,
        name + ": the vset and the restarted load retired (the faulted attempt "
               "did not), got " + Dec(run.vec_retire));
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

    PhaseVsOff(&dut, &reporter, geometry, "vs-off");
    PhaseArith(&dut, &reporter, geometry, "arith");
    PhaseRestart(&dut, &reporter, geometry, "restart");
    const RunResult scalar = PhaseScalar(&dut, &reporter, geometry, "scalar");
    (void)scalar;

    detail = "checks=" + Dec(static_cast<uint64_t>(reporter.checks())) +
             " phases=4 seed=" + Dec(options.seed);
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
