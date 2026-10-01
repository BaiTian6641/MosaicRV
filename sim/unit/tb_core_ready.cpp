// ============================================================================
// tb_core_ready.cpp -- CASE=core.unwritten_reg_read, work package I-023.
//
// The DUT is the integrated p0 core, the same top CASE=core.corpus_branch and
// CASE=fabric.fixed_two_cluster drive: fetch -> decode buffer -> dispatch -> two
// clusters -> writeback arbiter -> ROB -> in-order retire -> commit to rename.
//
// ------------------------------------------------------------- the defect
//
// Reset maps architectural register i to physical tag i at generation 0, and the
// free list owns those 32 tags (mosaic_rename.sv). Nothing ever writes them, so
// the writeback arbiter's ready table never marks them written and the register
// file never holds a value for them. Dispatch, though, decided readiness only
// through that table: a source resolving to an initial mapping was inserted into
// the issue queue **not ready**, to be woken by a broadcast for its
// (tag, generation). No producer exists for that identity, so no broadcast can
// come; the issue queue waits forever, the ROB head never completes, and nothing
// retires.
//
// CASE=core.corpus_branch missed it only because `p02_branch` happens to write
// every register it reads. Any program that reads a register it has not written
// -- a crt0, or a program that initialises a register on one path only --
// deadlocks. This case is that program, reduced to four instructions.
//
// --------------------------------------------------------------- the rule
//
// The architectural contract is that a read of a never-written register yields
// zero: `mosaic_bringup_core.sv` already models an architectural register file
// that reads 0 before any write, and the corpus and `tools/host_oracle.py`
// assume zero-initialised registers. `mosaic_rename.sv` therefore reports the
// architectural initial mapping on its source read ports -- `rs1_init`/`rs2_init`
// are high when the source does not address x0 and its tag has never been
// allocated since reset -- and `mosaic_dispatch.sv` folds such a source into a
// ready constant zero, the same slot AUIPC's PC and the ALU's immediate already
// use. The operand is then never inserted not-ready, so there is no wakeup to
// wait for.
//
// The flag is exact rather than a heuristic (rename's header gives the argument:
// `gen_valid` is set by allocation and cleared only by the undo of that same
// allocation), so:
//
//   * x0 keeps its own rule -- `rs*_is_x0` -- and the fold excludes it;
//   * a register a *real producer* wrote takes the ordinary ready/wakeup path
//     and can never be shadowed by the constant, because its mapping came from
//     an allocation and that tag's `gen_valid` is set.
//
// -------------------------------------------------------------- the program
//
// Hand-encoded into the memory model; nothing under tests/programs/ is touched.
// Assembled with `riscv64-elf-as -march=rv64im` from
//
//     add  x5, x31, x31    # the FIRST instruction reads x31, which the program
//                          # never writes: the deadlock this case reproduces
//     addi x6, x0, 9       # a producer whose value is not zero
//     add  x7, x31, x6     # reads the never-written x31 *and* the written x6
//     ecall                # refused by dispatch, which stops the machine
//
// The listing and the assembler command are in results/reports/I-023-readiness.md.
//
// Instruction 2 is what makes the case check both halves of the rule. It must
// read x31 as 0 (the initial mapping) and x6 as 9 (a real producer, through the
// ordinary wakeup path), so `x7 = 0 + 9 = 9`. An over-eager constant path that
// treated every source as the initial mapping would make x7 = 0, and a case that
// only checked "the machine retires something" would not notice.
//
// ------------------------------------------------------------ what it checks
//
//   1. the three instructions retire, in program order, with the ISA's values,
//      including the one that reads x31;
//   2. the canonical retirement event stream equals the hand-computed
//      expectation, via sim/common/event_tap.cpp;
//   3. the machine stops cleanly at the refused ECALL and quiesces at a rename
//      boundary with an empty ROB, with no illegal decode and no squash;
//   4. the program really did read a never-written register: at the end of the
//      run x31's mapping is still (tag 31, generation 0) with `gen_valid` clear
//      and `wb_done` clear, read straight off the DUT's rename state through
//      o_dbg_spec_map / o_dbg_gen_valid / o_dbg_wb_done;
//   5. the registers the program *did* write have allocated mappings, so the
//      constant path cannot have shadowed them.
//
// ---------------------------------------------------- what failure looks like
//
// A stall is not a timeout: the case fails, by name, at cycle kStallWindow with
//
//   RESULT FAIL core.unwritten_reg_read contract violated: no retire within 2000
//   cycles: ROB head pc=0x80000000 (program word 0, reads x31) retired=0 ... |
//   initial mappings no wakeup can reach: x31->(tag 31, generation 0)
//   gen_valid=0 wb_done=0 ...
//
// which names the register and the (tag, generation) that stalled rather than
// merely hanging until --max-cycles.
//
// ------------------------------------------------------------- not covered
//
// The two-wide lane-1 source path: the integrated core is one-wide (the ROB has
// a single allocation port) and ties rename's lane-1 ports off, so the initial
// mapping is only ever folded on lanes 0's ports. Operand-collector
// retry/latency behaviour (I-027) is not exercised either: this case's operands
// are an immediate, x0, the initial-mapping constant, and one single-cycle ALU
// producer, so the bank-conflict retry (dispatch's `src_conflict`/`hold_val`)
// never fires. Loads, stores, CSRs and traps are not in this package; dispatch
// refuses them, which is why the program ends in an ECALL.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "Vmosaic_core_tb.h"
#include "event_tap.h"
#include "memory_model.h"
#include "sim_common.h"

