// ============================================================================
// tb_soc.cpp -- CASE=soc.bus_errors_and_ids, work package I-047.
//
// The DUT is `mosaic_soc`, the platform interconnect and its peripherals. The
// driver owns both requester ports and the memory system's backpressure line,
// and it keeps its own counters for every accept and completion so the fabric's
// conservation identity is checked against an *independent* count rather than
// against itself.
//
// What the campaign establishes, one phase each:
//
//   id-preservation   two requesters issue interleaved requests with distinct
//                     identities; every completion is delivered on the port
//                     that issued it and carries that port's identity.
//   strobe-narrow     a byte store into a wide peripheral register (UART
//                     scratch) and into a wide RAM word changes exactly the
//                     strobed byte; the value read back is lane-aligned.
//   backpressure      the transaction table fills, the next offer is held (not
//                     lost, not duplicated) and completes once when it drains;
//                     a completion held by a requester's low `rsp_ready` is
//                     stable and consumed once.
//   unmapped          an address no region covers produces a DECERR completion,
//                     not silence.
//   slverr            a write to the read-only ROM, a write to the read-only
//                     harness window, an access to a device register the UART
//                     does not implement, and a misaligned device access each
//                     produce a SLVERR completion and no side effect.
//   reset-in-flight   a request accepted but not yet completed when reset
//                     asserts is aborted, reported on `o_abort_pulse`, never
//                     delivered later, and the fabric is usable afterwards.
//   mmio-exactly-once four UART RX reads pop four distinct queued words; a
//                     STATUS read pops nothing; three TX writes transmit three
//                     bytes; the timer compare and test-end registers are
//                     written once per accepted access.
//   interrupts        the timer interrupt follows mtime >= mtimecmp, the
//                     external interrupt follows the RX queue, and (on the
//                     profiles that declare it) the software interrupt follows
//                     the MSIP word.
//
// Every cycle, the conservation identity is required to hold:
//
//     accepted == completed_normal + completed_error + outstanding
//
// and the fabric's four counters are required to equal this driver's own. A
// counter leak is the card's fail mode ("a response error is dropped and the
// ROB waits for ever"), which the end-of-phase drain catches.
//
// Mutation hooks: the shipping build defines none. See
// results/reports/I-047-soc.md and tools/run_soc_controls.py.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "Vmosaic_soc_tb.h"
#include "sim_common.h"

namespace {

using mosaic::Hex;
using mosaic::Options;
using mosaic::Reporter;

// ------------------------------------------------------------- plan constants
constexpr int      kResetCycles = 4;
constexpr unsigned kIdW         = 4;      // the wrapper fixes ID_W = 4
constexpr unsigned kMaxOut      = 4;      // the wrapper fixes MAX_OUTSTANDING = 4

// The frozen map, from config/memory/p0.json (and p1.json for the MSIP word).
constexpr uint64_t kRomBase     = 0x00000000ull;
constexpr uint64_t kUartBase    = 0x00100000ull;
constexpr uint64_t kUartRx      = kUartBase + 0x00ull;
constexpr uint64_t kUartTx      = kUartBase + 0x04ull;
constexpr uint64_t kUartStatus  = kUartBase + 0x08ull;
constexpr uint64_t kUartScratch = kUartBase + 0x0Cull;
constexpr uint64_t kUartBad     = kUartBase + 0x40ull;   // unmodelled register
constexpr uint64_t kHarnBase    = 0x00102000ull;
constexpr uint64_t kClintBase   = 0x02000000ull;
constexpr uint64_t kClintMtime  = kClintBase + 0x00ull;
constexpr uint64_t kClintCmp    = kClintBase + 0x08ull;
constexpr uint64_t kClintExit   = kClintBase + 0x10ull;
constexpr uint64_t kMsip        = 0x000C0000ull;
constexpr uint64_t kRamBase     = 0x80000000ull;
constexpr uint64_t kGap         = 0x00100100ull;  // between uart and harness
constexpr uint64_t kUnmapped    = 0x40000000ull;  // no region at all
constexpr uint64_t kHarnessId   = 0x54455354ull;  // "TEST"

// Response error classes, mirrored from the RTL's localparams.
constexpr unsigned kErrSlverr = 1;
constexpr unsigned kErrDecerr = 2;

// Thrown on the first failed check; the run stops there so the report shows one
// defect rather than a thousand consequences of it.
struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& expected,
                       const std::string& actual) {
  throw Failure{where + ": expected " + expected + ", got " + actual};
}

std::string Dec(unsigned value) {
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "%u", value);
  return buffer;
}

// ---------------------------------------------------------------- bookkeeping
struct Issued {
  bool     we = false;
  uint64_t addr = 0;
  unsigned size = 0;
  unsigned nbytes = 0;
  uint8_t  wstrb = 0;
  uint64_t wdata = 0;
  bool     amo = false;
  uint64_t cycle = 0;
};

struct Completion {
  bool     seen = false;
  uint64_t rdata = 0;
  bool     fault = false;
  unsigned err = 0;
  uint64_t cycle = 0;
  Issued   req;
};

class Soc {
 public:
  Soc(Vmosaic_soc_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles)
      : dut_(dut), clk_(clk), max_cycles_(max_cycles) {}

