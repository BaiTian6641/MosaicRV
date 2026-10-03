// ============================================================================
// tb_core_prototype.cpp -- CASE=p0.prototype_gate, work package I-080.
//
// The first real p0 functional prototype gate. The card (implementation-plan.md
// section I-080) freezes the p0 *no-cache* configuration, runs the p0 corpus
// through the two-cluster out-of-order core, and requires that the *dynamic
// dual-cluster path actually occurs* -- not that it could. Five trace
// conditions must each be observed at least once, with a location:
//
//   1. two clusters were alive simultaneously;
//   2. a remote route occurred;
//   3. a writeback collision occurred;
//   4. an out-of-order completion occurred;
//   5. retirement stayed in order within a hart.
//
// ------------------------------------------------------------- the p0 config
//
// Every program is run **twice** through the same elaborated core, one toggle
// apart, exactly as CASE=fabric.integrated does:
//
//   * `fab_dyn_i = 0`  the fixed alternating cluster affinity (the baseline);
//   * `fab_dyn_i = 1`  the I-029 steering policy -- the dynamic dual-cluster
//                      path this gate is about.
//
// Everything else is the frozen p0 configuration and is *asserted*, not
// assumed: `cache_en_i = 0` (the no-cache choice -- I-042/I-043 exist and are
// green, so this is a configuration choice, not a capability hole), the I-060
// locality switches off, the I-084 vector coalescing switch untouched, and no
// lane-quota request. The DUT's own PMU cache counters are required to be zero,
// so a build that silently enabled the cache cannot pass as p0.
//
// The architecture is checked against the host oracle
// (`tools/host_oracle.py --all`), the same expectation CASE=core.corpus_sweep
// uses, so a fault injected into the dynamic route is caught as a divergence
// rather than passing as a "the trace conditions still occurred" run.
//
// --------------------------------------------------------- the remote route
//
// The integrated p0 core has **no remote-link endpoint**: `mosaic_remote_link`
// (I-028) is delivered as a standalone module and its own case is
// `remote.kill_with_delayed_response`, but the core does not instantiate it.
// The card's "remote route" is therefore defined here as the route that *does*
// exist in this machine -- a macro the dynamic steering executed in the other
// cluster than the fixed affinity executed it in, i.e. a route that crosses the
// cluster boundary the fixed baseline would have kept. It is observed by
// comparing the two runs' per-program-position cluster assignment, both taken
// from the DUT's own grant ports. That mapping (rather than a remote-link
// transaction) is stated in the report, and the standalone link's evidence is
// named there as the related-but-separate fact.
//
// The check can fail: a mutant that disables the steering (`MOSAIC_FAB_MUTANT_NO_DELTA`)
// leaves every position on its fixed cluster, and a mutant that pins every
// macro to cluster 0 (`MOSAIC_DISPATCH_MUTANT_SINGLE_CLUSTER`) empties cluster 1
// -- both are registered as this case's negative controls.
// ============================================================================

#include <verilated.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Vmosaic_core_tb.h"

#include "elf_loader.h"
#include "event_tap.h"
#include "mem_ref.h"
#include "memory_model.h"
#include "mosaic_platform.h"
#include "sim_common.h"

using mosaic_ref::DataMem;

namespace {

constexpr int kResetCycles = 4;
constexpr int kSettleCycles = 8;
constexpr uint64_t kStallCycles = 50000;

// The four programs the gate runs, one input each. Between them they carry the
// dual-cluster ALU/branch path (p02), the shared MUL/DIV long-latency path that
// makes completion out of order (p11), and the ordered load/store path (p03).
struct ProgramChoice {
  const char* program;
  int input;
};

const ProgramChoice kPrograms[] = {
    {"p01_addsub", 0},
    {"p06_shiftlogic", 0},
    {"p11_bigmuldiv", 0},
    {"p12_memwalk", 0},
};

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string Dec(uint64_t value) { return std::to_string(value); }

// ============================================================================
// The loaded program image
// ============================================================================
class ProgImage {
 public:
  bool Load(const std::string& path, std::string* detail) {
    mosaic::Image image;
    const mosaic::LoadStatus status = mosaic::LoadElf(path, &image, detail);
    if (status != mosaic::LoadStatus::kOk) {
      *detail = std::string("ELF refused: ") + mosaic::LoadStatusName(status) + ": " +
                *detail;
      return false;
    }
    elf_ = image;
    entry_ = image.entry;
    for (const mosaic::Segment& seg : image.segments) {
      for (uint64_t off = 0; off + 4 <= seg.memsz; off += 4) {
        uint32_t word = 0;
        for (uint64_t b = 0; b < 4; b++) {
          if (off + b < seg.filesz) {
            word |= static_cast<uint32_t>(seg.data[off + b]) << (8 * b);
          }
        }
        words_[seg.vaddr + off] = word;
      }
      for (uint64_t i = 0; i < seg.filesz; ++i) {
        bytes_[seg.vaddr + i] = seg.data[i];
      }
    }
    return !words_.empty();
  }

  uint32_t Word(uint64_t addr) const {
    auto it = words_.find(addr);
    return (it == words_.end()) ? 0x00000073u : it->second;
  }

  uint64_t entry() const { return entry_; }
  const mosaic::Image& elf() const { return elf_; }

 private:
  std::map<uint64_t, uint32_t> words_;
  std::map<uint64_t, uint8_t> bytes_;
  mosaic::Image elf_;
  uint64_t entry_ = 0;
};

// ============================================================================
// The instruction memory (one outstanding request, fixed latency)
// ============================================================================
class Imem {
 public:
  struct Request {
    uint64_t addr = 0;
    uint32_t id = 0;
    uint32_t epoch = 0;
  };

  explicit Imem(const ProgImage* img) : img_(img) {}

  void Reset() {
    inflight_.clear();
    ready_.clear();
  }