namespace {

// ------------------------------------------------------------------ failure
// Thrown on the first failed check, so the report names one defect at one cycle
// instead of a thousand consequences of it.
struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string Dec(uint64_t value) { return std::to_string(value); }

// ============================================================================
// The program (CASE=core.unwritten_reg_read)
// ============================================================================
const uint32_t kProgram[] = {
    0x01ff82b3u,  // add  x5, x31, x31
    0x00900313u,  // addi x6, x0, 9
    0x006f83b3u,  // add  x7, x31, x6
    0x00000073u,  // ecall
};
constexpr int kProgWords = static_cast<int>(sizeof(kProgram) / sizeof(kProgram[0]));
constexpr uint32_t kEcallWord = 0x00000073u;
// ECALL for every word past the program, so a fetch that runs off the end of the
// program stops the machine as a refused macro rather than as an illegal decode.
constexpr int kFillerWords = 8;

// The architectural result the ISA gives, instruction by instruction. Written
// from the ISA text, not from the DUT.
struct Expect {
  uint64_t pc_offset;
  uint32_t rd;
  uint64_t value;
};
const Expect kExpected[3] = {
    {0x0, 5, 0},  // add  x5, x31, x31 -> 0 + 0
    {0x4, 6, 9},  // addi x6, x0, 9  -> 0 + 9
    {0x8, 7, 9},  // add  x7, x31, x6 -> 0 + 9
};
constexpr int kExpectedRetires = 3;

// The bounded observation window. Under the defect nothing retires at all, and
// the case must say so rather than sit until --max-cycles. A correct run retires
// the whole program in a few tens of cycles.
constexpr uint64_t kStallWindow = 2000;
constexpr int kResetCycles = 4;

// The source registers a program word reads, for the diagnostic below. It is
// not an expectation: nothing checks the DUT against it.
std::vector<uint32_t> UniqueSources(uint32_t word) {
  std::vector<uint32_t> addresses;
  const uint32_t opcode = word & 0x7fu;
  addresses.push_back((word >> 15) & 0x1fu);
  if (opcode == 0x33u) {
    const uint32_t rs2 = (word >> 20) & 0x1fu;
    if (rs2 != addresses[0]) addresses.push_back(rs2);
  }
  return addresses;
}

// ============================================================================
// Geometry, read out of the elaborated DUT (never re-derived)
// ============================================================================
struct Geometry {
  uint32_t xlen = 0, clusters = 0, retire_width = 0, rob_entries = 0;
  uint32_t rob_index_w = 0, rob_gen_w = 0, uop_id_w = 0, uop_index_w = 0;
  uint32_t prf_entries = 0, prf_tag_w = 0, int_gen_w = 0;
  uint32_t seq_w = 0, ret_id_w = 0;
  uint64_t reset_vector = 0;
};

Geometry ReadGeometry(Vmosaic_core_tb* dut) {
  Geometry g;
  g.xlen = dut->o_geom_xlen_o;
  g.clusters = dut->o_geom_clusters_o;
  g.retire_width = dut->o_geom_retire_width_o;
  g.rob_entries = dut->o_geom_rob_entries_o;
  g.rob_index_w = dut->o_geom_rob_index_w_o;
  g.rob_gen_w = dut->o_geom_rob_gen_w_o;
  g.uop_index_w = dut->o_geom_uop_index_w_o;
  g.uop_id_w = dut->o_geom_uop_id_w_o;
  g.prf_entries = dut->o_geom_prf_entries_o;
  g.prf_tag_w = dut->o_geom_prf_tag_w_o;
  g.int_gen_w = dut->o_geom_int_gen_w_o;
  g.seq_w = dut->o_geom_seq_w_o;
  g.ret_id_w = dut->o_geom_ret_id_w_o;
  g.reset_vector = dut->o_geom_reset_vector_o;
  return g;
}

// ============================================================================
// Wide and packed port access
// ============================================================================
// A 128-bit event payload (`ev_pc`, `ev_value`) is two 64-bit lanes, so
// Verilator hands it over as a `VlWide` indexed in 32-bit words; the small
// per-lane fields (`ev_rd`, `ev_seq`) are packed by *field width*.
template <typename Wide>
uint64_t PayloadLane(const Wide& wide, uint32_t lane) {
  return static_cast<uint64_t>(wide[lane * 2]) |
         (static_cast<uint64_t>(wide[lane * 2 + 1]) << 32);
}

uint64_t PackedLane(uint64_t packed, uint32_t lane, uint32_t width) {
  if (width >= 64) return packed;
  return (packed >> (lane * width)) & ((1ull << width) - 1ull);
}

// One bit field out of a wide port, lsb-relative.
template <typename Wide>
uint64_t WideBits(const Wide& wide, unsigned lsb, unsigned width) {
  uint64_t out = 0;
  for (unsigned i = 0; i < width && i < 64; i++) {
    const unsigned bit = lsb + i;
    out |= static_cast<uint64_t>((wide[bit / 32] >> (bit % 32)) & 1u) << i;
  }
  return out;
}

// ============================================================================
// The instruction memory: the memory model, with a one-cycle handshake
// ============================================================================
// The program is written into `mosaic::MemoryModel` (RAM at the profile's reset
// vector) and every fetch is served from it, so an address the map does not
// cover faults instead of silently reading as zero.
class Imem {
 public:
  struct Request {
    uint64_t addr = 0;
    uint32_t id = 0;
    uint32_t epoch = 0;
  };