  // ------------------------------------------------------------- scheduling
  void Step(bool rst) {
    if (clk_->cycle() >= max_cycles_) {
      Fail("max-cycles",
           "the campaign fits in " + Dec(static_cast<unsigned>(max_cycles_)) + " cycles",
           "exhausted at cycle " + Dec(static_cast<unsigned>(clk_->cycle())));
    }

    dut_->rst   = rst ? 1 : 0;
    dut_->stall = stall_held_ ? 1 : 0;
    Pump(0);
    Pump(1);
    DriveMaster(0);
    DriveMaster(1);
    dut_->rom_load_en    = rom_load_en_ ? 1 : 0;
    dut_->rom_load_index = rom_load_index_;
    dut_->rom_load_data  = rom_load_data_;
    dut_->uart_rx_push   = uart_rx_push_ ? 1 : 0;
    dut_->uart_rx_data   = uart_rx_data_;

    dut_->eval();

    // The conservation identity is a property of the fabric's registered state
    // and must hold in every live cycle, including the cycle a reset is
    // asserted.
    if (live_ && !dut_->o_conservation_ok) {
      Fail("conservation identity",
           "accepted == completed_normal + completed_error + outstanding",
           "accepted=" + Dec(static_cast<unsigned>(dut_->o_accepted_ctr)) +
           " completed=" + Dec(static_cast<unsigned>(dut_->o_completed_normal_ctr)) +
           "+" + Dec(static_cast<unsigned>(dut_->o_completed_error_ctr)) +
           " outstanding=" + Dec(static_cast<unsigned>(dut_->o_outstanding_ctr)));
    }

    if (!rst) {
      SampleHandshakes();
    } else {
      // Reset aborts whatever is outstanding; report it and mirror the clear in
      // this driver's counters so the post-edge comparison is honest.
      if (dut_->o_abort_pulse) abort_seen_ = true;
      tb_accepted_ = tb_ok_ = tb_err_ = tb_out_ = 0;
      issued_[0].clear();
      issued_[1].clear();
      accepted_.clear();
      Release(0);
      Release(1);
    }

    dut_->clk = 1;
    dut_->eval();
    CompareCounters();
    dut_->clk = 0;
    dut_->eval();

    PublishOutputs();
    clk_->Tick();
  }

  // ------------------------------------------------------------- requester API
  // A request is queued on its requester's port and presented as soon as the
  // port is free, so one requester can have several requests outstanding and
  // two requesters can interleave however the fabric's arbiter chooses. `Issue`
  // returns the identity it allocated, which is how every later check names the
  // transaction.
  unsigned Issue(unsigned m, bool we, uint64_t addr, unsigned nbytes, uint64_t value,
                 bool amo = false) {
    const unsigned id = NextId();
    Issued rec;
    rec.we = we;
    rec.addr = addr;
    rec.size = Log2(nbytes);
    rec.nbytes = nbytes;
    rec.wstrb = Wstrb(addr, nbytes);
    rec.wdata = value;
    rec.amo = amo;
    rec.cycle = clk_->cycle();
    queue_[m].push_back(std::make_pair(id, rec));
    return id;
  }

  void SetRspReady(unsigned m, bool ready) {
    if (m == 0) m0_rsp_ready_ = ready; else m1_rsp_ready_ = ready;
  }

  void Release(unsigned m) {
    queue_[m].clear();
    presenting_[m] = false;
    presenting_id_[m] = 0;
    if (m == 0) m0_valid_ = false; else m1_valid_ = false;
  }

  void SetStall(bool stall) { stall_held_ = stall; }

  // Push one byte into the UART's serial receive side and let it settle.
  void PushRx(uint8_t byte) {
    uart_rx_push_ = true;
    uart_rx_data_ = byte;
    Step(false);
    uart_rx_push_ = false;
    Step(false);
  }

  unsigned Outstanding() const { return tb_out_; }

  // A new phase starts with nothing outstanding, so its identities are fresh.
  void BeginPhase(const std::string& phase) {
    if (tb_out_ != 0) {
      Fail(phase + ": phase boundary",
           "nothing outstanding at the start of a phase",
           Dec(tb_out_) + " requests still outstanding");
    }
    comp_.clear();
    issued_[0].clear();
    issued_[1].clear();
    accepted_.clear();
    queue_[0].clear();
    queue_[1].clear();
    presenting_[0] = presenting_[1] = false;
    next_id_ = 0;
  }

  // Tick until requester m accepts the named request, or fail.
  void AwaitAccepted(unsigned m, unsigned id, const std::string& phase) {
    for (int i = 0; i < 400; ++i) {
      if (accepted_.count({m, id})) return;
      Tick(false);
    }
    Fail(phase + ": accept", "requester " + Dec(m) + " accepts id " + Dec(id) +
         " (an offered request is never lost)",
         "not accepted within 400 cycles");
  }

  // Issue one request and wait for its acceptance; returns its identity.
  unsigned IssueAndAccept(unsigned m, bool we, uint64_t addr, unsigned nbytes,
                          uint64_t value, bool amo, const std::string& phase) {
    const unsigned id = Issue(m, we, addr, nbytes, value, amo);
    AwaitAccepted(m, id, phase);
    return id;
  }

