// ============================================================================
// tb_core_vecselfcheck.cpp -- CASE=vec.selfcheck_corpus
//
// Run the self-checking RVV programs in tests/programs/vec/build/ on the
// integrated out-of-order core and require every one to reach its PASS trap.
//
// ---------------------------------------------------------- why this exists
//
// The scalar corpus is checked against tools/host_oracle.py, which models no
// vector state at all, so a vector program cannot be oracle-recorded.  The
// programs under tests/programs/vec/ therefore carry their expected values as
// constants derived by hand from the RVV specification (the derivation is in
// each program's header comment), compute the vector result on the DUT, and
// compare the DUT's result against those constants *inside the program*.  A
// disagreement makes the program write a non-zero word with bit 0 clear to
// TOHOST, which the frozen protocol reports as FAIL.
//
// This driver's whole job is to require that every listed program reaches the
// PASS trap and to name the first program that does not.  It does not and
// cannot decide whether a program's hand derivation is right; that is the human
// oracle's job and the reason these programs are evidence of execution and
// arithmetic, not of conformance.
//
// ---------------------------------------------------------- what is checked
//
// For each program:
//   * it reaches the frozen exit protocol (a non-zero write to TOHOST);
//   * that write is the PASS encoding (TOHOST == 1);
//   * the vector engine actually ran: at least one vector macro retired, no
//     vector trap and no vector fault, the vector length CSRs read back
//     (vlenb == 16 for VLEN = 128), and vill is clear;
//   * `vec_status` (a symbol in the program image) is 1.
//
// On a failure the driver prints `CHECK FAILED: <program>: <detail>` and names
// the first failing check the program recorded (its check index and the first
// differing byte offset, both read out of `vec_status`), or the raw mcause when
// the program trapped.
//
// ------------------------------------------------------------ the controls
//
// `--programs a,b,c` and `--vec-dir <dir>` let tools/run_vector_selfcheck_
// controls.py run one program (the deliberately-wrong-constant control) from a
// separate build directory through the same driver and require it to FAIL.
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "Vmosaic_core_tb.h"

#include "bus_reset_gate.h"
#include "elf_loader.h"
#include "mem_ref.h"
#include "memory_model.h"
#include "mosaic_platform.h"
#include "sim_common.h"

namespace {

using mosaic_ref::DataMem;

// Reset is held for this many rising edges; the same number every core case
// uses.
constexpr int kResetCycles = 4;
// After the program writes TOHOST, let the last vector counters land.
constexpr int kSettleCycles = 8;
// No commit and no allocation for this long is a hang with a location.
constexpr uint64_t kStallCycles = 200000;

// The programs the registered case runs.  Each name is both the ELF basename
// (<name>.elf) and the label used in the per-program line.
const char* const kPrograms[] = {
    "v01_addsub_e64",
    "v02_addsub_e32",
    "v03_logic_e16",
    "v04_masklog_e8",
    "v05_maskpfx_e8",
    "v06_loadstore",
    "v08_masked_addsub_e32",
    "v10_wide_e32",
    "v11_mul_e32",
    "v12_mulw_e16",
    "v13_shift_e32",
    "v15_minmax_e32",
    "v16_cmp_e32",
    "v17_sat_e32",
    "v18_slide_e32",
    "v19_gather_e32",
    "v20_compress_e8",
    "v21_reduce_e32",
    "v22_redwide_e16",
};

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string Dec(uint64_t value) { return std::to_string(value); }

// ------------------------------------------------------------ program image
class ProgImage {
 public:
  bool LoadElf(const std::string& path, std::string* detail) {
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
  mosaic::Image elf_;
  uint64_t entry_ = 0;
};

// ------------------------------------------------------- instruction memory
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

struct Geometry {
  uint32_t xlen = 0;
  uint64_t reset_vector = 0;
};

Geometry ReadGeometry(Vmosaic_core_tb* dut) {
  Geometry g;
  g.xlen = dut->o_geom_xlen_o;
  g.reset_vector = dut->o_geom_reset_vector_o;
  return g;
}

std::string FindRepoRoot() {
  std::string dir = ".";
  for (int depth = 0; depth < 8; ++depth) {
    std::ifstream probe(dir + "/config/profiles/p0.json");
    if (probe) {
      char resolved[4096];
      if (realpath(dir.c_str(), resolved) != nullptr) return std::string(resolved);
      return dir;
    }
    dir += "/..";
  }
  Fail("setup", "cannot find the repository root: no config/profiles/p0.json above the "
                "working directory");
}

// -------------------------------------------------------------- one run
struct Outcome {
  bool finished = false;       // the program wrote TOHOST
  uint64_t tohost = 0;         // the word it wrote
  bool passed = false;         // TOHOST == 1
  uint64_t vec_retire = 0;
  uint64_t vec_trap = 0;
  uint64_t vec_fault = 0;
  uint64_t vec_vlenb = 0;
  uint64_t vec_vill = 0;
  uint64_t vec_alu_elems = 0;
  uint64_t vec_vrf_bad = 0;
  bool stopped = false;
  bool timed_out = false;
  bool stalled = false;
  uint64_t cycles = 0;
};

class Runner {
 public:
  explicit Runner(Vmosaic_core_tb* dut) : dut_(dut) {}