  Imem(mosaic::MemoryModel* mem, int latency) : mem_(mem), latency_(latency) {}

  void Reset() {
    inflight_.clear();
    ready_.clear();
  }

  bool HasResponse() const { return !ready_.empty(); }
  const Request& Response() const { return ready_.front(); }
  uint64_t ResponseWord() { return Word(ready_.front().addr); }

  void Accept(uint64_t addr, uint32_t id, uint32_t epoch) {
    Entry entry;
    entry.req.addr = addr;
    entry.req.id = id;
    entry.req.epoch = epoch;
    entry.left = latency_;
    inflight_.push_back(entry);
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

  uint64_t Word(uint64_t addr) {
    uint64_t value = 0;
    if (mem_->Read(addr, 4, &value) != mosaic::AccessStatus::kOk) {
      ++faults_;
      return kEcallWord;  // a runaway fetch stops the machine cleanly
    }
    return value & 0xffffffffull;
  }

  uint64_t faults() const { return faults_; }

 private:
  struct Entry {
    Request req;
    int left = 0;
  };

  mosaic::MemoryModel* mem_;
  int latency_;
  std::deque<Entry> inflight_;
  std::deque<Request> ready_;
  uint64_t faults_ = 0;
};

// ============================================================================
// The driver
// ============================================================================
class Driver {
 public:
  Driver(Vmosaic_core_tb* dut, mosaic::ClockDriver* clk, mosaic::Reporter* reporter,
         const Geometry& g, mosaic::MemoryModel* mem, uint64_t window)
      : dut_(dut), clk_(clk), reporter_(reporter), g_(g), mem_(mem), imem_(mem, 1),
        window_(window) {
    ret_mask_ = (g.retire_width >= 32) ? 0xffffffffu : ((1u << g.retire_width) - 1u);
  }