  // Tick until a named completion has been consumed, or fail. The failure names
  // the request so a dropped completion (the card's fail mode) is visible.
  const Completion& AwaitCompletion(unsigned m, unsigned id, const std::string& phase) {
    for (int i = 0; i < 2000; ++i) {
      if (CompletionSeen(m, id)) return GetCompletion(m, id);
      Tick(false);
    }
    Fail(phase + ": completion for requester " + Dec(m) + " id " + Dec(id),
         "the completion arrives (an accepted request is never dropped)",
         "absent after 2000 cycles");
  }

  // Tick until nothing is outstanding, or fail: the liveness half of "every
  // accepted request receives exactly one completion".
  void Drain(const std::string& phase) {
    for (int i = 0; i < 4000 && tb_out_ != 0; ++i) Tick(false);
    if (tb_out_ != 0) {
      Fail(phase + ": completion",
           "every accepted request completes (all " + Dec(tb_accepted_) +
               " accounted for as " + Dec(tb_ok_) + " normal + " + Dec(tb_err_) +
               " error)",
           Dec(tb_out_) + " outstanding with no completion");
    }
  }

  void Settle(int cycles) { for (int i = 0; i < cycles; ++i) Tick(false); }

  bool CompletionSeen(unsigned m, unsigned id) const {
    auto it = comp_.find({m, id});
    return it != comp_.end() && it->second.seen;
  }

  const Completion& GetCompletion(unsigned m, unsigned id) const {
    auto it = comp_.find({m, id});
    if (it == comp_.end() || !it->second.seen) {
      Fail("completion lookup", "a completion for requester " + Dec(m) + " id " + Dec(id),
           "none recorded");
    }
    return it->second;
  }

  bool ConservedEveryCycle() const { return conservation_checks_ > 0 && !conservation_failed_; }
  uint64_t Cycle() const { return clk_->cycle(); }
  bool AbortSeen() const { return abort_seen_; }
  void MarkLive() { live_ = true; }

  // Forget the records of the previous epoch after a reset aborts them.
  void ForgetHistory() {
    comp_.clear();
    issued_[0].clear();
    issued_[1].clear();
    accepted_.clear();
  }

  // Outputs sampled after each edge, including the one-cycle pulses that would
  // otherwise be missed between checks.
  bool     exit_seen = false;
  uint32_t exit_code_seen = 0;
  unsigned tx_pulses = 0;
  std::vector<uint8_t> tx_bytes;
  bool     irq_timer = false, irq_soft = false, irq_ext = false;

 private:
  static unsigned Log2(unsigned nbytes) {
    unsigned s = 0;
    while ((1u << s) < nbytes) ++s;
    return s;
  }
  static uint8_t Wstrb(uint64_t addr, unsigned nbytes) {
    return static_cast<uint8_t>(((1u << nbytes) - 1u) << (addr & 7u));
  }
  unsigned NextId() {
    unsigned id = next_id_;
    next_id_ = (next_id_ + 1u) & ((1u << kIdW) - 1u);
    return id;
  }

  void Pump(unsigned m) {
    if (presenting_[m] || queue_[m].empty()) return;
    presenting_[m] = true;
    presenting_id_[m] = queue_[m].front().first;
    presenting_rec_[m] = queue_[m].front().second;
    queue_[m].pop_front();
    if (m == 0) {
      m0_valid_ = true; m0_we_ = presenting_rec_[0].we; m0_addr_ = presenting_rec_[0].addr;
      m0_size_ = presenting_rec_[0].size; m0_wstrb_ = presenting_rec_[0].wstrb;
      m0_wdata_ = presenting_rec_[0].wdata; m0_amo_ = presenting_rec_[0].amo;
      m0_id_ = presenting_id_[0];
    } else {
      m1_valid_ = true; m1_we_ = presenting_rec_[1].we; m1_addr_ = presenting_rec_[1].addr;
      m1_size_ = presenting_rec_[1].size; m1_wstrb_ = presenting_rec_[1].wstrb;
      m1_wdata_ = presenting_rec_[1].wdata; m1_amo_ = presenting_rec_[1].amo;
      m1_id_ = presenting_id_[1];
    }
  }

  void DriveMaster(unsigned m) {
    if (m == 0) {
      dut_->m0_req_valid = m0_valid_ ? 1 : 0;
      dut_->m0_req_we    = m0_we_ ? 1 : 0;
      dut_->m0_req_addr  = m0_addr_;
      dut_->m0_req_size  = m0_size_;
      dut_->m0_req_wstrb = m0_wstrb_;
      dut_->m0_req_wdata = m0_wdata_;
      dut_->m0_req_amo   = m0_amo_ ? 1 : 0;
      dut_->m0_req_id    = m0_id_;
      dut_->m0_rsp_ready = m0_rsp_ready_ ? 1 : 0;
    } else {
      dut_->m1_req_valid = m1_valid_ ? 1 : 0;
      dut_->m1_req_we    = m1_we_ ? 1 : 0;
      dut_->m1_req_addr  = m1_addr_;
      dut_->m1_req_size  = m1_size_;
      dut_->m1_req_wstrb = m1_wstrb_;
      dut_->m1_req_wdata = m1_wdata_;
      dut_->m1_req_amo   = m1_amo_ ? 1 : 0;
      dut_->m1_req_id    = m1_id_;
      dut_->m1_rsp_ready = m1_rsp_ready_ ? 1 : 0;
    }
  }

  void Tick(bool rst) { Step(rst); }