  Outcome Run(const ProgImage& image, mosaic::MemoryModel* mem, uint64_t max_cycles) {
    Outcome out;
    Imem imem(&image);
    DataMem dmem(mem);
    imem.Reset();
    dmem.Reset();
    cycles_ = 0;
    last_commit_ = 0;
    last_alloc_ = 0;
    last_progress_ = 0;

    dut_->clk = 0;
    dut_->rst = 1;
    dut_->eval();
    for (int i = 0; i < kResetCycles; i++) Cycle(true, &imem, &dmem);

    while (true) {
      Cycle(false, &imem, &dmem);
      if (mem->finished()) {
        out.finished = true;
        break;
      }
      if (dut_->o_stopped_o != 0) {
        out.stopped = true;
        break;
      }
      if (cycles_ >= max_cycles) {
        out.timed_out = true;
        break;
      }
      if (cycles_ - last_progress_ > kStallCycles) {
        out.stalled = true;
        break;
      }
    }
    for (int i = 0; i < kSettleCycles; i++) Cycle(false, &imem, &dmem);

    out.cycles = cycles_;
    if (mem->finished()) {
      out.tohost = mem->exit_code();
      out.passed = mem->passed();
    }
    out.vec_retire = dut_->o_vec_retire_ctr_o;
    out.vec_trap = dut_->o_vec_trap_ctr_o;
    out.vec_fault = dut_->o_vec_fault_ctr_o;
    out.vec_vlenb = dut_->o_vec_vlenb_o;
    out.vec_vill = dut_->o_vec_vill_o;
    out.vec_alu_elems = dut_->o_vec_alu_elems_o;
    out.vec_vrf_bad = dut_->o_vec_vrf_bad_ctr_o;
    return out;
  }