  // ------------------------------------------------------------- geometry
  void GeometryChecks() {
    Check("this case proves the 64-bit two-cluster profile",
          g_.xlen == 64 && g_.clusters == 2,
          "XLEN=" + Dec(g_.xlen) + " clusters=" + Dec(g_.clusters));
    Check("the program is encoded at the profile's reset vector",
          g_.reset_vector == 0x80000000ull,
          "reset vector=" + U64(g_.reset_vector));
    Check("the physical tag and generation widths are the ones the stall report decodes",
          g_.prf_entries == 96 && g_.prf_tag_w == 7 && g_.int_gen_w == 7,
          "entries=" + Dec(g_.prf_entries) + " tag_w=" + Dec(g_.prf_tag_w) +
              " gen_w=" + Dec(g_.int_gen_w));
    Check("the program fits the ROB window", kProgWords <= static_cast<int>(g_.rob_entries),
          "program words=" + Dec(kProgWords) + " ROB entries=" + Dec(g_.rob_entries));
    Check("the observation window fits the case's cycle budget",
          window_ <= 2000 && g_.retire_width >= 1,
          "window=" + Dec(window_));
  }

  // ---------------------------------------------------------- program image
  void LoadProgram() {
    for (int i = 0; i < kProgWords; i++) {
      WriteWord(g_.reset_vector + 4ull * static_cast<uint64_t>(i), kProgram[i], "program");
    }
    for (int i = 0; i < kFillerWords; i++) {
      const uint64_t addr = g_.reset_vector + 4ull * static_cast<uint64_t>(kProgWords + i);
      WriteWord(addr, kEcallWord, "ECALL filler");
    }
  }

  // ---------------------------------------------------------------- running
  void Reset(int cycles) {
    imem_.Reset();
    for (int i = 0; i < cycles; i++) Cycle(true);
  }

  void Run() {
    while (true) {
      Cycle(false);
      if (dut_->o_commit_o >= kExpectedRetires && dut_->o_rob_occupied_o == 0 &&
          dut_->o_stopped_o != 0) {
        return;
      }
      if (cycles_ >= window_) {
        if (dut_->o_commit_o < kExpectedRetires) {
          Fail("no retire within " + Dec(window_) + " cycles", Diagnose());
        }
        Fail("the machine retired but did not quiesce within " + Dec(window_) + " cycles",
             Diagnose());
      }
    }
  }