  bool HasResponse() const { return !ready_.empty(); }
  const Request& Response() const { return ready_.front(); }
  uint64_t ResponseWord() const { return img_->Word(ready_.front().addr); }

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

// ============================================================================
// Geometry, read from the elaborated DUT
// ============================================================================
struct Geometry {
  uint32_t xlen = 0;
  uint32_t retire_width = 0;
  uint32_t rob_entries = 0;
  uint32_t rob_index_w = 0;
  uint32_t rob_gen_w = 0;
  uint32_t uop_index_w = 0;
  uint64_t reset_vector = 0;
  uint32_t seq_w = 0;
  uint32_t ret_id_w = 0;
};

Geometry ReadGeometry(Vmosaic_core_tb* dut) {
  Geometry g;
  g.xlen = dut->o_geom_xlen_o;
  g.retire_width = dut->o_geom_retire_width_o;
  g.rob_entries = dut->o_geom_rob_entries_o;
  g.rob_index_w = dut->o_geom_rob_index_w_o;
  g.rob_gen_w = dut->o_geom_rob_gen_w_o;
  g.uop_index_w = dut->o_geom_uop_index_w_o;
  g.reset_vector = dut->o_geom_reset_vector_o;
  g.seq_w = dut->o_geom_seq_w_o;
  g.ret_id_w = dut->o_geom_ret_id_w_o;
  return g;
}

template <typename Wide>
uint64_t PayloadLane(const Wide& wide, uint32_t lane) {
  return static_cast<uint64_t>(wide[lane * 2]) |
         (static_cast<uint64_t>(wide[lane * 2 + 1]) << 32);
}

uint64_t ScalarField(uint64_t value, uint32_t lane, uint32_t width) {
  if (width == 0) return 0;
  const uint64_t shift = static_cast<uint64_t>(lane) * width;
  const uint64_t mask = (width >= 64) ? ~UINT64_C(0) : ((UINT64_C(1) << width) - 1);
  return (value >> shift) & mask;
}

template <typename Wide>
uint32_t WideWord(const Wide& wide, uint32_t index) {
  return static_cast<uint32_t>(wide[index]);
}

std::string FindRepoRoot() {
  std::string dir = ".";
  for (int depth = 0; depth < 8; ++depth) {
    std::FILE* probe = std::fopen((dir + "/config/profiles/p0.json").c_str(), "r");
    if (probe != nullptr) {
      std::fclose(probe);
      return dir;
    }
    dir += "/..";
  }
  return std::string();
}

// ============================================================================
// Source-register decode (RV64I + M + Zicsr), enough to find operand edges
// ============================================================================
// The gate does not need an architectural reference -- the host oracle is that
// -- it needs only the *operand edges* of a program, so that a value produced
// in one cluster and consumed in the other can be named. rs1 is bits [19:15]
// and rs2 is bits [24:20]; whether each is a register or an immediate field
// depends on the opcode, and that table is the whole of this function.
struct Operands {
  int rs1 = -1;
  int rs2 = -1;
};

Operands DecodeSources(uint32_t insn) {
  Operands out;
  const uint32_t opcode = insn & 0x7Fu;
  const int rs1 = static_cast<int>((insn >> 15) & 0x1Fu);
  const int rs2 = static_cast<int>((insn >> 20) & 0x1Fu);
  switch (opcode) {
    case 0x33:  // OP:         rd, rs1, rs2
    case 0x3B:  // OP-32
    case 0x63:  // BRANCH
    case 0x23:  // STORE
      out.rs1 = rs1;
      out.rs2 = rs2;
      break;
    case 0x13:  // OP-IMM
    case 0x1B:  // OP-IMM-32
    case 0x03:  // LOAD
    case 0x67:  // JALR
    case 0x73:  // SYSTEM (CSR*; ECALL/EBREAK have rs1 = x0)
    case 0x0F:  // MISC-MEM (FENCE; rs1 is a field, x0)
      out.rs1 = rs1;
      break;
    default:
      break;  // LUI, AUIPC, JAL, and anything else: no register source
  }
  return out;
}

// ============================================================================
// The host oracle, run as a subprocess (the same expectation CASE=core.corpus_sweep uses)
// ============================================================================
struct OracleRow {
  std::string program;
  int input = 0;
  uint64_t c = 0;
  uint64_t sig[4] = {0, 0, 0, 0};
};

bool IsProgramRow(const std::string& token) {
  if (token.size() < 5) return false;
  if (token[0] != 'p') return false;
  if (!std::isdigit(static_cast<unsigned char>(token[1]))) return false;
  if (!std::isdigit(static_cast<unsigned char>(token[2]))) return false;
  return token[3] == '_';
}

bool RunOracle(const std::string& repo, std::vector<OracleRow>* rows,
               std::string* detail) {
  const std::string command = "python3 " + repo + "/tools/host_oracle.py --all 2>&1";
  std::FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    *detail = "cannot start `" + command + "`";
    return false;
  }
  std::string output;
  char line[4096];
  while (std::fgets(line, sizeof(line), pipe) != nullptr) output += line;
  const int status = pclose(pipe);
  if (status != 0) {
    *detail = "`tools/host_oracle.py --all` exited " + Dec(static_cast<uint64_t>(status)) +
              "; the oracle disagrees with the corpus declaration, the golden file or "
              "its own input-sensitivity check:\n" + output;
    return false;
  }
  std::istringstream text(output);
  std::string row_text;
  while (std::getline(text, row_text)) {
    std::istringstream row(row_text);
    std::string name;
    if (!(row >> name)) continue;
    if (!IsProgramRow(name)) continue;
    std::string index, a, b, c, s0, s1, s2, s3, traps;
    if (!(row >> index >> a >> b >> c >> s0 >> s1 >> s2 >> s3 >> traps)) continue;
    OracleRow entry;
    entry.program = name;
    entry.input = std::atoi(index.c_str());
    entry.c = std::strtoull(c.c_str(), nullptr, 16);
    entry.sig[0] = std::strtoull(s0.c_str(), nullptr, 16);
    entry.sig[1] = std::strtoull(s1.c_str(), nullptr, 16);
    entry.sig[2] = std::strtoull(s2.c_str(), nullptr, 16);
    entry.sig[3] = std::strtoull(s3.c_str(), nullptr, 16);
    rows->push_back(entry);
  }
  if (rows->empty()) {
    *detail = "the oracle printed no rows:\n" + output;
    return false;
  }
  return true;
}

// ============================================================================
// One retired instruction
// ============================================================================
struct RetireRec {
  uint64_t seq = 0;
  uint64_t pc = 0;
  uint64_t insn = 0;
  uint64_t len = 0;
  uint64_t id = 0;
  uint64_t reg_we = 0;
  uint64_t rd = 0;
  uint64_t value = 0;
  uint64_t store = 0;
  uint64_t store_addr = 0;
  uint64_t store_data = 0;
  uint64_t store_size = 0;
  uint64_t csr_we = 0;
  uint64_t csr_addr = 0;
  uint64_t csr_value = 0;
  uint64_t trap = 0;
  uint64_t trap_cause = 0;
  uint64_t trap_tval = 0;
  int cluster = -1;   // the cluster the DUT granted this instruction's uop to
  uint64_t grant_cycle = 0;  // the cycle that uop was granted (trace location)