  void SampleHandshakes() {
    for (unsigned m = 0; m < 2; ++m) {
      const bool valid = m == 0 ? m0_valid_ : m1_valid_;
      const bool ready = (m == 0 ? dut_->m0_req_ready : dut_->m1_req_ready) != 0;
      if (!(valid && ready)) continue;
      if (!presenting_[m]) {
        Fail("issue record", "the presented request is the one that was issued",
             "requester " + Dec(m) + " accepted a request it was not presenting");
      }
      issued_[m][presenting_id_[m]] = presenting_rec_[m];
      accepted_[{m, presenting_id_[m]}] = true;
      presenting_[m] = false;
      if (m == 0) m0_valid_ = false; else m1_valid_ = false;
      ++tb_accepted_;
      ++tb_out_;
    }

    for (unsigned m = 0; m < 2; ++m) {
      const bool rsp_valid = (m == 0 ? dut_->m0_rsp_valid : dut_->m1_rsp_valid) != 0;
      const bool rsp_ready = m == 0 ? m0_rsp_ready_ : m1_rsp_ready_;
      if (!(rsp_valid && rsp_ready)) continue;

      const unsigned id    = m == 0 ? static_cast<unsigned>(dut_->m0_rsp_id)
                                    : static_cast<unsigned>(dut_->m1_rsp_id);
      const uint64_t rdata = m == 0 ? dut_->m0_rsp_rdata : dut_->m1_rsp_rdata;
      const bool     fault = (m == 0 ? dut_->m0_rsp_fault : dut_->m1_rsp_fault) != 0;
      const unsigned err   = m == 0 ? static_cast<unsigned>(dut_->m0_rsp_err)
                                    : static_cast<unsigned>(dut_->m1_rsp_err);

      auto it = issued_[m].find(id);
      if (it == issued_[m].end()) {
        for (unsigned other = 0; other < 2; ++other) {
          if (other == m) continue;
          if (issued_[other].count(id)) {
            Fail("id routing",
                 "a completion for an id issued by requester " + Dec(other) +
                     " is delivered to requester " + Dec(m),
                 "requester " + Dec(m) + " was delivered id " + Dec(id) + " instead");
          }
        }
        Fail("duplicate completion",
             "every accepted request completes exactly once",
             "requester " + Dec(m) + " was delivered id " + Dec(id) + " a second time");
      }
      Completion c;
      c.seen = true;
      c.rdata = rdata;
      c.fault = fault;
      c.err = err;
      c.cycle = clk_->cycle();
      c.req = it->second;
      comp_[{m, id}] = c;
      issued_[m].erase(it);
      if (fault) ++tb_err_; else ++tb_ok_;
      --tb_out_;
    }
  }

  void CompareCounters() {
    if (!live_) return;
    if (dut_->o_accepted_ctr != tb_accepted_ ||
        dut_->o_completed_normal_ctr != tb_ok_ ||
        dut_->o_completed_error_ctr != tb_err_ ||
        dut_->o_outstanding_ctr != tb_out_) {
      Fail("counter agreement",
           "the fabric's counters equal the observed handshakes (accepted " +
               Dec(tb_accepted_) + ", ok " + Dec(tb_ok_) + ", err " + Dec(tb_err_) +
               ", outstanding " + Dec(tb_out_) + ")",
           "accepted " + Dec(static_cast<unsigned>(dut_->o_accepted_ctr)) +
               ", ok " + Dec(static_cast<unsigned>(dut_->o_completed_normal_ctr)) +
               ", err " + Dec(static_cast<unsigned>(dut_->o_completed_error_ctr)) +
               ", outstanding " + Dec(static_cast<unsigned>(dut_->o_outstanding_ctr)));
    }
    if (!dut_->o_conservation_ok) conservation_failed_ = true;
    ++conservation_checks_;
  }

  void PublishOutputs() {
    if (dut_->uart_tx_valid) {
      ++tx_pulses;
      tx_bytes.push_back(static_cast<uint8_t>(dut_->uart_tx_data));
    }
    if (dut_->exit_valid) {
      exit_seen = true;
      exit_code_seen = dut_->exit_code;
    }
    irq_timer = dut_->irq_timer != 0;
    irq_soft  = dut_->irq_soft != 0;
    irq_ext   = dut_->irq_ext != 0;
  }

  Vmosaic_soc_tb*      dut_;
  mosaic::ClockDriver* clk_;
  uint64_t             max_cycles_;
  bool                 live_ = false;

  bool     m0_valid_ = false, m0_we_ = false, m0_amo_ = false;
  uint64_t m0_addr_ = 0, m0_wdata_ = 0;
  unsigned m0_size_ = 0, m0_id_ = 0;
  uint8_t  m0_wstrb_ = 0;
  bool     m0_rsp_ready_ = true;

  bool     m1_valid_ = false, m1_we_ = false, m1_amo_ = false;
  uint64_t m1_addr_ = 0, m1_wdata_ = 0;
  unsigned m1_size_ = 0, m1_id_ = 0;
  uint8_t  m1_wstrb_ = 0;
  bool     m1_rsp_ready_ = true;

  bool     stall_held_ = false;
  bool     rom_load_en_ = false;
  unsigned rom_load_index_ = 0;
  uint64_t rom_load_data_ = 0;
  bool     uart_rx_push_ = false;
  uint8_t  uart_rx_data_ = 0;