  // ---------------------------------------------------------------- checks
  void FinalChecks(mosaic::Options* options) {
    Check("three instructions retire, and nothing else",
          retires_.size() == static_cast<size_t>(kExpectedRetires),
          "retired=" + Dec(retires_.size()) + " -- " + Diagnose());

    for (int i = 0; i < kExpectedRetires; i++) {
      const mosaic::RetireEvent& event = retires_.events()[static_cast<size_t>(i)];
      const uint64_t want_pc = g_.reset_vector + kExpected[i].pc_offset;
      Check("retire " + Dec(i) + " is the program's instruction " + Dec(i),
            event.pc == want_pc,
            "pc=" + U64(event.pc) + " expected " + U64(want_pc));
      Check("retire " + Dec(i) + " writes x" + Dec(kExpected[i].rd),
            event.has_rd && event.rd == kExpected[i].rd,
            "has_rd=" + Dec(event.has_rd) + " rd=x" + Dec(event.rd));
      Check("retire " + Dec(i) + " carries the ISA's value",
            event.rd_value == kExpected[i].value,
            "x" + Dec(kExpected[i].rd) + "=" + U64(event.rd_value) + " expected " +
                U64(kExpected[i].value));
      Check("retire " + Dec(i) + " carries sequence number " + Dec(i),
            event.seq == static_cast<uint64_t>(i),
            "seq=" + Dec(event.seq));
      Check("retire " + Dec(i) + " is neither a trap nor a store",
            !event.is_trap && !event.is_store,
            "trap=" + Dec(event.is_trap) + " store=" + Dec(event.is_store));
    }

    // The canonical stream against an expectation built from the ISA: this is
    // the whole event record, not the three fields checked above.
    mosaic::EventTap expected;
    for (int i = 0; i < kExpectedRetires; i++) {
      mosaic::RetireEvent event;
      event.hart = 0;
      event.seq = static_cast<uint64_t>(i);
      event.pc = g_.reset_vector + kExpected[i].pc_offset;
      event.next_pc = 0;  // the core does not publish a next PC
      event.insn = 0;     // the core does not publish the instruction bits
      event.has_rd = true;
      event.rd = static_cast<uint8_t>(kExpected[i].rd);
      event.rd_value = kExpected[i].value;
      expected.Record(event);
    }
    const std::string path = options->out_dir + "/expected_retire.events";
    std::string save_detail;
    if (!expected.Save(path, &save_detail)) {
      Fail("the expectation could not be written", save_detail);
    }
    std::string first_mismatch;
    std::string compare_detail;
    int mismatches = 0;
    const bool differs = retires_.Compare(path, &first_mismatch, &mismatches, &compare_detail);
    Check("the canonical retirement stream equals the ISA expectation",
          !differs,
          compare_detail + (first_mismatch.empty() ? "" : " -- " + first_mismatch));

    Check("the ECALL is refused, not executed",
          dut_->o_unsupported_o >= 1 && dut_->o_illegal_o == 0,
          "unsupported=" + Dec(dut_->o_unsupported_o) + " illegal=" + Dec(dut_->o_illegal_o));
    Check("the machine stops at the refused ECALL", dut_->o_stopped_o != 0,
          "stopped=" + Dec(dut_->o_stopped_o));
    Check("the ROB is empty and the machine is quiescent",
          dut_->o_rob_occupied_o == 0,
          "occupied=" + Dec(dut_->o_rob_occupied_o));
    Check("the empty ROB is at a rename boundary",
          dut_->o_rename_boundary_o != 0 && dut_->o_desc_live_o == 0,
          "boundary=" + Dec(dut_->o_rename_boundary_o) + " desc_live=" +
              Dec(dut_->o_desc_live_o));
    Check("no squash, no journal overflow and no squashed allocation",
          dut_->o_squash_nc_o == 0 && dut_->o_squash_under_o == 0 &&
              dut_->o_journal_ovf_o == 0,
          "squash_nc=" + Dec(dut_->o_squash_nc_o) + " under=" + Dec(dut_->o_squash_under_o) +
              " journal=" + Dec(dut_->o_journal_ovf_o));
    Check("every fetch and every retire was inside the memory map",
          imem_.faults() == 0 && mem_->access_fault_count() == 0,
          "imem faults=" + Dec(imem_.faults()) + " model faults=" +
              Dec(mem_->access_fault_count()));
    Check("the free set is back to its reset population: every initial mapping the "
          "program superseded was released",
          dut_->o_free_count_o == (g_.prf_entries - 32),
          "free=" + Dec(dut_->o_free_count_o) + " expected " +
              Dec(g_.prf_entries - 32));

    // The property the case is *about*, read off the DUT's rename state: the
    // register the first instruction read is still the architectural initial
    // mapping, so the machine really did retire through a never-written
    // register and never wrote it.
    Check("x31 is still the architectural initial mapping, never allocated and never written",
          MapTag(31) == 31 && MapGen(31) == 0 && GenValid(31) == 0 && WbDone(31) == 0,
          "x31 -> tag " + Dec(MapTag(31)) + " generation " + Dec(MapGen(31)) +
              " gen_valid=" + Dec(GenValid(31)) + " wb_done=" + Dec(WbDone(31)));
    for (uint32_t i = 0; i < 3; i++) {
      const uint32_t rd = kExpected[i].rd;
      Check("x" + Dec(rd) + "'s mapping came from an allocation, so the constant path "
            "did not shadow it",
            GenValid(MapTag(rd)) != 0,
            "x" + Dec(rd) + " -> tag " + Dec(MapTag(rd)) + " gen_valid=" +
                Dec(GenValid(MapTag(rd))));
    }
    Check("the producer that supplied x7's second operand was woken, not folded: the "
          "writeback path carried a completion and a wakeup",
          dut_->o_wb_wr_o >= 3 && dut_->o_wb_wake_o >= 1,
          "wb_wr=" + Dec(dut_->o_wb_wr_o) + " wb_wake=" + Dec(dut_->o_wb_wake_o));
  }