  bool operator==(const RetireRec& o) const {
    return seq == o.seq && pc == o.pc && insn == o.insn && len == o.len &&
           reg_we == o.reg_we && rd == o.rd && value == o.value && store == o.store &&
           store_addr == o.store_addr && store_data == o.store_data &&
           store_size == o.store_size && csr_we == o.csr_we && csr_addr == o.csr_addr &&
           csr_value == o.csr_value && trap == o.trap && trap_cause == o.trap_cause &&
           trap_tval == o.trap_tval;
  }
};

// ============================================================================
// Trace conditions
// ============================================================================
struct Observation {
  bool seen = false;
  uint64_t cycle = 0;
  uint64_t event = 0;
  std::string detail;
};

struct TraceConditions {
  Observation both_clusters_alive;   // both clusters hold un-retired in-flight work
  Observation both_clusters_iq;      // both issue queues hold live uops at once
  Observation remote_route;
  Observation wb_collision;
  Observation ooo_completion;
  bool in_order = false;
  uint64_t retire_events = 0;
  std::string in_order_detail;
};

// ============================================================================
// One run's record
// ============================================================================
struct RunRecord {
  std::string program;
  int input = 0;
  bool fab_dyn = false;
  bool finished = false;
  bool passed = false;
  uint64_t cycles = 0;
  uint64_t retires_port = 0;
  uint64_t redirects = 0;
  uint64_t traps = 0;
  uint64_t unsupported = 0;
  uint64_t illegal = 0;
  uint64_t rob_occupied_end = 0;
  uint64_t desc_live_end = 0;
  uint64_t invariant_violations = 0;
  std::string invariant_first;
  bool signature_readable = false;
  uint64_t signature[4] = {0, 0, 0, 0};

  uint32_t c0_alu = 0, c1_alu = 0, c0_br = 0, c1_br = 0, muldiv = 0;
  uint32_t unit_issues[4] = {0, 0, 0, 0};
  uint32_t wb_collision_ctr = 0;

  // Cache counters -- required to be zero for the frozen p0 no-cache build.
  uint32_t pmu_dcache_txn = 0, pmu_dcache_hit = 0, pmu_dcache_miss = 0;
  uint32_t pmu_dcache_refill = 0, pmu_dcache_wb = 0;

  std::vector<RetireRec> retires;      // in program order
  std::vector<int> cluster_by_pos;     // cluster of the uop at each program position
  TraceConditions trace;

};

// ============================================================================
// The runner
// ============================================================================
class Runner {
 public:
  Runner(Vmosaic_core_tb* dut, const Geometry& g) : dut_(dut), g_(g) {}

  RunRecord Run(const ProgImage& image, const std::string& program, int input,
                bool fab_dyn, uint64_t max_cycles, mosaic::EventTap* tap) {
    RunRecord record;
    record.program = program;
    record.input = input;
    record.fab_dyn = fab_dyn;

    dut_mem_ = mosaic::MemoryModel();
    {
      std::string detail;
      if (!dut_mem_.LoadImage(image.elf(), &detail)) Fail("setup", detail);
    }
    // crt0 reads FROMHOST and overwrites input word 0 with it when non-zero.
    // The corpus programs are built with input 0, so FROMHOST stays zero; the
    // input word the memory model was loaded with is the declared one.
    (void)input;

    Imem imem(&image);
    DataMem dmem(&dut_mem_);
    imem.Reset();
    dmem.Reset();

    cycles_ = 0;
    violations_ = 0;
    seq_width_ = g_.seq_w;
    ret_id_width_ = g_.ret_id_w;
    invariant_first_.clear();
    last_commit_ = 0;
    last_alloc_ = 0;
    last_progress_ = 0;
    retires_.clear();
    fab_dyn_ = fab_dyn;
    cluster_by_index_.assign(static_cast<size_t>(g_.rob_entries), -1);
    grant_cycle_.assign(static_cast<size_t>(g_.rob_entries), 0);
    inflight_[0] = 0;
    inflight_[1] = 0;
    trace_ = TraceConditions{};
    tap_ = tap;

    dut_->clk = 0;
    dut_->rst = 1;
    dut_->fab_dyn_i = fab_dyn ? 1 : 0;
    dut_->eval();
    for (int i = 0; i < kResetCycles; i++) Cycle(true, &imem, &dmem);

    enum class RunStatus { kPass, kStopped, kStalled, kTimeout };
    RunStatus status = RunStatus::kTimeout;
    while (true) {
      Cycle(false, &imem, &dmem);
      if (dut_mem_.finished()) {
        status = RunStatus::kPass;
        break;
      }
      if (dut_->o_stopped_o != 0) {
        status = RunStatus::kStopped;
        break;
      }
      if (cycles_ >= max_cycles) break;
      if (cycles_ - last_progress_ > kStallCycles) {
        status = RunStatus::kStalled;
        break;
      }
    }
    for (int i = 0; i < kSettleCycles; i++) Cycle(false, &imem, &dmem);

    record.finished = (status == RunStatus::kPass);
    record.passed = dut_mem_.passed();
    record.cycles = cycles_;
    record.retires_port = dut_->o_commit_o;
    record.redirects = dut_->o_redirect_o;
    record.traps = dut_->o_csr_trap_o;
    record.unsupported = dut_->o_unsupported_o;
    record.illegal = dut_->o_illegal_o;
    record.rob_occupied_end = dut_->o_rob_occupied_o;
    record.desc_live_end = dut_->o_desc_live_o;
    record.invariant_violations = violations_;
    record.invariant_first = invariant_first_;
    record.retires = retires_;
    record.trace = trace_;
    record.wb_collision_ctr = dut_->o_wb_collision_o;
    record.pmu_dcache_txn = dut_->o_pmu_dcache_txn_ctr_o;
    record.pmu_dcache_hit = dut_->o_pmu_dcache_hit_ctr_o;
    record.pmu_dcache_miss = dut_->o_pmu_dcache_miss_ctr_o;
    record.pmu_dcache_refill = dut_->o_pmu_dcache_refill_ctr_o;
    record.pmu_dcache_wb = dut_->o_pmu_dcache_wb_ctr_o;
    record.c0_alu = dut_->o_c0_alu_o;
    record.c1_alu = dut_->o_c1_alu_o;
    record.c0_br = dut_->o_c0_br_o;
    record.c1_br = dut_->o_c1_br_o;
    record.muldiv = dut_->o_muldiv_o;
    for (uint32_t u = 0; u < 4; ++u) record.unit_issues[u] = WideWord(dut_->o_fab_unit_issues_o, u);

    for (const RetireRec& r : retires_) record.cluster_by_pos.push_back(r.cluster);

    {
      std::vector<uint64_t> words;
      if (dut_mem_.ReadSignature(&words) && words.size() == 4) {
        record.signature_readable = true;
        for (int k = 0; k < 4; k++) record.signature[k] = words[k];
      }
    }

    if (!record.finished) {
      std::string why = "the run did not reach the program's exit protocol";
      if (status == RunStatus::kStopped) {
        why = "the machine stopped on an instruction it refuses (unsupported=" +
              Dec(record.unsupported) + ", illegal=" + Dec(record.illegal) +
              ", pc=" + U64(dut_->o_dbg_deliver_pc_o) + ")";
      } else if (status == RunStatus::kStalled) {
        why = "no commit and no allocation for " + Dec(kStallCycles) + " cycles";
      } else {
        why = "the cycle bound was reached";
      }
      Fail(fab_dyn ? "dynamic run" : "fixed run", why + " (" + program + ")");
    }
    return record;
  }