  unsigned next_id_ = 0;
  bool     presenting_[2] = {false, false};
  unsigned presenting_id_[2] = {0, 0};
  Issued   presenting_rec_[2];
  std::deque<std::pair<unsigned, Issued>> queue_[2];
  unsigned tb_accepted_ = 0, tb_ok_ = 0, tb_err_ = 0, tb_out_ = 0;
  bool     conservation_failed_ = false;
  int      conservation_checks_ = 0;
  bool     abort_seen_ = false;
  std::map<std::pair<unsigned, unsigned>, Completion> comp_;
  std::map<unsigned, Issued> issued_[2];
  std::map<std::pair<unsigned, unsigned>, bool> accepted_;
};

void Phase(const std::string& name) {
  std::printf("phase %s\n", name.c_str());
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  std::string error;
  if (!Options::Parse(argc, argv, &options, &error)) {
    std::fprintf(stderr, "usage error: %s\n", error.c_str());
    return mosaic::kExitUsage;
  }
  if (options.case_id != "soc.bus_errors_and_ids") {
    std::fprintf(stderr, "unknown case %s\n", options.case_id.c_str());
    return mosaic::kExitUsage;
  }
  Verilated::commandArgs(argc, argv);

  Reporter reporter(options, std::string("Verilator ") + Verilated::productVersion());
  mosaic::ClockDriver clk;
  Vmosaic_soc_tb dut;
  dut.clk = 0;
  dut.rst = 1;
  Soc soc(&dut, &clk, options.max_cycles);

  bool passed = true;
  std::string detail = "campaign did not start";

  try {
    // ------------------------------------------------------------- reset
    Phase("reset");
    for (int i = 0; i < kResetCycles; ++i) soc.Step(true);
    soc.Step(false);
    soc.MarkLive();
    reporter.Check(dut.o_outstanding_ctr == 0, "reset clears outstanding");
    reporter.Check(dut.m0_rsp_valid == 0 && dut.m1_rsp_valid == 0,
                   "no completion is presented after reset");

    // --------------------------------------------------- id preservation
    Phase("id-preservation");
    soc.BeginPhase("id-preservation");
    {
      // Two requesters, distinct identities, one slave each.
      const unsigned id0 = soc.IssueAndAccept(0, false, kRamBase + 0x000, 8, 0, false,
                                              "id-preservation");
      const unsigned id1 = soc.IssueAndAccept(1, false, kClintMtime, 8, 0, false,
                                              "id-preservation");
      const Completion& c0 = soc.AwaitCompletion(0, id0, "id-preservation");
      const Completion& c1 = soc.AwaitCompletion(1, id1, "id-preservation");
      reporter.Check(!c0.fault && !c1.fault,
                     "id-preservation: both reads complete normally");
      reporter.Check(c0.req.addr == kRamBase + 0x000 && c1.req.addr == kClintMtime,
                     "id-preservation: each completion names the address it issued");

      // Three interleaved requests, two from one requester and one from the other.
      const unsigned a0 = soc.IssueAndAccept(0, false, kRamBase + 0x008, 8, 0, false,
                                             "id-preservation");
      const unsigned a1 = soc.IssueAndAccept(1, false, kRamBase + 0x010, 8, 0, false,
                                             "id-preservation");
      const unsigned a2 = soc.IssueAndAccept(0, false, kRamBase + 0x018, 8, 0, false,
                                             "id-preservation");
      soc.AwaitCompletion(0, a0, "id-preservation");
      soc.AwaitCompletion(1, a1, "id-preservation");
      soc.AwaitCompletion(0, a2, "id-preservation");
      soc.Drain("id-preservation");
      reporter.Check(soc.ConservedEveryCycle(),
                     "id-preservation: the identity held every cycle");
      reporter.Check(dut.o_id_mismatch == 0, "id-preservation: no slave mangled an id");
    }

    // ------------------------------------------------------- narrow strobes
    Phase("strobe-narrow");
    soc.BeginPhase("strobe-narrow");
    {
      // A word write to the 32-bit UART scratch register, lane-aligned: the
      // register's bytes sit at lanes 4..7 of the port's doubleword.
      const unsigned w0 = soc.IssueAndAccept(0, true, kUartScratch, 4,
                                             0xAABBCCDDull << 32, false, "strobe-narrow");
      soc.AwaitCompletion(0, w0, "strobe-narrow");

      // A byte write at scratch+1: only that byte may change.
      const unsigned w1 = soc.IssueAndAccept(0, true, kUartScratch + 1, 1,
                                             0x11ull << 40, false, "strobe-narrow");
      soc.AwaitCompletion(0, w1, "strobe-narrow");

      const unsigned r0 = soc.IssueAndAccept(0, false, kUartScratch, 4, 0, false,
                                             "strobe-narrow");
      {
        const Completion& c = soc.AwaitCompletion(0, r0, "strobe-narrow");
        const uint64_t expect = 0xAABB11DDull << 32;
        if (c.rdata != expect) {
          Fail("strobe-narrow: UART scratch read-back", Hex(expect), Hex(c.rdata));
        }
      }

      // The same rule on a wide RAM word: an 8-byte store then a byte store.
      const unsigned m0 = soc.IssueAndAccept(0, true, kRamBase + 0x100, 8,
                                             0x1122334455667788ull, false, "strobe-narrow");
      soc.AwaitCompletion(0, m0, "strobe-narrow");
      const unsigned m1 = soc.IssueAndAccept(0, true, kRamBase + 0x103, 1,
                                             0xAAull << 24, false, "strobe-narrow");
      soc.AwaitCompletion(0, m1, "strobe-narrow");
      const unsigned m2 = soc.IssueAndAccept(0, false, kRamBase + 0x100, 8, 0, false,
                                             "strobe-narrow");
      {
        const Completion& c = soc.AwaitCompletion(0, m2, "strobe-narrow");
        const uint64_t expect = 0x11223344AA667788ull;
        if (c.rdata != expect) {
          Fail("strobe-narrow: RAM byte-store read-back", Hex(expect), Hex(c.rdata));
        }
      }
      soc.Drain("strobe-narrow");
      reporter.Check(soc.ConservedEveryCycle(),
                     "strobe-narrow: the identity held every cycle");
    }

    // ---------------------------------------------------------- backpressure
    Phase("backpressure");
    soc.BeginPhase("backpressure");
    {
      // Fill the transaction table with requests the stalled memory cannot take.
      soc.SetStall(true);
      unsigned ids[kMaxOut];
      for (unsigned i = 0; i < kMaxOut; ++i) {
        ids[i] = soc.Issue(i % 2, false, kRamBase + 0x200 + 8 * i, 8, 0);
      }
      for (unsigned i = 0; i < kMaxOut; ++i) {
        soc.AwaitAccepted(i % 2, ids[i], "backpressure");
      }
      reporter.Check(soc.Outstanding() == kMaxOut,
                     "backpressure: the table holds exactly MAX_OUTSTANDING requests");

      // The next offer cannot be accepted while the table is full.
      const unsigned fifth = soc.Issue(0, false, kRamBase + 0x240, 8, 0);
      for (int i = 0; i < 4; ++i) soc.Step(false);
      reporter.Check(dut.m0_req_ready == 0,
                     "backpressure: the fabric refuses a request while full");

      // Release the memory system: every request, including the held one,
      // completes exactly once.
      soc.SetStall(false);
      soc.AwaitAccepted(0, fifth, "backpressure");
      soc.Drain("backpressure");
      reporter.Check(soc.CompletionSeen(0, fifth),
                     "backpressure: the held request is accepted once and completes");
      reporter.Check(soc.Outstanding() == 0, "backpressure: nothing is left outstanding");

      // A completion held by a low requester `rsp_ready` is stable and consumed
      // exactly once.
      const unsigned held = soc.IssueAndAccept(0, false, kRamBase + 0x280, 8, 0, false,
                                               "backpressure");
      soc.SetRspReady(0, false);
      bool saw_valid = false;
      for (int i = 0; i < 20 && !soc.CompletionSeen(0, held); ++i) {
        soc.Step(false);
        if (dut.m0_rsp_valid) saw_valid = true;
      }
      soc.SetRspReady(0, true);
      soc.AwaitCompletion(0, held, "backpressure");
      reporter.Check(saw_valid, "backpressure: the completion was presented while held");
      soc.Drain("backpressure");
      reporter.Check(soc.ConservedEveryCycle(),
                     "backpressure: the identity held every cycle");
    }

    // --------------------------------------------------------------- unmapped
    Phase("unmapped");
    soc.BeginPhase("unmapped");
    {
      const uint32_t dec_before = dut.o_dec_err_ctr;
      const unsigned d0 = soc.IssueAndAccept(0, false, kUnmapped, 8, 0, false, "unmapped");
      const Completion& c = soc.AwaitCompletion(0, d0, "unmapped");
      reporter.Check(c.fault, "unmapped: a read to an unmapped address faults");
      reporter.Check(c.err == kErrDecerr, "unmapped: the error class is DECERR");
      reporter.Check(c.rdata == 0, "unmapped: the faulted read returns zero data");

      const unsigned d1 = soc.IssueAndAccept(1, true, kGap, 4, 0, false, "unmapped");
      const Completion& c2 = soc.AwaitCompletion(1, d1, "unmapped");
      reporter.Check(c2.fault && c2.err == kErrDecerr,
                     "unmapped: a write to the gap between regions is DECERR");
      soc.Drain("unmapped");
      reporter.Check(dut.o_dec_err_ctr == dec_before + 2,
                     "unmapped: the DECERR counter records exactly two errors");
    }

    // ----------------------------------------------------------------- slverr
    Phase("slverr");
    soc.BeginPhase("slverr");
    {
      const uint32_t slv_before = dut.o_slv_err_ctr;

      const unsigned e0 = soc.IssueAndAccept(0, true, kRomBase, 4, 0xDEADBEEFull, false,
                                             "slverr");
      const Completion& c0 = soc.AwaitCompletion(0, e0, "slverr");
      reporter.Check(c0.fault && c0.err == kErrSlverr,
                     "slverr: a write to the boot ROM is SLVERR");

      const unsigned e1 = soc.IssueAndAccept(0, true, kHarnBase + 4, 4, 0, false, "slverr");
      const Completion& c1 = soc.AwaitCompletion(0, e1, "slverr");
      reporter.Check(c1.fault && c1.err == kErrSlverr,
                     "slverr: a write to the harness window is SLVERR");

      const unsigned e2 = soc.IssueAndAccept(0, false, kHarnBase, 4, 0, false, "slverr");
      {
        const Completion& c = soc.AwaitCompletion(0, e2, "slverr");
        reporter.Check(!c.fault && c.rdata == kHarnessId,
                       "slverr: the harness identity register reads TEST");
      }

      const unsigned e3 = soc.IssueAndAccept(0, true, kUartBad, 4, 0, false, "slverr");
      const Completion& c3 = soc.AwaitCompletion(0, e3, "slverr");
      reporter.Check(c3.fault && c3.err == kErrSlverr,
                     "slverr: an unmodelled UART register is SLVERR");

      const unsigned e4 = soc.IssueAndAccept(0, false, kUartScratch + 2, 4, 0, false,
                                             "slverr");
      const Completion& c4 = soc.AwaitCompletion(0, e4, "slverr");
      reporter.Check(c4.fault && c4.err == kErrSlverr,
                     "slverr: a misaligned device access is SLVERR");

      const unsigned e5 = soc.IssueAndAccept(0, false, kRomBase, 4, 0, false, "slverr");
      {
        const Completion& c = soc.AwaitCompletion(0, e5, "slverr");
        reporter.Check(!c.fault && c.rdata == 0,
                       "slverr: the refused ROM write changed nothing");
      }
      soc.Drain("slverr");
      reporter.Check(dut.o_slv_err_ctr == slv_before + 4,
                     "slverr: the SLVERR counter records exactly four errors");
      reporter.Check(soc.ConservedEveryCycle(), "slverr: the identity held every cycle");
    }

    // ------------------------------------------------------- reset in flight
    Phase("reset-in-flight");
    soc.BeginPhase("reset-in-flight");
    {
      soc.SetStall(true);
      const unsigned t0 = soc.Issue(0, false, kRamBase + 0x300, 8, 0);
      soc.AwaitAccepted(0, t0, "reset-in-flight");
      soc.Settle(1);
      reporter.Check(soc.Outstanding() == 1,
                     "reset-in-flight: one request is accepted and outstanding");

      // Assert reset while the transaction is in flight.
      soc.Step(true);
      reporter.Check(soc.AbortSeen(), "reset-in-flight: o_abort_pulse reports the abort");
      soc.Step(true);
      soc.SetStall(false);
      soc.Step(false);

      reporter.Check(dut.o_outstanding_ctr == 0, "reset-in-flight: reset clears outstanding");
      reporter.Check(dut.m0_rsp_valid == 0,
                     "reset-in-flight: the aborted request is never completed later");
      reporter.Check(dut.o_accepted_ctr == 0 && dut.o_completed_normal_ctr == 0 &&
                         dut.o_completed_error_ctr == 0,
                     "reset-in-flight: reset clears the counters together");
      soc.MarkLive();
      soc.ForgetHistory();

      // The fabric is alive after the reset.
      const unsigned t1 = soc.IssueAndAccept(0, false, kRamBase + 0x308, 8, 0, false,
                                            "reset-in-flight");
      const Completion& c = soc.AwaitCompletion(0, t1, "reset-in-flight");
      reporter.Check(!c.fault, "reset-in-flight: a fresh request completes after reset");
      soc.Drain("reset-in-flight");
    }

    // ------------------------------------------------------ mmio exactly once
    Phase("mmio-exactly-once");
    soc.BeginPhase("mmio-exactly-once");
    {
      // Queue four distinct input words on the UART's serial side.
      for (unsigned i = 0; i < 4; ++i) {
        soc.PushRx(static_cast<uint8_t>(0x41 + i));
      }
      const uint32_t pops_before = dut.o_uart_rx_pop_ctr;

      // Four RX reads pop exactly four words, in order.
      const uint64_t expect_rx[4] = {0x41, 0x42, 0x43, 0x44};
      for (unsigned i = 0; i < 4; ++i) {
        const unsigned id = soc.IssueAndAccept(0, false, kUartRx, 4, 0, false,
                                               "mmio-exactly-once");
        const Completion& c = soc.AwaitCompletion(0, id, "mmio-exactly-once");
        if (c.rdata != expect_rx[i]) {
          Fail("mmio-exactly-once: RX pop " + Dec(i), Hex(expect_rx[i]), Hex(c.rdata));
        }
      }
      reporter.Check(dut.o_uart_rx_pop_ctr == pops_before + 4,
                     "mmio-exactly-once: four RX reads popped exactly four words");
      soc.Drain("mmio-exactly-once");

      // A STATUS read has no side effect.
      const unsigned st = soc.IssueAndAccept(0, false, kUartStatus, 4, 0, false,
                                             "mmio-exactly-once");
      soc.AwaitCompletion(0, st, "mmio-exactly-once");
      reporter.Check(dut.o_uart_rx_pop_ctr == pops_before + 4,
                     "mmio-exactly-once: a STATUS read does not pop");
      reporter.Check(!soc.irq_ext,
                     "mmio-exactly-once: the external interrupt clears with RX");

      // Three TX writes transmit three bytes, one pulse each.
      const uint32_t tx_before = dut.o_uart_tx_ctr;
      soc.tx_pulses = 0;
      soc.tx_bytes.clear();
      for (unsigned i = 0; i < 3; ++i) {
        const unsigned id = soc.IssueAndAccept(0, true, kUartTx, 1,
                                               static_cast<uint64_t>(0x61 + i) << 32,
                                               false, "mmio-exactly-once");
        soc.AwaitCompletion(0, id, "mmio-exactly-once");
      }
      soc.Settle(2);
      reporter.Check(dut.o_uart_tx_ctr == tx_before + 3,
                     "mmio-exactly-once: three TX writes are three side effects");
      reporter.Check(soc.tx_pulses == 3, "mmio-exactly-once: the UART emitted three pulses");
      soc.Drain("mmio-exactly-once");

      // The timer compare and the test-end register each happen once.
      const unsigned cmpw = soc.IssueAndAccept(0, true, kClintCmp, 8, 0x1234ull, false,
                                               "mmio-exactly-once");
      soc.AwaitCompletion(0, cmpw, "mmio-exactly-once");
      const unsigned cmpr = soc.IssueAndAccept(0, false, kClintCmp, 8, 0, false,
                                               "mmio-exactly-once");
      {
        const Completion& c = soc.AwaitCompletion(0, cmpr, "mmio-exactly-once");
        reporter.Check(!c.fault && c.rdata == 0x1234ull,
                       "mmio-exactly-once: the timer compare reads back what was written");
      }

      const uint32_t exit_before = dut.o_clint_exit_ctr;
      const unsigned ex = soc.IssueAndAccept(0, true, kClintExit, 4, 5, false,
                                             "mmio-exactly-once");
      soc.AwaitCompletion(0, ex, "mmio-exactly-once");
      soc.Settle(1);
      reporter.Check(dut.o_clint_exit_ctr == exit_before + 1,
                     "mmio-exactly-once: the test-end write is one side effect");
      reporter.Check(soc.exit_seen && soc.exit_code_seen == 5,
                     "mmio-exactly-once: the test-end register carries the written code");
      soc.Drain("mmio-exactly-once");
      reporter.Check(soc.ConservedEveryCycle(),
                     "mmio-exactly-once: the identity held every cycle");
    }

    // ------------------------------------------------------------ interrupts
    Phase("interrupts");
    soc.BeginPhase("interrupts");
    {
      // mtime counts upward; a compare of zero asserts the timer line and an
      // all-ones compare clears it.
      const unsigned tz = soc.IssueAndAccept(0, true, kClintCmp, 8, 0, false, "interrupts");
      soc.AwaitCompletion(0, tz, "interrupts");
      soc.Settle(1);
      reporter.Check(soc.irq_timer, "interrupts: mtime >= mtimecmp asserts the timer line");

      const unsigned tf = soc.IssueAndAccept(0, true, kClintCmp, 8, 0xFFFFFFFFFFFFFFFFull,
                                             false, "interrupts");
      soc.AwaitCompletion(0, tf, "interrupts");
      soc.Settle(1);
      reporter.Check(!soc.irq_timer, "interrupts: a future compare deasserts the timer line");

      // The MSIP word is a p1 (and later) region; probe it and test whichever
      // map the profile declares.
      const unsigned probe_id = soc.IssueAndAccept(0, false, kMsip, 4, 0, false,
                                                   "interrupts");
      const Completion& probe = soc.AwaitCompletion(0, probe_id, "interrupts");
      if (probe.fault) {
        reporter.Check(probe.err == kErrDecerr,
                       "interrupts: the MSIP word is DECERR where the map omits it");
      } else {
        const unsigned ms = soc.IssueAndAccept(0, true, kMsip, 4, 1, false, "interrupts");
        soc.AwaitCompletion(0, ms, "interrupts");
        soc.Settle(1);
        reporter.Check(soc.irq_soft, "interrupts: writing MSIP asserts the software line");
        const unsigned mc = soc.IssueAndAccept(0, true, kMsip, 4, 0, false, "interrupts");
        soc.AwaitCompletion(0, mc, "interrupts");
        soc.Settle(1);
        reporter.Check(!soc.irq_soft, "interrupts: clearing MSIP deasserts the software line");
      }
      soc.Drain("interrupts");
      reporter.Check(soc.ConservedEveryCycle(), "interrupts: the identity held every cycle");
    }

    // ------------------------------------------------------------- summary
    reporter.Check(soc.ConservedEveryCycle(),
                   "the conservation identity held in every live cycle");
    reporter.Check(soc.Outstanding() == 0 && dut.o_outstanding_ctr == 0,
                   "every accepted request completed by the end of the campaign");
    reporter.Check(dut.o_id_mismatch == 0, "no slave corrupted a transaction id");

    detail = "accepted=" + Dec(dut.o_accepted_ctr) + " normal=" +
             Dec(dut.o_completed_normal_ctr) + " error=" +
             Dec(dut.o_completed_error_ctr) + " decerr=" + Dec(dut.o_dec_err_ctr) +
             " slverr=" + Dec(dut.o_slv_err_ctr) + " rx_pop=" +
             Dec(dut.o_uart_rx_pop_ctr) + " tx=" + Dec(dut.o_uart_tx_ctr) +
             " exit=" + Dec(dut.o_clint_exit_ctr) + " cycles=" +
             Dec(static_cast<unsigned>(soc.Cycle()));
  } catch (const Failure& failure) {
    passed = false;
    detail = "first failure: " + failure.what;
    std::fprintf(stderr, "FAIL %s\n", failure.what.c_str());
  }

  const bool ok = passed && reporter.failures() == 0;
  return reporter.Finish(ok ? "PASS" : "FAIL", detail);
}