  std::string Summary() const {
    std::string text = "checks=" + Dec(reporter_->checks()) +
                       " cycles=" + Dec(cycles_) + " retires=" + Dec(retires_.size());
    for (int i = 0; i < kExpectedRetires && static_cast<size_t>(i) < retires_.size(); i++) {
      text += " (x" + Dec(kExpected[i].rd) + "=" +
              U64(retires_.events()[static_cast<size_t>(i)].rd_value) + " at cycle " +
              Dec(retire_cycles_[static_cast<size_t>(i)]) + ")";
    }
    text += " stopped=" + Dec(dut_->o_stopped_o) + " free=" + Dec(dut_->o_free_count_o);
    return text;
  }

 private:
  // ------------------------------------------------------------- one cycle
  void Cycle(bool rst) {
    dut_->rst = rst ? 1 : 0;
    dut_->imem_req_ready_i = rst ? 0 : 1;
    if (!rst && imem_.HasResponse()) {
      const Imem::Request& request = imem_.Response();
      dut_->imem_rsp_valid_i = 1;
      dut_->imem_rsp_rdata_i = static_cast<uint32_t>(imem_.ResponseWord());
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = request.id;
      dut_->imem_rsp_epoch_i = request.epoch;
      dut_->imem_rsp_len_i = 4;  // a 32-bit instruction, in bytes
    } else {
      dut_->imem_rsp_valid_i = 0;
      dut_->imem_rsp_rdata_i = 0;
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = 0;
      dut_->imem_rsp_epoch_i = 0;
      dut_->imem_rsp_len_i = 0;
    }
    // The data port and the standalone redirect arbiter are idle: this case uses
    // neither (the core ties dmem_req_valid low, and the wrapper's arbiter is a
    // directed-test fixture for CASE=fabric.fixed_two_cluster).
    dut_->dmem_req_ready_i = 1;
    dut_->dmem_rsp_valid_i = 0;
    dut_->dmem_rsp_rdata_i = 0;
    dut_->dmem_rsp_fault_i = 0;
    dut_->arb_req_valid0_i = 0;
    dut_->arb_req_valid1_i = 0;
    dut_->arb_req_pc0_i = 0;
    dut_->arb_req_pc1_i = 0;
    dut_->arb_req_idx0_i = 0;
    dut_->arb_req_idx1_i = 0;
    dut_->arb_req_gen0_i = 0;
    dut_->arb_req_gen1_i = 0;
    dut_->arb_req_taken0_i = 0;
    dut_->arb_req_taken1_i = 0;
    dut_->arb_head_valid_i = 0;
    dut_->arb_head_index_i = 0;
    dut_->arb_head_gen_i = 0;
    dut_->arb_head_occupied_i = 0;
    dut_->arb_head_retire_i = 0;

    dut_->eval();
    if (!rst) Observe();
    ModelStep();
    Edge();
    ++cycles_;
  }

  void Edge() {
    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
  }

  void ModelStep() {
    if (dut_->imem_req_valid_o && dut_->imem_req_ready_i) {
      imem_.Accept(dut_->imem_req_addr_o, static_cast<uint32_t>(dut_->imem_req_id_o),
                   static_cast<uint32_t>(dut_->imem_req_epoch_o));
    }
    if (dut_->imem_rsp_valid_i && dut_->imem_rsp_ready_o && imem_.HasResponse()) {
      imem_.PopResponse();
    }
    imem_.Advance();
  }