 private:
  void Cycle(bool rst, Imem* imem, DataMem* dmem) {
    dut_->rst = rst ? 1 : 0;
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

    dut_->eval();

    if (bus_reset_.MayAccept(rst, (dut_->imem_req_valid_o != 0) &&
                                      (dut_->imem_req_ready_i != 0))) {
      imem->Accept(dut_->imem_req_addr_o, dut_->imem_req_id_o,
                   dut_->imem_req_epoch_o);
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

    const bool progressed = (dut_->o_commit_o != last_commit_) ||
                            (dut_->o_wb_pub_valid_o != 0) ||
                            (dut_->o_dbg_alloc_ctr_o != last_alloc_);
    last_commit_ = dut_->o_commit_o;
    last_alloc_ = dut_->o_dbg_alloc_ctr_o;
    if (progressed) last_progress_ = cycles_;
  }

  Vmosaic_core_tb* dut_;
  mosaic::BusResetGate bus_reset_;
  uint64_t cycles_ = 0;
  uint64_t last_commit_ = 0;
  uint64_t last_alloc_ = 0;
  uint64_t last_progress_ = 0;
};

std::vector<std::string> SplitList(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : text) {
    if (c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::string programs_arg;
  std::string vec_dir;
  std::vector<char*> filtered;
  filtered.push_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const bool has_value = (i + 1) < argc;
    if (flag == "--programs" && has_value) {
      programs_arg = argv[++i];
    } else if (flag == "--vec-dir" && has_value) {
      vec_dir = argv[++i];
    } else {
      filtered.push_back(argv[i]);
      if (has_value && (flag == "--case" || flag == "--out" || flag == "--image" ||
                        flag == "--expect" || flag == "--seed" ||
                        flag == "--max-cycles")) {
        filtered.push_back(argv[++i]);
      }
    }
  }

  mosaic::Options options;
  std::string error;
  if (!mosaic::Options::Parse(static_cast<int>(filtered.size()), filtered.data(),
                              &options, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return mosaic::kExitUsage;
  }
  Verilated::commandArgs(argc, argv);

  mosaic::Reporter reporter(options,
                            std::string("Verilator ") + Verilated::productVersion());
  Vmosaic_core_tb dut;

  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();
    const Geometry geometry = ReadGeometry(&dut);
    const std::string repo = FindRepoRoot();

    if (geometry.xlen != 64) Fail("geometry", "the profile is not 64-bit");
    if (geometry.reset_vector != 0x80000000ull) {
      Fail("geometry", "the reset vector is not the corpus link base: " +
                           U64(geometry.reset_vector));
    }

    std::vector<std::string> programs;
    if (!programs_arg.empty()) {
      programs = SplitList(programs_arg);
    } else {
      for (const char* name : kPrograms) programs.push_back(name);
    }
    const std::string dir =
        vec_dir.empty() ? (repo + "/tests/programs/vec/build") : vec_dir;

    std::printf("CASE vec.selfcheck_corpus -- self-checking RVV programs on the "
                "out-of-order core\n");
    std::printf("  reset vector %s, TOHOST %s, image dir %s\n",
                U64(geometry.reset_vector).c_str(), U64(MOSAIC_TOHOST).c_str(),
                dir.c_str());
    std::printf("  programs: %zu; each carries its expected values as constants "
                "derived by hand from the RVV specification\n", programs.size());

    Runner runner(&dut);
    int passed = 0;
    int ran = 0;

    for (const std::string& name : programs) {
      const std::string elf_path = dir + "/" + name + ".elf";
      ProgImage image;
      std::string load_detail;
      if (!image.LoadElf(elf_path, &load_detail)) {
        reporter.Check(false, name + ": " + load_detail +
                                   " (build with `make -C tests/programs/vec all`)");
        continue;
      }

      mosaic::MemoryModel mem;
      std::string mem_detail;
      if (!mem.LoadImage(image.elf(), &mem_detail)) {
        reporter.Check(false, name + ": " + mem_detail);
        continue;
      }

      ++ran;
      const Outcome out = runner.Run(image, &mem, options.max_cycles);

      // Read the program's own status word, if it has one.
      uint64_t status = 0;
      bool have_status = false;
      if (const mosaic::Symbol* sym = image.elf().FindSymbol("vec_status")) {
        uint64_t value = 0;
        if (mem.Read(sym->value, 8, &value) == mosaic::AccessStatus::kOk) {
          status = value;
          have_status = true;
        }
      }
      std::string detail;
      if (have_status) {
        if ((status & 0x80000000ull) != 0) {
          detail = "trapped: mcause=" + Dec(status & 0xFFull);
        } else if (status != 0) {
          detail = "check " + Dec(status >> 16) + " failed at byte offset " +
                   Dec(status & 0xFFFFull);
        } else {
          detail = "vec_status is 0";
        }
      }

      bool ok = true;
      if (!out.finished) {
        ok = false;
        detail = out.stopped ? "the machine refused an instruction and stopped"
               : out.timed_out ? "timed out after " + Dec(options.max_cycles) + " cycles"
               : out.stalled ? "stalled with no forward progress"
                             : "did not reach the exit protocol";
      } else if (!out.passed) {
        ok = false;
        detail = "wrote TOHOST=" + U64(out.tohost) + " (FAIL); " + detail;
      } else if (out.vec_retire == 0) {
        ok = false;
        detail = "no vector macro retired (the vector path was not taken)";
      } else if (out.vec_trap != 0) {
        ok = false;
        detail = "the vector engine reported " + Dec(out.vec_trap) + " vector trap(s)";
      } else if (out.vec_fault != 0) {
        ok = false;
        detail = "the vector engine reported " + Dec(out.vec_fault) + " vector fault(s)";
      } else if (out.vec_vill != 0) {
        ok = false;
        detail = "vill is set (the last vsetvli established an unsupported vtype)";
      } else if (out.vec_vlenb != 16) {
        ok = false;
        detail = "vlenb reads " + Dec(out.vec_vlenb) + ", expected 16 (VLEN=128)";
      } else if (out.vec_vrf_bad != 0) {
        ok = false;
        detail = "the VRF refused " + Dec(out.vec_vrf_bad) + " read(s)";
      } else if (have_status && status != 1) {
        ok = false;
        detail = "reached TOHOST=1 but vec_status=" + Dec(status);
      }

      if (ok) {
        ++passed;
        reporter.Check(true, name);
        std::printf("  PASS %-22s vec_retire=%-3llu elems=%-4llu vlenb=%-2llu cycles=%llu\n",
                    name.c_str(),
                    static_cast<unsigned long long>(out.vec_retire),
                    static_cast<unsigned long long>(out.vec_alu_elems),
                    static_cast<unsigned long long>(out.vec_vlenb),
                    static_cast<unsigned long long>(out.cycles));
      } else {
        reporter.Check(false, name + ": " + detail);
        std::printf("  FAIL %-22s %s\n", name.c_str(), detail.c_str());
      }
    }

    const std::string detail = Dec(static_cast<uint64_t>(passed)) + "/" +
                               Dec(static_cast<uint64_t>(ran)) +
                               " self-checking RVV programs reached the pass trap";
    return reporter.Finish(passed == ran ? "PASS" : "FAIL", detail);
  } catch (const Failure& f) {
    std::fprintf(stderr, "CHECK FAILED: %s\n", f.what.c_str());
    return reporter.Finish("FAIL", f.what);
  }
}