 private:
  void Note(const std::string& text) {
    violations_++;
    if (invariant_first_.empty()) {
      invariant_first_ = "cycle " + Dec(cycles_) + ": " + text;
    }
  }

  void Observe() {
    const uint32_t mask = (g_.retire_width >= 32)
                              ? 0xFFFFFFFFu
                              : ((1u << g_.retire_width) - 1u);
    const uint32_t valid = static_cast<uint32_t>(dut_->ev_valid_o) & mask;

    // ------------------------------------------------------- trace condition 1
    // Both clusters held live work at the same instant. Two observations, both
    // reported: the strong one is that each cluster has at least one granted,
    // un-retired uop in flight (the "alive" the card means); the secondary one
    // is the two issue queues' own occupancy counts being non-zero in one cycle.
    if (!trace_.both_clusters_alive.seen && inflight_[0] > 0 && inflight_[1] > 0) {
      trace_.both_clusters_alive.seen = true;
      trace_.both_clusters_alive.cycle = cycles_;
      trace_.both_clusters_alive.event = retires_.size();
      trace_.both_clusters_alive.detail = "in flight c0=" + Dec(inflight_[0]) +
                                          " c1=" + Dec(inflight_[1]);
    }
    if (!trace_.both_clusters_iq.seen && dut_->o_c0_count_o > 0 &&
        dut_->o_c1_count_o > 0) {
      trace_.both_clusters_iq.seen = true;
      trace_.both_clusters_iq.cycle = cycles_;
      trace_.both_clusters_iq.event = retires_.size();
      trace_.both_clusters_iq.detail =
          "c0_occ=" + Dec(dut_->o_c0_count_o) + " c1_occ=" + Dec(dut_->o_c1_count_o);
    }

    // ------------------------------------------------------- trace condition 3
    // A writeback collision: the DUT's own arbiter counter advanced.
    if (!trace_.wb_collision.seen && dut_->o_wb_collision_o > 0) {
      trace_.wb_collision.seen = true;
      trace_.wb_collision.cycle = cycles_;
      trace_.wb_collision.event = retires_.size();
      trace_.wb_collision.detail = "arbiter collisions=" + Dec(dut_->o_wb_collision_o);
    }

    // ------------------------------------------------------- trace condition 4
    // An out-of-order completion: a completion is published for a ROB entry that
    // is not the head, while the head is occupied and not yet complete.
    if (!trace_.ooo_completion.seen && dut_->o_wb_pub_valid_o != 0 &&
        dut_->o_dbg_head_valid_o != 0 && dut_->o_dbg_head_complete_o == 0 &&
        dut_->o_wb_pub_index_o != dut_->o_dbg_head_index_o) {
      trace_.ooo_completion.seen = true;
      trace_.ooo_completion.cycle = cycles_;
      trace_.ooo_completion.event = retires_.size();
      trace_.ooo_completion.detail =
          "completed rob_index=" + Dec(dut_->o_wb_pub_index_o) +
          " while head rob_index=" + Dec(dut_->o_dbg_head_index_o) +
          " is occupied and incomplete";
    }

    // ------------------------------------------------------------ grant trace
    // The cluster each uop is granted to, keyed by rob index. The ROB frees an
    // index only when its previous occupant retires, so an index-keyed map
    // cleared at retire is unambiguous.
    if (dut_->o_c0_grant_valid_o != 0) {
      const uint64_t index = GrantIndex(dut_->o_c0_grant_uop_o);
      if (index < g_.rob_entries) {
        cluster_by_index_[index] = 0;
        grant_cycle_[index] = cycles_;
      }
      inflight_[0]++;
    }
    if (dut_->o_c1_grant_valid_o != 0) {
      const uint64_t index = GrantIndex(dut_->o_c1_grant_uop_o);
      if (index < g_.rob_entries) {
        cluster_by_index_[index] = 1;
        grant_cycle_[index] = cycles_;
      }
      inflight_[1]++;
    }

    // --------------------------------------------------------- retire stream
    for (uint32_t lane = 0; lane < g_.retire_width && lane < 32; lane++) {
      if ((valid & (1u << lane)) == 0) continue;
      RetireRec r;
      r.seq = ScalarField(dut_->ev_seq_o, lane, seq_width_);
      r.pc = PayloadLane(dut_->ev_pc_o, lane);
      r.insn = ScalarField(dut_->ev_insn_o, lane, 32);
      r.len = ScalarField(dut_->ev_len_o, lane, 3);
      r.id = ScalarField(dut_->ev_id_o, lane, ret_id_width_);
      r.reg_we = ScalarField(dut_->ev_reg_we_o, lane, 1);
      r.rd = ScalarField(dut_->ev_rd_o, lane, 5);
      r.value = PayloadLane(dut_->ev_value_o, lane);
      r.store = ScalarField(dut_->ev_store_o, lane, 1);
      r.store_addr = PayloadLane(dut_->ev_store_addr_o, lane);
      r.store_data = PayloadLane(dut_->ev_store_data_o, lane);
      r.store_size = ScalarField(dut_->ev_store_size_o, lane, 3);
      r.csr_we = ScalarField(dut_->ev_csr_we_o, lane, 1);
      r.csr_addr = ScalarField(dut_->ev_csr_addr_o, lane, 12);
      r.csr_value = PayloadLane(dut_->ev_csr_value_o, lane);
      r.trap = ScalarField(dut_->ev_trap_o, lane, 1);
      r.trap_cause = PayloadLane(dut_->ev_trap_cause_o, lane);
      r.trap_tval = PayloadLane(dut_->ev_trap_tval_o, lane);
      // The lane's ROB index: lane 0 is the head, lane 1 the entry behind it.
      if (dut_->o_dbg_head_valid_o != 0) {
        const uint64_t index = (static_cast<uint64_t>(dut_->o_dbg_head_index_o) + lane) %
                               g_.rob_entries;
        r.cluster = cluster_by_index_[index];
        r.grant_cycle = grant_cycle_[index];
        cluster_by_index_[index] = -1;
      }
      if (r.cluster >= 0) inflight_[r.cluster]--;
      retires_.push_back(r);
      if (tap_ != nullptr) {
        mosaic::RetireEvent ev;
        ev.hart = 0;
        ev.seq = r.seq;
        ev.pc = r.pc;
        ev.insn = static_cast<uint32_t>(r.insn);
        ev.has_rd = (r.reg_we != 0);
        ev.rd = static_cast<uint8_t>(r.rd);
        ev.rd_value = r.value;
        ev.is_trap = (r.trap != 0);
        ev.cause = r.trap_cause;
        ev.tval = r.trap_tval;
        ev.is_store = (r.store != 0);
        ev.store_address = r.store_addr;
        ev.store_data = r.store_data;
        ev.store_size = static_cast<unsigned>(r.store_size);
        tap_->Record(ev);
      }
    }

    // ------------------------------------------------------- in-order retire
    // The event port's own sequence is per-hart monotonic and the retire unit
    // publishes in program order; a reordered retirement would break it.
    if (!retires_.empty()) {
      const uint64_t n = retires_.size();
      const uint64_t seq_mask = (UINT64_C(1) << g_.seq_w) - 1;
      if (n >= 2 && retires_[n - 1].seq != ((retires_[n - 2].seq + 1) & seq_mask)) {
        Note("retire sequence did not advance by one at event " + Dec(n - 1) +
             " (seq " + Dec(retires_[n - 2].seq) + " -> " +
             Dec(retires_[n - 1].seq) + ")");
      }
    }

    // --------------------------------------------------- standing invariants
    if (dut_->o_squash_nc_o != 0 || dut_->o_squash_under_o != 0 ||
        dut_->o_journal_ovf_o != 0) {
      Note("recovery counters: squash_nc=" + Dec(dut_->o_squash_nc_o) + " under=" +
           Dec(dut_->o_squash_under_o) + " journal=" + Dec(dut_->o_journal_ovf_o));
    }
    if (dut_->o_rob_occupied_o > g_.rob_entries) {
      Note("ROB occupancy " + Dec(dut_->o_rob_occupied_o) + " exceeds " +
           Dec(g_.rob_entries));
    }
    if (dut_->o_mem_lq_query_mismatch_o != 0) {
      Note("the load queue's forwarding-query cross-check tripped: " +
           Dec(dut_->o_mem_lq_query_mismatch_o));
    }
    if (dut_->o_mem_sq_auth_o > dut_->o_mem_sq_occupied_o) {
      Note("store-queue authorised prefix " + Dec(dut_->o_mem_sq_auth_o) +
           " exceeds occupancy " + Dec(dut_->o_mem_sq_occupied_o));
    }

    const bool progressed = (dut_->o_commit_o != last_commit_) ||
                            (dut_->o_wb_pub_valid_o != 0) ||
                            (dut_->o_dbg_alloc_ctr_o != last_alloc_);
    last_commit_ = dut_->o_commit_o;
    last_alloc_ = dut_->o_dbg_alloc_ctr_o;
    if (progressed) last_progress_ = cycles_;
  }