  void Observe() {
    // The core's own retire counter is an independent tally of the event stream
    // it publishes; check it before this cycle's events are read.
    Check("the retire counter equals the event stream published so far",
          dut_->o_commit_o == static_cast<uint32_t>(retires_.size()),
          "counter=" + Dec(dut_->o_commit_o) + " events=" + Dec(retires_.size()));
    Check("ROB occupancy is within the ROB",
          dut_->o_rob_occupied_o <= g_.rob_entries,
          "occupied=" + Dec(dut_->o_rob_occupied_o));

    const uint32_t mask = static_cast<uint32_t>(dut_->ev_valid_o) & ret_mask_;
    if (g_.retire_width >= 2) {
      Check("retire lane 1 is never set without lane 0",
            ((mask & 2u) == 0) || ((mask & 1u) != 0),
            "ev_valid=" + Dec(mask));
    }
    for (uint32_t lane = 0; lane < g_.retire_width && lane < 32; lane++) {
      if ((mask & (1u << lane)) == 0) continue;
      mosaic::RetireEvent event;
      event.hart = 0;
      event.seq = PackedLane(static_cast<uint64_t>(dut_->ev_seq_o), lane, g_.seq_w);
      event.pc = PayloadLane(dut_->ev_pc_o, lane);
      event.next_pc = 0;
      event.insn = 0;
      event.has_rd = PackedLane(static_cast<uint64_t>(dut_->ev_reg_we_o), lane, 1) != 0;
      event.rd = static_cast<uint8_t>(PackedLane(static_cast<uint64_t>(dut_->ev_rd_o),
                                                 lane, 5));
      event.rd_value = PayloadLane(dut_->ev_value_o, lane);
      event.is_trap = PackedLane(static_cast<uint64_t>(dut_->ev_trap_o), lane, 1) != 0;
      event.is_store = PackedLane(static_cast<uint64_t>(dut_->ev_store_o), lane, 1) != 0;
      retires_.Record(event);
      retire_cycles_.push_back(cycles_);
    }

    // Progress, for the stall report: the last cycle in which anything moved.
    const bool progressed = dut_->o_commit_o != last_commit_ ||
                            dut_->o_wb_pub_valid_o != 0 ||
                            dut_->o_dbg_alloc_ctr_o != last_alloc_;
    last_commit_ = dut_->o_commit_o;
    last_alloc_ = dut_->o_dbg_alloc_ctr_o;
    if (progressed) last_progress_ = cycles_;
  }

  // ------------------------------------------------------------ diagnosis
  std::string Diagnose() const {
    std::string text = "ROB head pc=" + U64(dut_->o_dbg_head_pc_o) +
                       " head_rd=x" + Dec(dut_->o_dbg_desc_rd0_o);
    const uint64_t head_pc = dut_->o_dbg_head_pc_o;
    int head_index = -1;
    if (head_pc >= g_.reset_vector && ((head_pc - g_.reset_vector) % 4ull) == 0 &&
        (head_pc - g_.reset_vector) / 4ull < static_cast<uint64_t>(kProgWords)) {
      head_index = static_cast<int>((head_pc - g_.reset_vector) / 4ull);
      const std::vector<uint32_t> sources = UniqueSources(kProgram[head_index]);
      text += " (program word " + Dec(head_index) + ", reads";
      for (uint32_t address : sources) text += " x" + Dec(address);
      text += ")";
    }
    text += " retired=" + Dec(dut_->o_commit_o) + " allocated=" + Dec(dut_->o_dbg_alloc_ctr_o) +
            " inserted=" + Dec(dut_->o_dbg_ins_ctr_o) + " occupied=" +
            Dec(dut_->o_rob_occupied_o) + " stopped=" + Dec(dut_->o_stopped_o) + " wb_wr=" +
            Dec(dut_->o_wb_wr_o) + " wb_wake=" + Dec(dut_->o_wb_wake_o) + " last_progress=cycle " +
            Dec(last_progress_);

    // The operands that stalled, named exactly: a source that resolves to the
    // architectural initial mapping has no producer, so no wakeup for its
    // (tag, generation) can ever be broadcast. The head macro's own sources are
    // reported first, and every one of them is classified, so a stall that is
    // *not* this defect says so.
    text += " | head macro's sources:";
    if (head_index >= 0) {
      bool first = true;
      for (uint32_t address : UniqueSources(kProgram[head_index])) {
        text += first ? " " : ", ";
        first = false;
        text += "x" + Dec(address) + "->(tag " + Dec(MapTag(address)) + ", generation " +
                Dec(MapGen(address)) + ")";
        if (address == 0) {
          text += " x0 by definition";
        } else if (MapTag(address) == address && MapGen(address) == 0 &&
                   GenValid(address) == 0 && WbDone(address) == 0) {
          text += " gen_valid=0 wb_done=0 -- the architectural initial mapping, no wakeup "
                  "for it can exist";
        } else {
          text += " gen_valid=" + Dec(GenValid(MapTag(address))) + " wb_done=" +
                  Dec(WbDone(MapTag(address)));
        }
      }
    } else {
      text += " (the head is not one of the program's instructions)";
    }

    // How many other architectural registers are still their initial mapping, as
    // a bounded summary rather than 31 entries of noise.
    uint32_t others = 0;
    for (uint32_t arch = 1; arch < 32; arch++) {
      if (MapTag(arch) == arch && MapGen(arch) == 0 && GenValid(arch) == 0 &&
          WbDone(arch) == 0) {
        others++;
      }
    }
    text += " | " + Dec(others) + " architectural register(s) are still the initial mapping";
    return text;
  }