  uint64_t GrantIndex(uint64_t grant_uop) const {
    const uint32_t shift = g_.rob_gen_w + g_.uop_index_w;
    return (grant_uop >> shift) & ((UINT64_C(1) << g_.rob_index_w) - 1);
  }

  void Cycle(bool rst, Imem* imem, DataMem* dmem) {
    dut_->rst = rst ? 1 : 0;
    dut_->fab_dyn_i = fab_dyn_ ? 1 : 0;
    // The frozen p0 configuration, driven explicitly so the case cannot be
    // fooled by a default: no cache, no locality, no vector coalescing change,
    // no lane-quota request.
    dut_->cache_en_i = 0;
    dut_->loc_llb_en_i = 0;
    dut_->loc_pf_en_i = 0;
    dut_->loc_pf_conf_thresh_i = 0;
    dut_->loc_snoop_all_i = 0;
    dut_->vec_coalesce_dis_i = 0;
    dut_->lane_quota_req_i = 0;
    dut_->lane_quota_val_i = 0;

    dut_->imem_req_ready_i = 1;
    if (imem->HasResponse()) {
      const Imem::Request& r = imem->Response();
      dut_->imem_rsp_valid_i = 1;
      dut_->imem_rsp_rdata_i = static_cast<uint32_t>(imem->ResponseWord());
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = r.id;
      dut_->imem_rsp_epoch_i = r.epoch;
      dut_->imem_rsp_len_i = 4;
    } else {
      dut_->imem_rsp_valid_i = 0;
      dut_->imem_rsp_rdata_i = 0;
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = 0;
      dut_->imem_rsp_epoch_i = 0;
      dut_->imem_rsp_len_i = 0;
    }

    dut_->dmem_req_ready_i = 1;
    if (dmem->HasResponse()) {
      const DataMem::Rsp& r = dmem->CurrentResponse();
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
    dut_->irq_soft_i = 0;
    dut_->irq_timer_i = 0;
    dut_->irq_ext_i = 0;
    dut_->mtime_i = 0;
    dut_->ext_write_valid_i = 0;
    dut_->ext_write_addr_i = 0;
    dut_->ext_write_bytes_i = 0;

    dut_->eval();

    if (!rst) Observe();

    if (bus_reset_.MayAccept(rst, (dut_->imem_req_valid_o != 0) &&
                                      (dut_->imem_req_ready_i != 0))) {
      imem->Accept(dut_->imem_req_addr_o, dut_->imem_req_id_o, dut_->imem_req_epoch_o);
    }
    if (bus_reset_.MayDeliver(rst) && (dut_->imem_rsp_valid_i != 0) &&
        (dut_->imem_rsp_ready_o != 0)) {
      imem->PopResponse();
    }
    imem->Advance();

    if (bus_reset_.MayAccept(rst, (dut_->dmem_req_valid_o != 0) &&
                                      (dut_->dmem_req_ready_i != 0))) {
      DataMem::Request r;
      r.we = dut_->dmem_req_we_o != 0;
      r.addr = dut_->dmem_req_addr_o;
      r.size = dut_->dmem_req_size_o;
      r.wstrb = dut_->dmem_req_wstrb_o;
      r.wdata = dut_->dmem_req_wdata_o;
      dmem->Accept(r, cycles_);
    }
    if (bus_reset_.MayDeliver(rst) && (dut_->dmem_rsp_valid_i != 0) &&
        (dut_->dmem_rsp_ready_o != 0)) {
      dmem->PopResponse();
    }
    dmem->Advance();

    dut_->clk = 0;
    dut_->eval();
    dut_->clk = 1;
    dut_->eval();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;
  }

  Vmosaic_core_tb* dut_;
  mosaic::BusResetGate bus_reset_;
  Geometry g_;
  mosaic::MemoryModel dut_mem_;
  mosaic::EventTap* tap_ = nullptr;
  uint32_t seq_width_ = 0;
  uint32_t ret_id_width_ = 0;
  uint64_t cycles_ = 0;
  uint64_t violations_ = 0;
  uint64_t last_commit_ = 0;
  uint64_t last_alloc_ = 0;
  uint64_t last_progress_ = 0;
  bool fab_dyn_ = false;
  std::string invariant_first_;
  std::vector<RetireRec> retires_;
  std::vector<int> cluster_by_index_;
  std::vector<uint64_t> grant_cycle_;
  int inflight_[2] = {0, 0};
  TraceConditions trace_;
};

// ============================================================================
// Operand-edge analysis: a value produced in one cluster and consumed in the
// other is the cross-cluster route the machine's wakeup/PRF path carries.
// ============================================================================
struct RouteStats {
  uint64_t cross_cluster_edges = 0;
  std::string first;
};

RouteStats CrossClusterRoutes(const RunRecord& run) {
  RouteStats stats;
  std::vector<int> last_writer(32, -1);
  for (size_t j = 0; j < run.retires.size(); ++j) {
    const RetireRec& r = run.retires[j];
    if (r.len != 4) continue;  // the p0 corpus is not compressed
    const Operands ops = DecodeSources(static_cast<uint32_t>(r.insn));
    for (int src : {ops.rs1, ops.rs2}) {
      if (src <= 0) continue;
      const int producer = last_writer[src];
      if (producer < 0) continue;
      const int pc_cluster = run.cluster_by_pos[producer];
      const int cc_cluster = run.cluster_by_pos[j];
      if (pc_cluster >= 0 && cc_cluster >= 0 && pc_cluster != cc_cluster) {
        stats.cross_cluster_edges++;
        if (stats.first.empty()) {
          stats.first = "producer position " + Dec(producer) + " (cluster " +
                        Dec(pc_cluster) + ") -> consumer position " + Dec(j) +
                        " (cluster " + Dec(cc_cluster) + "), register x" +
                        Dec(src);
        }
      }
    }
    if (r.reg_we != 0 && r.rd != 0) last_writer[r.rd] = static_cast<int>(j);
  }
  return stats;
}

std::string SigLine(const uint64_t sig[4]) {
  return U64(sig[0]) + " " + U64(sig[1]) + " " + U64(sig[2]) + " " + U64(sig[3]);
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
    const std::string repo = FindRepoRoot();
    if (repo.empty()) {
      Fail("setup", "cannot find the repository root: no config/profiles/p0.json "
                    "above the working directory");
    }

    std::vector<OracleRow> oracle;
    {
      std::string oracle_detail;
      if (!RunOracle(repo, &oracle, &oracle_detail)) Fail("setup", oracle_detail);
    }
    auto oracle_lookup = [&](const std::string& program, int input) -> const OracleRow* {
      for (const OracleRow& row : oracle) {
        if (row.program == program && row.input == input) return &row;
      }
      return nullptr;
    };

    // The wrapper's geometry read-back settles on `eval`.
    dut.clk = 0;
    dut.rst = 1;
    dut.fab_dyn_i = 0;
    dut.eval();
    const Geometry geom = ReadGeometry(&dut);
    if (geom.retire_width == 0 || geom.rob_entries == 0) {
      Fail("setup", "the wrapper's geometry read-back is zero: the generated "
                    "config package did not elaborate");
    }
    std::printf("p0.prototype_gate: retire_width=%u rob=%u index_w=%u gen_w=%u "
                "uop_w=%u programs=%zu\n",
                geom.retire_width, geom.rob_entries, geom.rob_index_w,
                geom.rob_gen_w, geom.uop_index_w,
                sizeof(kPrograms) / sizeof(kPrograms[0]));

    Runner runner(&dut, geom);
    const uint64_t max_cycles = options.max_cycles;

    // The gate aggregates the five trace conditions across every program and
    // every configuration; the dynamic configuration is the one the conditions
    // are asserted on.
    TraceConditions gate;
    std::string remote_route_program;
    uint64_t remote_route_pos = 0;

    for (const ProgramChoice& choice : kPrograms) {
      const std::string prog = choice.program;
      const std::string elf_path = repo + "/tests/programs/build/" + prog +
                                   ".i" + Dec(static_cast<uint64_t>(choice.input)) + ".elf";
      ProgImage image;
      std::string load_detail;
      if (!image.Load(elf_path, &load_detail)) {
        Fail("setup", load_detail + " (build the corpus with `make -C tests/programs all`)");
      }
      if (image.entry() != geom.reset_vector) {
        Fail("setup", "the image's entry " + U64(image.entry()) +
                          " is not the profile's reset vector " + U64(geom.reset_vector));
      }

      mosaic::EventTap tap;
      RunRecord fixed = runner.Run(image, prog, choice.input, false,
                                   max_cycles, nullptr);
      RunRecord dyn = runner.Run(image, prog, choice.input, true,
                                 max_cycles, &tap);
      // Save the dynamic retire event stream immediately, before any check can
      // abort the run: the injected-fault replay compares these files, and an
      // injected build that fails a check must still leave its trace on disk.
      if (!options.out_dir.empty()) {
        std::string save_detail;
        const std::string path = options.out_dir + "/events." + prog + ".txt";
        if (!tap.Save(path, &save_detail)) {
          Fail("setup", "cannot save the event stream: " + save_detail);
        }
      }

      std::printf("  %-14s in%d fixed(cycles=%llu c0/c1 alu=%u/%u br=%u/%u md=%u) "
                  "dyn(cycles=%llu alu=%u/%u br=%u/%u md=%u grants=%u/%u/%u/%u "
                  "wbcoll=%u)\n",
                  prog.c_str(), choice.input,
                  static_cast<unsigned long long>(fixed.cycles), fixed.c0_alu,
                  fixed.c1_alu, fixed.c0_br, fixed.c1_br, fixed.muldiv,
                  static_cast<unsigned long long>(dyn.cycles), dyn.c0_alu, dyn.c1_alu,
                  dyn.c0_br, dyn.c1_br, dyn.muldiv, dyn.unit_issues[0],
                  dyn.unit_issues[1], dyn.unit_issues[2], dyn.unit_issues[3],
                  dyn.wb_collision_ctr);

      // ---- the architecture: fixed == dynamic == the oracle -----------------
      const OracleRow* row = oracle_lookup(prog, choice.input);
      bool sig_ok = (row != nullptr) && dyn.signature_readable;
      if (sig_ok) {
        for (int k = 0; k < 4; k++) {
          if (dyn.signature[k] != row->sig[k]) sig_ok = false;
        }
      }
      reporter.Check(sig_ok,
                     "the dynamic p0 configuration retires the oracle's architecture (" +
                         prog + ".i" + Dec(static_cast<uint64_t>(choice.input)) +
                         "): signature " +
                         (row != nullptr ? SigLine(row->sig) : std::string("<none>")) +
                         " got " + SigLine(dyn.signature));

      bool same_stream = (fixed.retires.size() == dyn.retires.size());
      if (same_stream) {
        for (size_t i = 0; i < fixed.retires.size(); ++i) {
          if (!(fixed.retires[i] == dyn.retires[i])) {
            same_stream = false;
            std::printf("    ARCHITECTURE DIVERGES at retire %zu: fixed %s pc=%s / "
                        "dynamic %s pc=%s\n",
                        i, Dec(fixed.retires[i].value).c_str(),
                        U64(fixed.retires[i].pc).c_str(),
                        Dec(dyn.retires[i].value).c_str(),
                        U64(dyn.retires[i].pc).c_str());
            break;
          }
        }
      }
      reporter.Check(same_stream,
                     "the fixed and dynamic p0 configurations retire the same "
                     "architecture (" + prog + ".i" +
                         Dec(static_cast<uint64_t>(choice.input)) + ", " +
                         Dec(dyn.retires.size()) + " events)");

      // ---- the frozen no-cache configuration --------------------------------
      const bool cache_silent = dyn.pmu_dcache_txn == 0 && dyn.pmu_dcache_hit == 0 &&
                                dyn.pmu_dcache_miss == 0 && dyn.pmu_dcache_refill == 0 &&
                                dyn.pmu_dcache_wb == 0;
      reporter.Check(cache_silent,
                     "the p0 configuration is the no-cache configuration (" +
                         prog + "): the L1D's PMU counters are all zero "
                         "(txn/hit/miss/refill/wb=" + Dec(dyn.pmu_dcache_txn) + "/" +
                         Dec(dyn.pmu_dcache_hit) + "/" + Dec(dyn.pmu_dcache_miss) + "/" +
                         Dec(dyn.pmu_dcache_refill) + "/" + Dec(dyn.pmu_dcache_wb) + ")");

      // ---- the invariants ---------------------------------------------------
      for (const RunRecord* r : {&fixed, &dyn}) {
        const std::string label = r->fab_dyn ? "dynamic" : "fixed";
        if (r->invariant_violations != 0) {
          std::printf("    %s invariant first (%s): %s\n", label.c_str(), prog.c_str(),
                      r->invariant_first.c_str());
        }
        reporter.Check(r->invariant_violations == 0,
                       label + " configuration (" + prog +
                           "): the machine's invariants hold every cycle");
        reporter.Check(r->rob_occupied_end == 0 && r->desc_live_end == 0,
                       label + " configuration (" + prog +
                           "): the ROB and the descriptor store drain at the exit protocol");
        reporter.Check(r->passed,
                       label + " configuration (" + prog +
                           "): the program's own exit protocol declares success");
      }

      // The fixed baseline is a two-cluster machine too: a mutant that pins every
      // macro to cluster 0 (the I-023 fail mode) must not pass this gate. The
      // dynamic steering would route around it in the dynamic run, so the fixed
      // run is where the two-cluster property is asserted.
      reporter.Check(fixed.trace.both_clusters_alive.seen,
                     "two clusters were alive simultaneously (fixed baseline, " + prog +
                         "): both clusters held un-retired work" +
                         (fixed.trace.both_clusters_alive.seen
                              ? " -- first at cycle " +
                                    Dec(fixed.trace.both_clusters_alive.cycle)
                              : " -- never"));

      // ---- trace condition 5: in-order retirement within a hart -------------
      // The retire port's per-hart sequence is assigned in retirement order and
      // its field is truncated to the contract width, so the k-th event must
      // carry seq == k (mod 2^seq_w). A dropped, duplicated or reordered retire
      // event breaks the +1 chain; the architectural order itself is separately
      // checked by the oracle signature above.
      const uint64_t seq_mod = (UINT64_C(1) << geom.seq_w) - 1;
      bool in_order = !dyn.retires.empty();
      std::string in_order_why;
      for (size_t i = 0; i < dyn.retires.size(); ++i) {
        if (dyn.retires[i].seq != (i & seq_mod)) {
          in_order = false;
          in_order_why = "event " + Dec(i) + " carried seq " +
                         Dec(dyn.retires[i].seq);
          break;
        }
      }
      gate.in_order = gate.in_order || in_order;
      gate.retire_events += dyn.retires.size();
      if (in_order) {
        gate.in_order_detail = prog + ": " + Dec(dyn.retires.size()) +
                               " events, seq == event index mod " + Dec(seq_mod + 1);
      }
      reporter.Check(in_order,
                     "retirement stayed in order within a hart (" + prog +
                         ", dynamic): the per-hart sequence advances by one per event for "
                         "all " + Dec(dyn.retires.size()) + " events" +
                         (in_order_why.empty() ? "" : " -- " + in_order_why));

      // ---- the dynamic dual-cluster path actually occurred ------------------
      if (!gate.both_clusters_alive.seen && dyn.trace.both_clusters_alive.seen) {
        gate.both_clusters_alive = dyn.trace.both_clusters_alive;
        gate.both_clusters_alive.detail += " (" + prog + ")";
      }
      if (!gate.both_clusters_iq.seen && dyn.trace.both_clusters_iq.seen) {
        gate.both_clusters_iq = dyn.trace.both_clusters_iq;
        gate.both_clusters_iq.detail += " (" + prog + ")";
      }
      if (!gate.wb_collision.seen && dyn.trace.wb_collision.seen) {
        gate.wb_collision = dyn.trace.wb_collision;
        gate.wb_collision.detail += " (" + prog + ")";
      }
      if (!gate.ooo_completion.seen && dyn.trace.ooo_completion.seen) {
        gate.ooo_completion = dyn.trace.ooo_completion;
        gate.ooo_completion.detail += " (" + prog + ")";
      }

      // A remote route: a program position the dynamic steering executed in a
      // different cluster than the fixed affinity did.
      if (!gate.remote_route.seen) {
        size_t n = std::min(fixed.cluster_by_pos.size(), dyn.cluster_by_pos.size());
        for (size_t p = 0; p < n; ++p) {
          const int fc = fixed.cluster_by_pos[p];
          const int dc = dyn.cluster_by_pos[p];
          if (fc >= 0 && dc >= 0 && fc != dc) {
            gate.remote_route.seen = true;
            gate.remote_route.cycle = dyn.retires[p].grant_cycle;
            gate.remote_route.event = p;
            gate.remote_route.detail = "program position " + Dec(p) + " (pc " +
                                       U64(dyn.retires[p].pc) + ") granted at cycle " +
                                       Dec(dyn.retires[p].grant_cycle) + ": fixed cluster " +
                                       Dec(fc) + " -> dynamic cluster " + Dec(dc);
            remote_route_program = prog;
            remote_route_pos = p;
            break;
          }
        }
      }

      // Supplementary: the cross-cluster operand edges the shared wakeup/PRF
      // path carried in this program.
      const RouteStats routes = CrossClusterRoutes(dyn);
      std::printf("    cross-cluster operand edges (dynamic, %s): %llu%s%s\n",
                  prog.c_str(),
                  static_cast<unsigned long long>(routes.cross_cluster_edges),
                  routes.first.empty() ? "" : " first: ",
                  routes.first.c_str());

      // The dynamic configuration changed something measurable.
      const bool changed = (fixed.c0_alu != dyn.c0_alu) || (fixed.c1_alu != dyn.c1_alu) ||
                           (fixed.c0_br != dyn.c0_br) || (fixed.c1_br != dyn.c1_br) ||
                           (fixed.muldiv != dyn.muldiv) || (fixed.cycles != dyn.cycles);
      reporter.Check(changed,
                     "the dynamic p0 configuration changed something measurable (" +
                         prog + ")");
    }

    // ---- trace condition 2: a remote route --------------------------------
    reporter.Check(gate.remote_route.seen,
                   "a remote route occurred: the dynamic steering executed a macro in "
                   "the other cluster than the fixed affinity" +
                       (gate.remote_route.seen
                            ? std::string(" -- first at ") + gate.remote_route.detail +
                                  " in " + remote_route_program + ", program position " +
                                  Dec(remote_route_pos)
                            : std::string(" -- none occurred")));

    // ---- trace condition 1: two clusters alive simultaneously -------------
    reporter.Check(gate.both_clusters_alive.seen,
                   "two clusters were alive simultaneously: both issue queues held live "
                   "uops in the same cycle" +
                       (gate.both_clusters_alive.seen
                            ? std::string(" -- first at cycle ") +
                                  Dec(gate.both_clusters_alive.cycle) + ", " +
                                  gate.both_clusters_alive.detail
                            : std::string(" -- never")));

    // ---- trace condition 3: a writeback collision --------------------------
    reporter.Check(gate.wb_collision.seen,
                   "a writeback collision occurred: the arbiter delayed a same-bank "
                   "completion" +
                       (gate.wb_collision.seen
                            ? std::string(" -- first at cycle ") +
                                  Dec(gate.wb_collision.cycle) + ", " +
                                  gate.wb_collision.detail
                            : std::string(" -- never")));

    // ---- trace condition 4: an out-of-order completion ---------------------
    reporter.Check(gate.ooo_completion.seen,
                   "an out-of-order completion occurred: a completion was published for "
                   "a ROB entry that is not the occupied, incomplete head" +
                       (gate.ooo_completion.seen
                            ? std::string(" -- first at cycle ") +
                                  Dec(gate.ooo_completion.cycle) + ", " +
                                  gate.ooo_completion.detail
                            : std::string(" -- never")));

    // ---- trace condition 5: in-order retirement within a hart --------------
    reporter.Check(gate.in_order,
                   "retirement stayed in order within a hart across the gate (" +
                       gate.in_order_detail + ")");

    // The p0 capability advertisement is a *report* boundary, not a check: the
    // falsifiable statement that the cache is off is the per-program L1D PMU
    // counter check above, and what p0 does or does not advertise is named in
    // results/reports/p0.prototype_gate.md rather than asserted here.

    passed = (reporter.failures() == 0);
    std::printf("[trace] both clusters alive (in flight): %s\n",
                gate.both_clusters_alive.seen
                    ? ("cycle " + Dec(gate.both_clusters_alive.cycle) + ", " +
                       gate.both_clusters_alive.detail).c_str()
                    : "never");
    std::printf("[trace] both clusters alive (IQ occupancy): %s\n",
                gate.both_clusters_iq.seen
                    ? ("cycle " + Dec(gate.both_clusters_iq.cycle) + ", " +
                       gate.both_clusters_iq.detail).c_str()
                    : "never");
    std::printf("[trace] remote route: %s\n",
                gate.remote_route.seen
                    ? (gate.remote_route.detail + " in " + remote_route_program +
                       ", program position " + Dec(remote_route_pos)).c_str()
                    : "never");
    std::printf("[trace] writeback collision: %s\n",
                gate.wb_collision.seen
                    ? ("cycle " + Dec(gate.wb_collision.cycle) + ", " +
                       gate.wb_collision.detail).c_str()
                    : "never");
    std::printf("[trace] out-of-order completion: %s\n",
                gate.ooo_completion.seen
                    ? ("cycle " + Dec(gate.ooo_completion.cycle) + ", " +
                       gate.ooo_completion.detail).c_str()
                    : "never");
    std::printf("[trace] in-order retirement: %s\n", gate.in_order_detail.c_str());
    detail = "programs=" + Dec(sizeof(kPrograms) / sizeof(kPrograms[0])) +
             " both_clusters_alive=" +
             (gate.both_clusters_alive.seen ? "cycle " + Dec(gate.both_clusters_alive.cycle)
                                            : "never") +
             " both_clusters_iq=" +
             (gate.both_clusters_iq.seen ? "cycle " + Dec(gate.both_clusters_iq.cycle)
                                         : "never") +
             " remote_route=" +
             (gate.remote_route.seen ? gate.remote_route.detail : "never") +
             " wb_collision=" +
             (gate.wb_collision.seen ? "cycle " + Dec(gate.wb_collision.cycle) : "never") +
             " ooo_completion=" +
             (gate.ooo_completion.seen ? "cycle " + Dec(gate.ooo_completion.cycle)
                                       : "never") +
             " in_order_retires=" + Dec(gate.retire_events);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the p0 prototype gate", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