  // -------------------------------------------------------- rename probes
  uint64_t MapEntry(uint32_t arch) const {
    const unsigned map_w = g_.prf_tag_w + g_.int_gen_w;
    return WideBits(dut_->o_dbg_spec_map_o, arch * map_w, map_w);
  }
  uint64_t MapTag(uint32_t arch) const {
    return MapEntry(arch) & ((1ull << g_.prf_tag_w) - 1ull);
  }
  uint64_t MapGen(uint32_t arch) const { return MapEntry(arch) >> g_.prf_tag_w; }
  uint64_t GenValid(uint64_t tag) const {
    return WideBits(dut_->o_dbg_gen_valid_o, static_cast<unsigned>(tag), 1);
  }
  uint64_t WbDone(uint64_t tag) const {
    return WideBits(dut_->o_dbg_wb_done_o, static_cast<unsigned>(tag), 1);
  }

  // ---------------------------------------------------------------- helpers
  void Check(const std::string& what, bool ok, const std::string& detail) {
    reporter_->Check(ok, what + (ok ? "" : " -- " + detail));
    if (!ok) Fail(what, detail);
  }

  void WriteWord(uint64_t addr, uint32_t word, const char* what) {
    const mosaic::AccessStatus status = mem_->Write(addr, 4, word);
    if (status != mosaic::AccessStatus::kOk) {
      Fail("the " + std::string(what) + " image could not be written",
           "address " + U64(addr) + ": " +
               mosaic::AccessStatusName(status));
    }
  }

  Vmosaic_core_tb* dut_;
  mosaic::ClockDriver* clk_;
  mosaic::Reporter* reporter_;
  Geometry g_;
  mosaic::MemoryModel* mem_;
  Imem imem_;
  uint64_t window_;

  uint32_t ret_mask_ = 0;
  uint64_t cycles_ = 0;
  mosaic::EventTap retires_;
  std::vector<uint64_t> retire_cycles_;
  uint64_t last_commit_ = 0;
  uint64_t last_alloc_ = 0;
  uint64_t last_progress_ = 0;
};

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
  mosaic::ClockDriver clk;
  mosaic::MemoryModel memory;
  Vmosaic_core_tb dut;

  std::string detail;
  bool passed = true;
  try {
    // The geometry ports are constants, so one evaluation reads them.
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();
    const Geometry geometry = ReadGeometry(&dut);

    Driver driver(&dut, &clk, &reporter, geometry, &memory, kStallWindow);
    driver.GeometryChecks();
    driver.LoadProgram();
    driver.Reset(kResetCycles);
    driver.Run();
    driver.FinalChecks(&options);
    detail = driver.Summary();
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the machine retires through a never-written register",
                      "the machine stalled or produced the wrong value");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
