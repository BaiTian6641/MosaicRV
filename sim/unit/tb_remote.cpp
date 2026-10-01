// ============================================================================
// tb_remote.cpp -- CASE=remote.kill_with_delayed_response, work package I-028.
//
// Drives sim/tb/mosaic_remote_link_tb.sv and checks every cycle against an
// *independent* shadow model written from the prose contract at the top of
// rtl/core/mosaic_remote_link.sv.
//
// ------------------------------------------------ independence of the model
//
// The shadow is not a transcription of the RTL. It holds the same information
// in a different shape:
//
//   * The DUT's pipelines are `PIPE_DEPTH` arrays of flattened payload bits;
//     the shadow holds *fields* (identity, destination, opcode, immediate, the
//     two operands, the link id) and only builds the packed word when it
//     compares. A wrong field order in the RTL therefore shows up as a
//     mismatch, and the layout itself is checked in phase 0 against the offsets
//     the DUT reports from its own parameter list.
//   * The DUT's counters are 32-bit registers moved by the same signals that
//     move the table; the shadow's are 64-bit and unbounded, so a wrap in the
//     DUT cannot cancel against the same wrap here.
//   * The credit law is *checked*, not stored: every cycle the harness requires
//     `issued == returned + outstanding` on the DUT's own ports, requires
//     `returned == matched + killed`, and requires the outstanding counter to
//     equal the population of the occupancy bitmap. Three views of one fact,
//     two of which the shadow does not produce.
//
// The remote functional unit is an environment model in this file, not part of
// the shadow: it consumes the request *as the DUT presents it* on `rem_req_*`,
// computes a value from the opcode and the operands it was handed, and returns
// it after a delay the phase selects. The round trip is therefore checked end
// to end -- the value delivered at the home port is a function of what left the
// remote port -- and the link is never told anything the far side would not
// know.
//
// ------------------------------------------------------------------ phases
//
//   0  geometry        the elaborated sizes are the ones this file assumes, and
//                      the wire layout is the one it packs
//   1  reset-clean     nothing live, nothing counted, the credit law holds
//   2  round-trip      one request/response at each injected delay 1/2/4/8,
//                      with the exact cycle of every hop asserted
//   3  out-of-order    responses delivered in the order they *arrive*, which is
//                      not the order the requests were issued
//   4  kill-late       the namesake: a killed request's response is dropped and
//                      counted -- including when it arrives in the same cycle
//                      as the kill -- the credit comes back exactly once, and a
//                      new request with a different identity takes the recycled
//                      slot and completes while the stale response is refused
//   5  back-pressure   both directions saturated and released; no loss, no
//                      deadlock, and a stalled response path never blocks a
//                      request a free credit could carry
//   6  randomised      issue / kill / flush / back-pressure at random delays,
//                      shadow-compared every cycle, then drained to empty
//                      within a stated bound
//
// Coverage is counted and asserted at the end, so a stimulus change that stops
// reaching the recycled-slot case, the alias rejection, the kill, the flush or
// either back-pressure path fails the case instead of quietly passing over a
// shorter path.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_remote_link_tb.h"

namespace {

// Thrown on the first failed check, so the report names one defect rather than a
// thousand consequences of it.
struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

void Require(bool condition, const std::string& where, const std::string& detail) {
  if (!condition) Fail(where, detail);
}

std::string Bool(bool value) { return value ? "1" : "0"; }
std::string Dec(uint64_t value) { return std::to_string(value); }

std::string Hex64(uint64_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "0x%016llx",
                static_cast<unsigned long long>(value));
  return std::string(buffer);
}

// ---------------------------------------------------------------- geometry
// The sizes this file was written against. They are *asserted* against the
// elaborated hardware in phase 0 rather than being the source of truth, so a
// profile this file was not updated for fails loudly instead of quietly
// checking a different design.
constexpr unsigned kEntries   = 8;   // MOSAIC_IQ_ENTRIES
constexpr unsigned kIdW       = 17;  // macro_id_t: 1 + 6 + 7 + 3
constexpr unsigned kDstW      = 15;  // prf tag (7) + generation (8)
constexpr unsigned kOpW       = 4;   // mosaic_pkg::alu_op_e
constexpr unsigned kWordW     = 64;  // MOSAIC_XLEN
constexpr unsigned kLinkW     = 3;   // $clog2(kEntries)
constexpr unsigned kPipeDepth = 4;   // MOSAIC_REMOTE_LATENCY
constexpr unsigned kReqBodyW  = kIdW + kDstW + kOpW + 3 * kWordW;
constexpr unsigned kReqW      = kReqBodyW + kLinkW;
constexpr unsigned kRspW      = kIdW + kDstW + kWordW + 1 + kLinkW;

// Payload field offsets, low bits first, exactly as the RTL lays them out. The
// DUT reports the same numbers through o_*_lo and phase 0 requires the two to
// agree, so this is a cross-check and not a second authority.
constexpr unsigned kReqIdLo  = 0;
constexpr unsigned kReqDstLo = kReqIdLo + kIdW;
constexpr unsigned kReqOpLo  = kReqDstLo + kDstW;
constexpr unsigned kReqImmLo = kReqOpLo + kOpW;
constexpr unsigned kReqS1Lo  = kReqImmLo + kWordW;
constexpr unsigned kReqS2Lo  = kReqS1Lo + kWordW;
constexpr unsigned kReqLidLo = kReqS2Lo + kWordW;
constexpr unsigned kRspIdLo  = 0;
constexpr unsigned kRspDstLo = kRspIdLo + kIdW;
constexpr unsigned kRspValLo = kRspDstLo + kDstW;
constexpr unsigned kRspFltLo = kRspValLo + kWordW;
constexpr unsigned kRspLidLo = kRspFltLo + 1;

constexpr int kResetCycles = 4;
constexpr uint64_t kDrainBound = 64;  // cycles to empty a stopped link

// ------------------------------------------------------------------ payload
// A little-endian bit vector over 32-bit words: what a Verilator wide port is
// made of, and what the shadow builds when it compares against one.
class Payload {
 public:
  Payload() {}
  explicit Payload(unsigned nbits) : w_((nbits + 31) / 32, 0u) {}

  void Set(unsigned lo, unsigned width, uint64_t value) {
    for (unsigned b = 0; b < width; b++) {
      const unsigned bit = lo + b;
      const uint32_t mask = 1u << (bit % 32);
      if ((value >> b) & 1ull) {
        w_[bit / 32] |= mask;
      } else {
        w_[bit / 32] &= ~mask;
      }
    }
  }

  unsigned words() const { return static_cast<unsigned>(w_.size()); }
  const std::vector<uint32_t>& raw() const { return w_; }

  std::string str() const {
    std::string out;
    for (size_t i = w_.size(); i-- > 0;) {
      char buffer[16];
      std::snprintf(buffer, sizeof(buffer), "%08x", w_[i]);
      out += buffer;
    }
    return "0x" + out;
  }

 private:
  std::vector<uint32_t> w_;
};

uint64_t FieldAt(const std::vector<uint32_t>& words, unsigned lo, unsigned width) {
  uint64_t value = 0;
  for (unsigned b = 0; b < width; b++) {
    const unsigned bit = lo + b;
    if (bit / 32 >= words.size()) break;
    value |= static_cast<uint64_t>((words[bit / 32] >> (bit % 32)) & 1u) << b;
  }
  return value;
}

template <typename Wide>
void DriveWide(Wide& signal, const Payload& payload) {
  for (unsigned i = 0; i < payload.words(); i++) signal[i] = payload.raw()[i];
}

template <typename Wide>
std::vector<uint32_t> ReadWide(const Wide& signal, unsigned words) {
  std::vector<uint32_t> out(words, 0u);
  for (unsigned i = 0; i < words; i++) out[i] = signal[i];
  return out;
}

// ---------------------------------------------------------- what a uop is
// The request as fields. `lid` exists only on the wire between the clusters: it
// is allocated by the link, so the home side never supplies it.
struct LinkReq {
  uint64_t id = 0;
  uint64_t dst = 0;
  uint64_t op = 0;
  uint64_t imm = 0;
  uint64_t s1 = 0;
  uint64_t s2 = 0;
  unsigned lid = 0;
};

struct LinkRsp {
  uint64_t id = 0;
  uint64_t dst = 0;
  uint64_t val = 0;
  unsigned fault = 0;
  unsigned lid = 0;
};

Payload PackReqBody(const LinkReq& r) {
  Payload p(kReqBodyW);
  p.Set(kReqIdLo, kIdW, r.id);
  p.Set(kReqDstLo, kDstW, r.dst);
  p.Set(kReqOpLo, kOpW, r.op);
  p.Set(kReqImmLo, kWordW, r.imm);
  p.Set(kReqS1Lo, kWordW, r.s1);
  p.Set(kReqS2Lo, kWordW, r.s2);
  return p;
}

Payload PackRsp(const LinkRsp& r) {
  Payload p(kRspW);
  p.Set(kRspIdLo, kIdW, r.id);
  p.Set(kRspDstLo, kDstW, r.dst);
  p.Set(kRspValLo, kWordW, r.val);
  p.Set(kRspFltLo, 1, r.fault);
  p.Set(kRspLidLo, kLinkW, r.lid);
  return p;
}

LinkReq UnpackReq(const std::vector<uint32_t>& w) {
  LinkReq r;
  r.id  = FieldAt(w, kReqIdLo, kIdW);
  r.dst = FieldAt(w, kReqDstLo, kDstW);
  r.op  = FieldAt(w, kReqOpLo, kOpW);
  r.imm = FieldAt(w, kReqImmLo, kWordW);
  r.s1  = FieldAt(w, kReqS1Lo, kWordW);
  r.s2  = FieldAt(w, kReqS2Lo, kWordW);
  r.lid = static_cast<unsigned>(FieldAt(w, kReqLidLo, kLinkW));
  return r;
}

LinkRsp UnpackRsp(const std::vector<uint32_t>& w) {
  LinkRsp r;
  r.id    = FieldAt(w, kRspIdLo, kIdW);
  r.dst   = FieldAt(w, kRspDstLo, kDstW);
  r.val   = FieldAt(w, kRspValLo, kWordW);
  r.fault = static_cast<unsigned>(FieldAt(w, kRspFltLo, 1));
  r.lid   = static_cast<unsigned>(FieldAt(w, kRspLidLo, kLinkW));
  return r;
}

std::string ReqStr(const LinkReq& r) {
  return "{id=" + Dec(r.id) + " dst=" + Dec(r.dst) + " op=" + Dec(r.op) +
         " imm=" + Hex64(r.imm) + " s1=" + Hex64(r.s1) + " s2=" + Hex64(r.s2) +
         " lid=" + Dec(r.lid) + "}";
}

std::string RspStr(const LinkRsp& r) {
  return "{id=" + Dec(r.id) + " dst=" + Dec(r.dst) + " val=" + Hex64(r.val) +
         " fault=" + Dec(r.fault) + " lid=" + Dec(r.lid) + "}";
}

bool SameReq(const LinkReq& a, const LinkReq& b) {
  return a.id == b.id && a.dst == b.dst && a.op == b.op && a.imm == b.imm &&
         a.s1 == b.s1 && a.s2 == b.s2 && a.lid == b.lid;
}

bool SameRsp(const LinkRsp& a, const LinkRsp& b) {
  return a.id == b.id && a.dst == b.dst && a.val == b.val && a.fault == b.fault &&
         a.lid == b.lid;
}

// --------------------------------------------------------------- identity
// A uop identity, packed the way `mosaic_id_pkg::macro_id_t` is:
// {hart, rob_index, rob_gen, uop_index}. Destination: {gen, tag}.
constexpr unsigned kRobIndexW = 6, kRobGenW = 7, kUopIndexW = 3;
constexpr unsigned kTagW = 7;

uint64_t MakeId(uint32_t hart, uint32_t rob_index, uint32_t rob_gen, uint32_t uop_index) {
  return (static_cast<uint64_t>(hart & 1u) << (kRobIndexW + kRobGenW + kUopIndexW)) |
         (static_cast<uint64_t>(rob_index & 0x3fu) << (kRobGenW + kUopIndexW)) |
         (static_cast<uint64_t>(rob_gen & 0x7fu) << kUopIndexW) |
         static_cast<uint64_t>(uop_index & 0x7u);
}

uint64_t MakeDst(uint32_t tag, uint32_t gen) {
  return static_cast<uint64_t>(tag & 0x7fu) |
         (static_cast<uint64_t>(gen & 0xffu) << kTagW);
}

// A request whose fields differ from every other request the case builds, so a
// response delivered into the wrong slot shows up as an identity and a value
// that do not belong together.
LinkReq MakeReq(uint32_t index, uint64_t extra) {
  LinkReq r;
  r.id  = MakeId(0, (index * 7 + 3) & 0x3f, (0x40 + index) & 0x7f, index & 0x7);
  r.dst = MakeDst((index * 5 + 1) & 0x7f, (0x20 + index) & 0xff);
  r.op  = index % 16;
  r.imm = 0x1000ull + index * 0x111ull + extra;
  r.s1  = 0x2000ull + index * 0x222ull + extra;
  r.s2  = 0x3000ull + index * 0x333ull + extra;
  return r;
}

// The remote unit's arithmetic. The opcode encoding is `mosaic_pkg::alu_op_e`,
// the only thing in the payload the far side is allowed to interpret.
uint64_t RemoteValue(const LinkReq& r) {
  switch (r.op) {
    case 0:  return r.s1 + r.s2;           // ALU_ADD
    case 1:  return r.s1 - r.s2;           // ALU_SUB
    case 2:  return r.s1 << (r.s2 & 63);   // ALU_SLL
    case 5:  return r.s1 ^ r.s2;           // ALU_XOR
    case 6:  return r.s1 >> (r.s2 & 63);   // ALU_SRL
    case 8:  return r.s1 | r.s2;           // ALU_OR
    case 9:  return r.s1 & r.s2;           // ALU_AND
    default: return (r.s1 + r.imm) ^ (r.s2 & 0xffull);
  }
}

// --------------------------------------------------------------- stimulus
struct Stim {
  bool req_valid = false;
  LinkReq req{};
  bool rem_req_ready = false;
  bool rem_rsp_valid = false;
  LinkRsp rsp{};
  bool rsp_ready = false;
  bool kill_valid = false;
  uint64_t kill_id = 0;
  bool flush_valid = false;

  std::string str() const {
    return "[req=" + Bool(req_valid) + ReqStr(req) + " rreq_rdy=" + Bool(rem_req_ready) +
           " rsp=" + Bool(rem_rsp_valid) + RspStr(rsp) + " rsp_rdy=" + Bool(rsp_ready) +
           " kill=" + Bool(kill_valid) + ":" + Dec(kill_id) + " flush=" +
           Bool(flush_valid) + "]";
  }
};

struct EntryState {
  bool valid = false;
  uint64_t id = 0;
  uint64_t dst = 0;
};

struct Stage {
  bool valid = false;
  LinkReq req{};
  LinkRsp rsp{};
};

struct Predict {
  // before the edge
  bool req_ready = false;
  bool rem_req_valid = false;
  LinkReq rem_req{};
  bool rem_rsp_ready = false;
  bool rsp_valid = false;
  LinkRsp rsp{};
  bool alloc_free = false;
  // after the edge
  std::vector<EntryState> entries;
  std::vector<Stage> req_pipe;
  std::vector<Stage> rsp_pipe;
  uint64_t issued = 0, returned = 0, matched = 0, killed = 0, outstanding = 0;
  uint64_t stale = 0, alias = 0, delivered = 0, kill_missed = 0, flushes = 0;
  uint64_t req_stall = 0, rsp_stall = 0;
  // events of the edge
  bool ev_accept = false;
  bool ev_matched = false;
  bool ev_killed = false;
  bool ev_flush = false;
  bool ev_stale = false;
  bool ev_alias = false;
  bool ev_delivered = false;
  int ev_kill_slot = -1;
};

// ------------------------------------------------------------- the shadow
class Shadow {
 public:
  Shadow() {
    entries_.resize(kEntries);
    req_pipe_.resize(kPipeDepth);
    rsp_pipe_.resize(kPipeDepth);
    Reset();
  }

  void Reset() {
    for (unsigned k = 0; k < kEntries; k++) entries_[k] = EntryState{};
    for (unsigned s = 0; s < kPipeDepth; s++) {
      req_pipe_[s] = Stage{};
      rsp_pipe_[s] = Stage{};
    }
    issued_ = returned_ = matched_ = killed_ = outstanding_ = 0;
    stale_ = alias_ = delivered_ = kill_missed_ = flushes_ = 0;
    req_stall_ = rsp_stall_ = 0;
  }

  Predict Step(const Stim& s) {
    Predict r;
    r.entries.resize(kEntries);
    r.req_pipe.resize(kPipeDepth);
    r.rsp_pipe.resize(kPipeDepth);

    // ---- what the module presents before the edge
    const bool req_shift = !req_pipe_[kPipeDepth - 1].valid || s.rem_req_ready;
    const bool rsp_shift = !rsp_pipe_[kPipeDepth - 1].valid || s.rsp_ready;
    bool alloc_free = false;
    for (unsigned k = 0; k < kEntries; k++) alloc_free = alloc_free || !entries_[k].valid;

    r.rem_req_valid = req_pipe_[kPipeDepth - 1].valid;
    r.rem_req       = req_pipe_[kPipeDepth - 1].req;
    r.rem_rsp_ready = rsp_shift;
    r.rsp_valid     = rsp_pipe_[kPipeDepth - 1].valid;
    r.rsp           = rsp_pipe_[kPipeDepth - 1].rsp;
    r.req_ready     = alloc_free && req_shift;
    r.alloc_free    = alloc_free;

    const bool req_accept = s.req_valid && r.req_ready;
    const bool rsp_xfer   = s.rem_rsp_valid && r.rem_rsp_ready;
    r.ev_accept = req_accept;

    // ---- the kill, lowest index first, exactly as documented
    int kill_hit = -1;
    if (s.kill_valid && !s.flush_valid) {
      for (unsigned k = 0; k < kEntries; k++) {
        if (entries_[k].valid && entries_[k].id == s.kill_id) {
          kill_hit = static_cast<int>(k);
          break;
        }
      }
    }
    r.ev_kill_slot = kill_hit;
    r.ev_killed = kill_hit >= 0;

    // ---- the allocation scan: lowest invalid entry
    unsigned alloc_slot = 0;
    for (unsigned k = 0; k < kEntries; k++) {
      if (!entries_[k].valid) {
        alloc_slot = k;
        break;
      }
    }

    // ---- one pass decides release, match, alias and the counters
    unsigned freed = 0;
    bool any_match = false;
    bool any_alias = false;
    bool any_flush = false;
    for (unsigned k = 0; k < kEntries; k++) {
      const bool flush_win = s.flush_valid && entries_[k].valid;
      const bool kill_win  = (kill_hit == static_cast<int>(k)) && entries_[k].valid;
      const bool match_win = rsp_xfer && !flush_win && !kill_win &&
                             (s.rsp.lid == k) && entries_[k].valid &&
                             (entries_[k].id == s.rsp.id) && (entries_[k].dst == s.rsp.dst);
      if (flush_win || kill_win || match_win) freed++;
      if (match_win) any_match = true;
      if (flush_win) any_flush = true;
      // An alias is a response that named a *live* entry and disagreed with it.
      // An entry destroyed by this cycle's kill or flush is not an alias: those
      // are separate reasons, reported by separate counters, and conflating
      // them would hide which one happened.
      if (rsp_xfer && !match_win && entries_[k].valid && (s.rsp.lid == k) &&
          kill_hit != static_cast<int>(k) && !s.flush_valid) {
        any_alias = true;
      }
    }
    r.ev_matched = any_match;
    r.ev_flush = any_flush;
    r.ev_stale = rsp_xfer && !any_match;
    r.ev_alias = any_alias;

    // ---- post-edge
    for (unsigned k = 0; k < kEntries; k++) r.entries[k] = entries_[k];
    for (unsigned k = 0; k < kEntries; k++) {
      const bool flush_win = s.flush_valid && entries_[k].valid;
      const bool kill_win  = (kill_hit == static_cast<int>(k)) && entries_[k].valid;
      const bool match_win = rsp_xfer && !flush_win && !kill_win &&
                             (s.rsp.lid == k) && entries_[k].valid &&
                             (entries_[k].id == s.rsp.id) && (entries_[k].dst == s.rsp.dst);
      if (flush_win || kill_win || match_win) r.entries[k].valid = false;
    }
    if (req_accept) {
      r.entries[alloc_slot].valid = true;
      r.entries[alloc_slot].id = s.req.id;
      r.entries[alloc_slot].dst = s.req.dst;
    }

    for (unsigned st = 0; st < kPipeDepth; st++) {
      r.req_pipe[st] = req_pipe_[st];
      r.rsp_pipe[st] = rsp_pipe_[st];
    }
    if (req_shift) {
      for (unsigned st = kPipeDepth - 1; st > 0; st--) r.req_pipe[st] = req_pipe_[st - 1];
      LinkReq item = s.req;
      item.lid = alloc_slot;
      r.req_pipe[0].valid = req_accept;
      r.req_pipe[0].req = item;
    }
    if (rsp_shift) {
      for (unsigned st = kPipeDepth - 1; st > 0; st--) r.rsp_pipe[st] = rsp_pipe_[st - 1];
      r.rsp_pipe[0].valid = any_match;
      r.rsp_pipe[0].rsp = s.rsp;
    }

    // ---- the counters, from the same events
    r.issued      = issued_ + (req_accept ? 1 : 0);
    r.matched     = matched_ + (any_match ? 1 : 0);
    r.killed      = killed_ + freed - (any_match ? 1 : 0);
    r.returned    = returned_ + freed;
    r.outstanding = outstanding_ + (req_accept ? 1 : 0) - freed;
    r.stale       = stale_ + (r.ev_stale ? 1 : 0);
    r.alias       = alias_ + (any_alias ? 1 : 0);
    r.delivered   = delivered_ + ((r.rsp_valid && s.rsp_ready) ? 1 : 0);
    r.kill_missed = kill_missed_ +
                    ((s.kill_valid && !s.flush_valid && kill_hit < 0) ? 1 : 0);
    r.flushes     = flushes_ + ((s.flush_valid && freed != 0) ? 1 : 0);
    r.req_stall   = req_stall_ + ((s.req_valid && !r.req_ready) ? 1 : 0);
    r.rsp_stall   = rsp_stall_ + ((s.rem_rsp_valid && !r.rem_rsp_ready) ? 1 : 0);
    r.ev_delivered = r.rsp_valid && s.rsp_ready;

    entries_ = r.entries;
    req_pipe_ = r.req_pipe;
    rsp_pipe_ = r.rsp_pipe;
    issued_ = r.issued;
    returned_ = r.returned;
    matched_ = r.matched;
    killed_ = r.killed;
    outstanding_ = r.outstanding;
    stale_ = r.stale;
    alias_ = r.alias;
    delivered_ = r.delivered;
    kill_missed_ = r.kill_missed;
    flushes_ = r.flushes;
    req_stall_ = r.req_stall;
    rsp_stall_ = r.rsp_stall;
    return r;
  }

  const std::vector<EntryState>& entries() const { return entries_; }
  const std::vector<Stage>& req_pipe() const { return req_pipe_; }
  const std::vector<Stage>& rsp_pipe() const { return rsp_pipe_; }

 private:
  std::vector<EntryState> entries_;
  std::vector<Stage> req_pipe_;
  std::vector<Stage> rsp_pipe_;
  uint64_t issued_ = 0, returned_ = 0, matched_ = 0, killed_ = 0, outstanding_ = 0;
  uint64_t stale_ = 0, alias_ = 0, delivered_ = 0, kill_missed_ = 0, flushes_ = 0;
  uint64_t req_stall_ = 0, rsp_stall_ = 0;
};

// -------------------------------------------------------- the remote unit
// One response port, several results in flight. A result becomes ready after
// its own delay, and the unit presents the ready one with the smallest fire
// cycle, so responses can leave in an order that is not the order the requests
// arrived in -- the property the link must not assume away.
class Remote {
 public:
  struct Fire {
    uint64_t cycle = 0;
    uint64_t accepted_cycle = 0;
    LinkRsp rsp{};
  };

  void Reset() {
    pending_.clear();
    per_id_.clear();
    fires_.clear();
    holding_ = false;
    held_index_ = 0;
    held_ = LinkRsp{};
    accepted_ = 0;
    emitted_ = 0;
    delay_ = 1;
  }

  void SetDelay(unsigned delay) { delay_ = delay; }
  void SetDelayFor(uint64_t id, unsigned delay) { per_id_[id] = delay; }

  bool Offer(LinkRsp* out) const {
    if (!holding_) return false;
    *out = held_;
    return true;
  }

  void Accept(uint64_t now, const LinkReq& req) {
    Pending p;
    p.accepted_cycle = now;
    p.req = req;
    unsigned delay = delay_;
    auto it = per_id_.find(req.id);
    if (it != per_id_.end()) delay = it->second;
    p.fire_cycle = now + delay;
    pending_.push_back(p);
    accepted_++;
  }

  void Emitted() {
    if (!holding_) return;
    holding_ = false;
    emitted_++;
    pending_.erase(pending_.begin() + static_cast<long>(held_index_));
    held_index_ = 0;
  }

  void Advance(uint64_t now) {
    if (holding_) return;
    int best = -1;
    for (size_t i = 0; i < pending_.size(); i++) {
      if (pending_[i].fire_cycle > now) continue;
      if (best < 0 || pending_[i].fire_cycle <
                          pending_[static_cast<size_t>(best)].fire_cycle) {
        best = static_cast<int>(i);
      }
    }
    if (best < 0) return;
    holding_ = true;
    held_index_ = static_cast<size_t>(best);
    LinkRsp r;
    r.id = pending_[held_index_].req.id;
    r.dst = pending_[held_index_].req.dst;
    r.val = RemoteValue(pending_[held_index_].req);
    r.fault = 0;
    r.lid = pending_[held_index_].req.lid;
    held_ = r;
    Fire f;
    f.cycle = now;
    f.accepted_cycle = pending_[held_index_].accepted_cycle;
    f.rsp = r;
    fires_.push_back(f);
  }

  struct Pending {
    uint64_t fire_cycle = 0;
    uint64_t accepted_cycle = 0;
    LinkReq req{};
  };

  unsigned pending_count() const { return static_cast<unsigned>(pending_.size()); }
  bool holding() const { return holding_; }
  uint64_t accepted() const { return accepted_; }
  uint64_t emitted() const { return emitted_; }
  const std::vector<Fire>& fires() const { return fires_; }

 private:
  std::vector<Pending> pending_;
  std::map<uint64_t, unsigned> per_id_;
  std::vector<Fire> fires_;
  bool holding_ = false;
  size_t held_index_ = 0;
  LinkRsp held_{};
  unsigned delay_ = 1;
  uint64_t accepted_ = 0;
  uint64_t emitted_ = 0;
};

// ---------------------------------------------------------------- coverage
struct Coverage {
  uint64_t accepts = 0, matches = 0, kills = 0, flushes = 0, stale = 0, alias = 0;
  uint64_t delivered = 0, kill_missed = 0, req_stalls = 0, rsp_stalls = 0;
  uint64_t table_full = 0, reply_pipe_full = 0, ooo_delivery = 0;
  uint64_t kill_vs_response = 0;
};

// ---------------------------------------------------------------- harness
class Harness {
 public:
  Harness(Vmosaic_remote_link_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles)
      : dut_(dut), clk_(clk), max_cycles_(max_cycles) {}

  void Phase(const std::string& name) { phase_ = name; }

  Vmosaic_remote_link_tb* dut() { return dut_; }
  Shadow* shadow() { return &shadow_; }
  Remote* remote() { return &remote_; }
  Coverage* coverage() { return &coverage_; }
  uint64_t cycles() const { return cycles_; }

  // The index of the cycle that has just been driven. `cycles()` counts
  // *completed* clock periods, so the cycle being checked is always one behind
  // it; phases that reason about a specific cycle use this and not `cycles()`.
  uint64_t driven() const { return cycles_ - 1; }

  // True when nothing is left anywhere: no credit taken, no stage occupied, and
  // nothing in the remote unit. Reading only the credit counter is not enough
  // -- a matched response still in the response pipeline holds no credit but is
  // still on its way, and a "drained" claim that ignored it would be false.
  bool drained() const {
    if (dut_->o_outstanding != 0) return false;
    for (unsigned st = 0; st < kPipeDepth; st++) {
      if (req_pipe_valid_[st] || rsp_pipe_valid_[st]) return false;
    }
    return !remote_.holding() && remote_.pending_count() == 0;
  }

  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) Cycle(Stim{}, /*rst=*/true);
    shadow_.Reset();
    remote_.Reset();
    // The stability memory belongs to the cycle stream, not to the model: a
    // reset ends the stream it was watching.
    prev_req_stalled_ = false;
    prev_rsp_stalled_ = false;
  }

  // What the DUT presented this cycle, read before the edge.
  struct Seen {
    bool req_ready = false;
    bool rem_req_valid = false;
    LinkReq rem_req{};
    // The response port facing the remote unit: an input of the link, read back
    // so a check can name what the far side offered.
    bool rem_rsp_valid = false;
    LinkRsp rem_rsp{};
    // The response port facing the home side: an output of the link.
    bool rem_rsp_ready = false;
    bool rsp_valid = false;
    LinkRsp rsp{};
  };

  const Seen& seen() const { return seen_; }

  // Post-edge state as read back from the observation ports.
  const std::vector<bool>& entry_valid() const { return entry_valid_; }
  const std::vector<uint64_t>& entry_id() const { return entry_id_; }
  const std::vector<uint64_t>& entry_dst() const { return entry_dst_; }
  const std::vector<bool>& req_pipe_valid() const { return req_pipe_valid_; }
  const std::vector<bool>& rsp_pipe_valid() const { return rsp_pipe_valid_; }
  const std::vector<LinkReq>& req_pipe() const { return req_pipe_; }
  const std::vector<LinkRsp>& rsp_pipe() const { return rsp_pipe_; }

  uint64_t dut_issued() const { return dut_->o_issued_ctr; }
  uint64_t dut_returned() const { return dut_->o_returned_ctr; }
  uint64_t dut_matched() const { return dut_->o_matched_ctr; }
  uint64_t dut_killed() const { return dut_->o_killed_ctr; }
  uint64_t dut_outstanding() const { return dut_->o_outstanding; }
  uint64_t dut_stale() const { return dut_->o_stale_rsp_ctr; }
  uint64_t dut_alias() const { return dut_->o_alias_rsp_ctr; }
  uint64_t dut_delivered() const { return dut_->o_delivered_ctr; }
  uint64_t dut_kill_missed() const { return dut_->o_kill_missed_ctr; }
  uint64_t dut_flushes() const { return dut_->o_flush_ctr; }

  // Run one cycle with the remote unit attached: its offer becomes the response
  // input, so the phase code only has to say what the *link's* environment
  // does (back-pressure, kills, requests).
  Predict Run(const Stim& in) {
    Stim s = in;
    LinkRsp offered;
    s.rem_rsp_valid = remote_.Offer(&offered);
    s.rsp = offered;
    Predict p = Cycle(s);
    return p;
  }

  Predict Cycle(const Stim& s, bool rst = false) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles (" + Dec(max_cycles_) +
                       ") exhausted before the campaign finished");
    }

    dut_->clk = 0;
    dut_->rst = rst ? 1 : 0;
    dut_->req_valid = s.req_valid ? 1 : 0;
    DriveWide(dut_->req_payload, PackReqBody(s.req));
    dut_->rem_req_ready = s.rem_req_ready ? 1 : 0;
    dut_->rem_rsp_valid = s.rem_rsp_valid ? 1 : 0;
    DriveWide(dut_->rem_rsp_payload, PackRsp(s.rsp));
    dut_->rsp_ready = s.rsp_ready ? 1 : 0;
    dut_->kill_valid = s.kill_valid ? 1 : 0;
    dut_->kill_ident = static_cast<uint32_t>(s.kill_id & ((1ull << kIdW) - 1));
    dut_->flush_valid = s.flush_valid ? 1 : 0;
    dut_->eval();

    // Read the DUT's combinational answer first: the payload-stability check
    // below compares *this* cycle's offer against the previous cycle's, and it
    // can only do that if the observation has already been taken.
    ReadSeen();

    Predict pred;
    if (!rst) {
      pred = shadow_.Step(s);
      CompareOutputs(s, pred);
    }

    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    const uint64_t this_cycle = cycles_;
    ++cycles_;

    ReadState();

    if (!rst) {
      // The remote unit is clocked by the same edge. `now` is the index of the
      // cycle the transfer happened in, so a request taken at the end of cycle
      // N is offered back with delay D during cycle N+D -- "D cycles after the
      // far side consumed it" is then the literal meaning of the injected
      // delay, and the phase assertions are statements about the hardware
      // rather than about this file's arithmetic.
      if (seen_.rem_req_valid && s.rem_req_ready) remote_.Accept(this_cycle, seen_.rem_req);
      // The unit gives up the result it was holding when the *link* took it,
      // which is the remote response handshake -- not the home delivery, which
      // happens several cycles later and may never happen at all for a stale
      // response that the link consumes and drops.
      if (seen_.rem_rsp_valid && seen_.rem_rsp_ready) remote_.Emitted();
      remote_.Advance(this_cycle + 1);

      // The order decides which check names a defect first. The invariants run
      // before the state comparison because they are the directed statements --
      // "an unmatched response was not written anywhere" -- and a mismatch
      // there names the mechanism, where the state comparison would name the
      // entry that changed as a consequence.
      CheckInvariants(s, pred);
      CompareCounters(pred, s);
      CompareState(pred, s);
      AccumulateCoverage(s, pred);
      ++comparisons_;
    }
    return pred;
  }

  uint64_t comparisons() const { return comparisons_; }

 private:
  void ReadSeen() {
    seen_.req_ready = dut_->req_ready != 0;
    seen_.rem_req_valid = dut_->rem_req_valid != 0;
    seen_.rem_req = UnpackReq(ReadWide(dut_->rem_req_payload, (kReqW + 31) / 32));
    seen_.rem_rsp_valid = dut_->rem_rsp_valid != 0;
    seen_.rem_rsp = UnpackRsp(ReadWide(dut_->rem_rsp_payload, (kRspW + 31) / 32));
    seen_.rem_rsp_ready = dut_->rem_rsp_ready != 0;
    seen_.rsp_valid = dut_->rsp_valid != 0;
    seen_.rsp = UnpackRsp(ReadWide(dut_->rsp_payload, (kRspW + 31) / 32));
  }

  void ReadState() {
    entry_valid_.resize(kEntries);
    entry_id_.resize(kEntries);
    entry_dst_.resize(kEntries);
    const std::vector<uint32_t> ids = ReadWide(dut_->o_entry_id, (kEntries * kIdW + 31) / 32);
    const std::vector<uint32_t> dsts = ReadWide(dut_->o_entry_dst, (kEntries * kDstW + 31) / 32);
    for (unsigned k = 0; k < kEntries; k++) {
      entry_valid_[k] = ((dut_->o_entry_valid >> k) & 1u) != 0;
      entry_id_[k] = FieldAt(ids, k * kIdW, kIdW);
      entry_dst_[k] = FieldAt(dsts, k * kDstW, kDstW);
    }

    req_pipe_valid_.resize(kPipeDepth);
    req_pipe_.resize(kPipeDepth);
    rsp_pipe_valid_.resize(kPipeDepth);
    rsp_pipe_.resize(kPipeDepth);
    const std::vector<uint32_t> rp =
        ReadWide(dut_->o_req_pipe, (kPipeDepth * kReqW + 31) / 32);
    const std::vector<uint32_t> sp =
        ReadWide(dut_->o_rsp_pipe, (kPipeDepth * kRspW + 31) / 32);
    for (unsigned st = 0; st < kPipeDepth; st++) {
      req_pipe_valid_[st] = ((dut_->o_req_pipe_valid >> st) & 1u) != 0;
      req_pipe_[st] = UnpackReq(SliceAt(rp, st * kReqW));
      rsp_pipe_valid_[st] = ((dut_->o_rsp_pipe_valid >> st) & 1u) != 0;
      rsp_pipe_[st] = UnpackRsp(SliceAt(sp, st * kRspW));
    }
  }

  // A view of `words` shifted down by `lo` bits, so the field extractors can be
  // reused on one pipeline stage.
  static std::vector<uint32_t> SliceAt(const std::vector<uint32_t>& words, unsigned lo) {
    std::vector<uint32_t> out(words.size() + 1, 0u);
    const unsigned word = lo / 32;
    const unsigned rem = lo % 32;
    for (unsigned i = 0; i < out.size(); i++) {
      if (word + i >= words.size()) break;
      uint32_t value = words[word + i] >> rem;
      if (rem != 0 && word + i + 1 < words.size()) {
        value |= words[word + i + 1] << (32 - rem);
      }
      out[i] = value;
    }
    return out;
  }

  void CompareOutputs(const Stim& s, const Predict& e) {
    const std::string where = phase_ + ": cycle " + Dec(clk_->cycle()) + " " + s.str();
    Require(dut_->req_ready == (e.req_ready ? 1 : 0), where,
            "req_ready: expected " + Bool(e.req_ready) + ", got " +
                Bool(dut_->req_ready != 0));
    Require(dut_->rem_req_valid == (e.rem_req_valid ? 1 : 0), where,
            "rem_req_valid: expected " + Bool(e.rem_req_valid) + ", got " +
                Bool(dut_->rem_req_valid != 0));
    Require(dut_->rem_rsp_ready == (e.rem_rsp_ready ? 1 : 0), where,
            "rem_rsp_ready: expected " + Bool(e.rem_rsp_ready) + ", got " +
                Bool(dut_->rem_rsp_ready != 0));
    Require(dut_->rsp_valid == (e.rsp_valid ? 1 : 0), where,
            "rsp_valid: expected " + Bool(e.rsp_valid) + ", got " +
                Bool(dut_->rsp_valid != 0));

    if (e.rem_req_valid) {
      const LinkReq got = UnpackReq(ReadWide(dut_->rem_req_payload, (kReqW + 31) / 32));
      Require(SameReq(got, e.rem_req), where,
              "rem_req_payload: expected " + ReqStr(e.rem_req) + ", got " + ReqStr(got));
    }
    if (e.rsp_valid) {
      const LinkRsp got = UnpackRsp(ReadWide(dut_->rsp_payload, (kRspW + 31) / 32));
      Require(SameRsp(got, e.rsp), where,
              "rsp_payload: expected " + RspStr(e.rsp) + ", got " + RspStr(got));
    }

    // Payload stability: an item that was offered and not taken must be
    // re-presented unchanged. Checked on the DUT's own wires, across the two
    // snapshots, because that is the property a consumer relies on.
    if (prev_req_stalled_) {
      Require(SameReq(seen_.rem_req, prev_req_), where,
              "the request offered while stalled changed on the wire: was " +
                  ReqStr(prev_req_) + ", now " + ReqStr(seen_.rem_req));
    }
    if (prev_rsp_stalled_) {
      Require(SameRsp(seen_.rsp, prev_rsp_), where,
              "the response offered while stalled changed on the wire: was " +
                  RspStr(prev_rsp_) + ", now " + RspStr(seen_.rsp));
    }
    prev_req_stalled_ = e.rem_req_valid && !s.rem_req_ready;
    prev_req_ = seen_.rem_req;
    prev_rsp_stalled_ = e.rsp_valid && !s.rsp_ready;
    prev_rsp_ = seen_.rsp;
  }

  void CompareState(const Predict& e, const Stim& s) {
    const std::string where =
        phase_ + ": cycle " + Dec(clk_->cycle()) + " post-edge " + s.str();
    for (unsigned k = 0; k < kEntries; k++) {
      Require(entry_valid_[k] == e.entries[k].valid, where,
              "entry " + Dec(k) + " validity: expected " + Bool(e.entries[k].valid) +
                  ", got " + Bool(entry_valid_[k]));
      if (e.entries[k].valid) {
        Require(entry_id_[k] == e.entries[k].id, where,
                "entry " + Dec(k) + " identity: expected " + Dec(e.entries[k].id) +
                    ", got " + Dec(entry_id_[k]));
        Require(entry_dst_[k] == e.entries[k].dst, where,
                "entry " + Dec(k) + " destination: expected " + Dec(e.entries[k].dst) +
                    ", got " + Dec(entry_dst_[k]));
      }
    }
    for (unsigned st = 0; st < kPipeDepth; st++) {
      Require(req_pipe_valid_[st] == e.req_pipe[st].valid, where,
              "request pipe stage " + Dec(st) + " validity: expected " +
                  Bool(e.req_pipe[st].valid) + ", got " + Bool(req_pipe_valid_[st]));
      if (e.req_pipe[st].valid) {
        Require(SameReq(req_pipe_[st], e.req_pipe[st].req), where,
                "request pipe stage " + Dec(st) + ": expected " +
                    ReqStr(e.req_pipe[st].req) + ", got " + ReqStr(req_pipe_[st]));
      }
      Require(rsp_pipe_valid_[st] == e.rsp_pipe[st].valid, where,
              "response pipe stage " + Dec(st) + " validity: expected " +
                  Bool(e.rsp_pipe[st].valid) + ", got " + Bool(rsp_pipe_valid_[st]));
      if (e.rsp_pipe[st].valid) {
        Require(SameRsp(rsp_pipe_[st], e.rsp_pipe[st].rsp), where,
                "response pipe stage " + Dec(st) + ": expected " +
                    RspStr(e.rsp_pipe[st].rsp) + ", got " + RspStr(rsp_pipe_[st]));
      }
    }
  }

  void CompareCounters(const Predict& e, const Stim& s) {
    const std::string where =
        phase_ + ": cycle " + Dec(clk_->cycle()) + " post-edge " + s.str();
    auto need = [&](uint64_t got, uint64_t want, const char* name) {
      Require(got == want, where,
              std::string(name) + ": expected " + Dec(want) + ", got " + Dec(got));
    };
    need(dut_->o_issued_ctr, e.issued, "o_issued_ctr");
    need(dut_->o_returned_ctr, e.returned, "o_returned_ctr");
    need(dut_->o_matched_ctr, e.matched, "o_matched_ctr");
    need(dut_->o_killed_ctr, e.killed, "o_killed_ctr");
    need(dut_->o_outstanding, e.outstanding, "o_outstanding");
    need(dut_->o_stale_rsp_ctr, e.stale, "o_stale_rsp_ctr");
    need(dut_->o_alias_rsp_ctr, e.alias, "o_alias_rsp_ctr");
    need(dut_->o_delivered_ctr, e.delivered, "o_delivered_ctr");
    need(dut_->o_kill_missed_ctr, e.kill_missed, "o_kill_missed_ctr");
    need(dut_->o_flush_ctr, e.flushes, "o_flush_ctr");
    need(dut_->o_req_stall_ctr, e.req_stall, "o_req_stall_ctr");
    need(dut_->o_rsp_stall_ctr, e.rsp_stall, "o_rsp_stall_ctr");
  }

  void CheckInvariants(const Stim& s, const Predict& e) {
    const std::string where =
        phase_ + ": cycle " + Dec(clk_->cycle()) + " post-edge " + s.str();

    uint64_t live = 0;
    for (unsigned k = 0; k < kEntries; k++) live += entry_valid_[k] ? 1 : 0;
    Require(dut_->o_outstanding == live, where,
            "o_outstanding (" + Dec(dut_->o_outstanding) +
                ") is not the population of o_entry_valid (" + Dec(live) + ")");

    Require(dut_->o_issued_ctr == dut_->o_returned_ctr + dut_->o_outstanding, where,
            "credit conservation broken: issued " + Dec(dut_->o_issued_ctr) +
                " != returned " + Dec(dut_->o_returned_ctr) + " + outstanding " +
                Dec(dut_->o_outstanding));
    Require(dut_->o_returned_ctr == dut_->o_matched_ctr + dut_->o_killed_ctr, where,
            "returned " + Dec(dut_->o_returned_ctr) + " != matched " +
                Dec(dut_->o_matched_ctr) + " + killed " + Dec(dut_->o_killed_ctr));
    Require(dut_->o_stale_rsp_ctr >= dut_->o_alias_rsp_ctr, where,
            "o_alias_rsp_ctr (" + Dec(dut_->o_alias_rsp_ctr) +
                ") is not a subset of o_stale_rsp_ctr (" + Dec(dut_->o_stale_rsp_ctr) + ")");

    // The card's central rule, on the DUT's own wires: a response that matches
    // no live entry is consumed and *dropped*. If it were loaded into the
    // response pipeline it would be delivered, which is the stale response
    // writing into a slot it does not own.
    if (s.rem_rsp_valid && seen_.rem_rsp_ready && !e.ev_matched) {
      Require(!(rsp_pipe_valid_[0] && SameRsp(rsp_pipe_[0], seen_.rem_rsp)), where,
              "a response that matched no live entry was loaded into the response "
              "pipeline and would be delivered: " + RspStr(seen_.rem_rsp));
    }
    // And the converse: a response that the model says matched must have been
    // consumed rather than dropped.
    if (s.rem_rsp_valid && seen_.rem_rsp_ready && e.ev_matched) {
      Require(rsp_pipe_valid_[0] && SameRsp(rsp_pipe_[0], seen_.rem_rsp), where,
              "a response that matched a live entry was not loaded into the response "
              "pipeline unchanged, so its result is lost: " + RspStr(seen_.rem_rsp));
    }
  }

  void AccumulateCoverage(const Stim& s, const Predict& e) {
    if (e.ev_accept) coverage_.accepts++;
    if (e.ev_matched) coverage_.matches++;
    if (e.ev_killed && !s.flush_valid) coverage_.kills++;
    if (e.ev_flush) coverage_.flushes++;
    if (e.ev_stale) coverage_.stale++;
    if (e.ev_alias) coverage_.alias++;
    if (e.ev_delivered) coverage_.delivered++;
    if (s.kill_valid && !s.flush_valid && !e.ev_killed) coverage_.kill_missed++;
    if (s.req_valid && !e.req_ready) coverage_.req_stalls++;
    if (s.rem_rsp_valid && !e.rem_rsp_ready) coverage_.rsp_stalls++;
    if (!e.alloc_free) coverage_.table_full++;
    if (s.rem_rsp_valid && !e.rem_rsp_ready) coverage_.reply_pipe_full++;
    if (e.ev_killed && e.ev_stale) coverage_.kill_vs_response++;
  }

  Vmosaic_remote_link_tb* dut_;
  mosaic::ClockDriver* clk_;
  uint64_t max_cycles_;
  Shadow shadow_;
  Remote remote_;
  Coverage coverage_;
  Seen seen_;
  bool prev_req_stalled_ = false;
  LinkReq prev_req_{};
  bool prev_rsp_stalled_ = false;
  LinkRsp prev_rsp_{};
  std::vector<bool> entry_valid_;
  std::vector<uint64_t> entry_id_;
  std::vector<uint64_t> entry_dst_;
  std::vector<bool> req_pipe_valid_;
  std::vector<bool> rsp_pipe_valid_;
  std::vector<LinkReq> req_pipe_;
  std::vector<LinkRsp> rsp_pipe_;
  std::string phase_ = "init";
  uint64_t comparisons_ = 0;
  uint64_t cycles_ = 0;
};

// ------------------------------------------------------------------ phases

void Fresh(Harness* h, const std::string& name) {
  h->Phase(name);
  h->Reset(kResetCycles);
}

// Phase 0: the elaborated geometry is the one this file assumes, and the two
// files agree about the wire layout.
void PhaseGeometry(Harness* h, mosaic::Reporter* reporter) {
  Fresh(h, "geometry");
  Vmosaic_remote_link_tb* d = h->dut();
  const std::string w = "geometry";
  Require(d->o_entries == kEntries, w,
          "o_entries " + Dec(d->o_entries) + " != " + Dec(kEntries));
  Require(d->o_latency == kPipeDepth, w,
          "o_latency " + Dec(d->o_latency) + " != " + Dec(kPipeDepth));
  Require(d->o_pipe_depth == kPipeDepth, w,
          "o_pipe_depth " + Dec(d->o_pipe_depth) + " != " + Dec(kPipeDepth));
  Require(d->o_id_w == kIdW, w, "o_id_w " + Dec(d->o_id_w) + " != " + Dec(kIdW));
  Require(d->o_dst_w == kDstW, w, "o_dst_w " + Dec(d->o_dst_w) + " != " + Dec(kDstW));
  Require(d->o_op_w == kOpW, w, "o_op_w " + Dec(d->o_op_w) + " != " + Dec(kOpW));
  Require(d->o_word_w == kWordW, w, "o_word_w " + Dec(d->o_word_w) + " != " + Dec(kWordW));
  Require(d->o_link_id_w == kLinkW, w,
          "o_link_id_w " + Dec(d->o_link_id_w) + " != " + Dec(kLinkW));
  Require(d->o_req_body_w == kReqBodyW, w,
          "o_req_body_w " + Dec(d->o_req_body_w) + " != " + Dec(kReqBodyW));
  Require(d->o_req_w == kReqW, w, "o_req_w " + Dec(d->o_req_w) + " != " + Dec(kReqW));
  Require(d->o_rsp_w == kRspW, w, "o_rsp_w " + Dec(d->o_rsp_w) + " != " + Dec(kRspW));
  Require(d->o_cnt_w == 32, w, "o_cnt_w " + Dec(d->o_cnt_w) + " != 32");

  // The layout, from the RTL's own parameter list, against the offsets this
  // file packs with. A disagreement is the difference between a test and a
  // coincidence.
  Require(d->o_req_id_lo == kReqIdLo, w, "request identity offset");
  Require(d->o_req_dst_lo == kReqDstLo, w, "request destination offset");
  Require(d->o_req_op_lo == kReqOpLo, w, "request opcode offset");
  Require(d->o_req_imm_lo == kReqImmLo, w, "request immediate offset");
  Require(d->o_req_s1_lo == kReqS1Lo, w, "request source 1 offset");
  Require(d->o_req_s2_lo == kReqS2Lo, w, "request source 2 offset");
  Require(d->o_req_lid_lo == kReqLidLo, w, "request link id offset");
  Require(d->o_rsp_id_lo == kRspIdLo, w, "response identity offset");
  Require(d->o_rsp_dst_lo == kRspDstLo, w, "response destination offset");
  Require(d->o_rsp_val_lo == kRspValLo, w, "response value offset");
  Require(d->o_rsp_flt_lo == kRspFltLo, w, "response fault offset");
  Require(d->o_rsp_lid_lo == kRspLidLo, w, "response link id offset");

  // The wrapper's own arithmetic against the RTL's. If these ever disagree the
  // ports were connected at the wrong width, which no field check would catch.
  Require(d->tb_req_body_w == d->o_req_body_w, w, "wrapper request body width disagrees");
  Require(d->tb_req_w == d->o_req_w, w, "wrapper request width disagrees");
  Require(d->tb_rsp_w == d->o_rsp_w, w, "wrapper response width disagrees");
  Require(d->tb_pipe_depth == d->o_pipe_depth, w, "wrapper pipeline depth disagrees");
  Require(d->tb_entries == d->o_entries, w, "wrapper entry count disagrees");
  Require(d->tb_id_w == d->o_id_w, w, "wrapper identity width disagrees");
  Require(d->tb_dst_w == d->o_dst_w, w, "wrapper destination width disagrees");

  reporter->Check(true, "geometry: entries " + Dec(kEntries) + ", latency " + Dec(kPipeDepth) +
                            ", request " + Dec(kReqW) + " bits, response " + Dec(kRspW) + " bits");
}

// Phase 1: after reset nothing is live, nothing is counted, and the credit law
// holds on the DUT's own ports.
void PhaseResetState(Harness* h, mosaic::Reporter* reporter) {
  Fresh(h, "reset-state");
  for (unsigned k = 0; k < kEntries; k++) {
    Require(!h->entry_valid()[k], "reset-state", "entry " + Dec(k) + " is live after reset");
  }
  for (unsigned st = 0; st < kPipeDepth; st++) {
    Require(!h->req_pipe_valid()[st], "reset-state", "request pipe stage is live after reset");
    Require(!h->rsp_pipe_valid()[st], "reset-state", "response pipe stage is live after reset");
  }
  Require(h->dut_issued() == 0 && h->dut_returned() == 0 && h->dut_outstanding() == 0,
          "reset-state", "counters are not zero after reset");
  // A few idle cycles with no stimulus at all: the shadow watches them too.
  for (int c = 0; c < 4; c++) {
    Stim in;
    in.rem_req_ready = 1;
    in.rsp_ready = 1;
    h->Run(in);
  }
  reporter->Check(h->dut_issued() == 0 && h->dut_delivered() == 0,
                  "an idle link issued or delivered something");
}

// Phase 2: one round trip at each injected delay.
void PhaseRoundTrip(Harness* h, mosaic::Reporter* reporter) {
  const unsigned delays[] = {1, 2, 4, 8};
  for (unsigned di = 0; di < 4; di++) {
    const unsigned delay = delays[di];
    Fresh(h, "round-trip/" + Dec(delay));
    h->remote()->SetDelay(delay);

    const LinkReq req = MakeReq(static_cast<uint32_t>(di), 0x5000 + delay);
    bool accepted = false;
    bool remote_taken = false;
    bool delivered = false;
    uint64_t accept_cycle = 0, remote_cycle = 0, rsp_cycle = 0;
    LinkReq seen_remote{};
    LinkRsp seen_delivery{};

    for (int c = 0; c < 200 && !delivered; c++) {
      Stim in;
      in.req_valid = !accepted;
      in.req = req;
      in.rem_req_ready = 1;
      in.rsp_ready = 1;
      h->Run(in);
      const uint64_t now = h->cycles();

      if (!accepted) {
        Require(h->seen().req_ready, "round-trip/" + Dec(delay),
                "the link refused the first request with an empty table");
        accepted = true;
        accept_cycle = now;
      } else if (!remote_taken && h->seen().rem_req_valid) {
        remote_taken = true;
        remote_cycle = now;
        seen_remote = h->seen().rem_req;
        Require(seen_remote.id == req.id && seen_remote.dst == req.dst &&
                    seen_remote.op == req.op && seen_remote.imm == req.imm &&
                    seen_remote.s1 == req.s1 && seen_remote.s2 == req.s2,
                "round-trip/" + Dec(delay),
                "the remote port carried a different uop than the home port: sent " +
                    ReqStr(req) + ", got " + ReqStr(seen_remote));
        Require(seen_remote.lid == 0, "round-trip/" + Dec(delay),
                "the first request was not named with link id 0: " + Dec(seen_remote.lid));
      } else if (!delivered && h->seen().rsp_valid) {
        delivered = true;
        rsp_cycle = now;
        seen_delivery = h->seen().rsp;
      }
    }

    const std::string where = "round-trip/" + Dec(delay);
    Require(accepted && remote_taken && delivered, where,
            "the round trip did not complete: accepted=" + Bool(accepted) +
                " remote=" + Bool(remote_taken) + " delivered=" + Bool(delivered));
    // The delay the card asks for, as a cycle count rather than an impression:
    // the request takes exactly LINK_DEPTH cycles to cross the link, the remote
    // takes the injected delay, and the response takes exactly LINK_DEPTH more.
    Require(remote_cycle == accept_cycle + kPipeDepth, where,
            "the request reached the remote port after " + Dec(remote_cycle - accept_cycle) +
                " cycles, not " + Dec(kPipeDepth));
    Require(rsp_cycle == remote_cycle + delay + kPipeDepth, where,
            "the response took " + Dec(rsp_cycle - remote_cycle) + " cycles from the remote, "
            "not " + Dec(delay + kPipeDepth));
    LinkRsp want;
    want.id = req.id;
    want.dst = req.dst;
    want.val = RemoteValue(req);
    want.fault = 0;
    want.lid = static_cast<unsigned>(seen_remote.lid);
    Require(SameRsp(seen_delivery, want), where,
            "delivered " + RspStr(seen_delivery) + ", expected " + RspStr(want));
    Require(h->dut_issued() == 1 && h->dut_matched() == 1 && h->dut_outstanding() == 0,
            where, "counters after one round trip: issued " + Dec(h->dut_issued()) +
                       " matched " + Dec(h->dut_matched()) + " outstanding " +
                       Dec(h->dut_outstanding()));
    reporter->Check(true, where + ": request hop " + Dec(kPipeDepth) + ", remote " + Dec(delay) +
                              ", response hop " + Dec(kPipeDepth));
  }
}

// Phase 3: the link delivers in the order responses arrive, not the order
// requests were issued.
void PhaseOutOfOrder(Harness* h, mosaic::Reporter* reporter) {
  Fresh(h, "out-of-order");
  const unsigned kCount = 4;
  // The first request is answered last: the remote's fire cycle is
  // `delay`, so a decreasing delay produces strictly reversed responses.
  const unsigned delays[kCount] = {8, 6, 3, 1};
  std::vector<LinkReq> reqs;
  for (unsigned i = 0; i < kCount; i++) reqs.push_back(MakeReq(20 + i, 0x9000 + i));
  for (unsigned i = 0; i < kCount; i++) h->remote()->SetDelayFor(reqs[i].id, delays[i]);

  std::vector<unsigned> delivery_order;
  std::vector<LinkRsp> deliveries;
  bool issued[kCount] = {false, false, false, false};
  for (int c = 0; c < 200 && static_cast<unsigned>(delivery_order.size()) < kCount; c++) {
    Stim in;
    unsigned next = kCount;
    for (unsigned i = 0; i < kCount; i++) {
      if (!issued[i]) {
        next = i;
        break;
      }
    }
    in.req_valid = next < kCount;
    if (in.req_valid) in.req = reqs[next];
    in.rem_req_ready = 1;
    in.rsp_ready = 1;
    h->Run(in);
    if (in.req_valid && h->seen().req_ready) issued[next] = true;
    if (h->seen().rsp_valid) {
      unsigned which = kCount;
      for (unsigned i = 0; i < kCount; i++) {
        if (h->seen().rsp.id == reqs[i].id) which = i;
      }
      Require(which < kCount, "out-of-order",
              "a delivered response named no request this phase issued: " +
                  RspStr(h->seen().rsp));
      delivery_order.push_back(which);
      deliveries.push_back(h->seen().rsp);
    }
  }
  Require(delivery_order.size() == kCount, "out-of-order",
          "only " + Dec(delivery_order.size()) + " of " + Dec(kCount) + " responses came back");
  for (unsigned i = 0; i < kCount; i++) {
    const unsigned want = kCount - 1 - i;
    Require(delivery_order[i] == want, "out-of-order",
            "delivery " + Dec(i) + " was request " + Dec(delivery_order[i]) +
                ", expected " + Dec(want) + ": the link delivered in issue order");
    Require(deliveries[i].val == RemoteValue(reqs[want]), "out-of-order",
            "delivery " + Dec(i) + " carried the wrong value");
  }
  h->coverage()->ooo_delivery++;
  reporter->Check(true, "out-of-order: " + Dec(kCount) +
                            " responses delivered in reverse issue order");
}

// Phase 4: the namesake. A killed request's response is dropped and counted,
// its credit comes back exactly once, and the slot it freed is taken by a new
// request that the stale response must not disturb.
void PhaseKillLateResponse(Harness* h, mosaic::Reporter* reporter) {
  // ---- 4a: kill while the response is in flight, then let a new request take
  // the freed slot and complete.
  Fresh(h, "kill-late");
  // A is answered late and B even later, so A's response arrives while B is
  // still the occupant of the recycled slot: that is the case the card names,
  // and it is the alias path rather than the empty-slot path.
  h->remote()->SetDelayFor(MakeReq(40, 0).id, 10);
  h->remote()->SetDelayFor(MakeReq(41, 0).id, 20);
  const LinkReq a = MakeReq(40, 0x4000);
  const LinkReq b = MakeReq(41, 0x7000);
  const unsigned a_late = 10;
  const unsigned b_late = 20;

  bool a_accepted = false, a_remote = false, a_killed = false;
  uint64_t a_offered = 0;
  for (int c = 0; c < 40 && !a_killed; c++) {
    Stim in;
    in.req_valid = !a_accepted;
    in.req = a;
    in.rem_req_ready = 1;
    in.rsp_ready = 1;
    // The kill lands two cycles after the remote took the request: while its
    // response is in flight, and before it could have been matched.
    in.kill_valid = a_remote && (h->driven() + 1 >= a_offered + 2);
    in.kill_id = a.id;
    const uint64_t issued_before = h->dut_issued();
    const uint64_t killed_before = h->dut_killed();
    h->Run(in);
    if (!a_accepted && h->seen().req_ready) {
      a_accepted = true;
      Require(h->dut_issued() == issued_before + 1, "kill-late",
              "an accepted request did not take a credit");
    } else if (!a_remote && h->seen().rem_req_valid) {
      a_remote = true;
      a_offered = h->driven();
      Require(h->seen().rem_req.lid == 0, "kill-late",
              "the first request was not named with link id 0");
    } else if (in.kill_valid) {
      a_killed = true;
      Require(h->dut_killed() == killed_before + 1, "kill-late",
              "the kill did not return exactly one credit");
      Require(h->dut_outstanding() == 0, "kill-late",
              "a credit is still outstanding after the kill");
    }
  }
  Require(a_accepted && a_remote && a_killed, "kill-late",
          "the kill did not land while the response was in flight");
  const uint64_t stale_due = a_offered + a_late;

  // B arrives immediately and must take the slot A has just released.
  bool b_accepted = false, b_remote = false;
  unsigned b_lid = 99;
  uint64_t b_offered = 0;
  for (int c = 0; c < 30 && !b_remote; c++) {
    Stim in;
    in.req_valid = !b_accepted;
    in.req = b;
    in.rem_req_ready = 1;
    in.rsp_ready = 1;
    const uint64_t accepted_before = h->dut_issued();
    h->Run(in);
    if (!b_accepted && h->seen().req_ready) {
      b_accepted = true;
      Require(h->dut_issued() == accepted_before + 1, "kill-late",
              "B was accepted without taking a credit");
    }
    if (h->seen().rem_req_valid) {
      b_remote = true;
      b_offered = h->driven();
      b_lid = h->seen().rem_req.lid;
      Require(h->seen().rem_req.id == b.id, "kill-late",
              "B was not the request on the remote wire");
      // This is the namesake: B is the new occupant of the slot A's response
      // will name.
      Require(b_lid == 0, "kill-late",
              "the new request did not take the recycled slot: link id " + Dec(b_lid));
    }
  }
  Require(b_accepted && b_remote, "kill-late", "the new request never reached the remote unit");
  Require(b_offered + b_late > stale_due, "kill-late",
          "the test's own schedule let B answer before A's stale response arrived, which "
          "would leave the recycled slot empty: A at " + Dec(stale_due) + ", B at " +
              Dec(b_offered + b_late));

  // ---- 4b: A's response fires while B is live. It must be refused as an alias
  // and must not be delivered.
  bool saw_b_delivery = false;
  const uint64_t stale_before = h->dut_stale();
  const uint64_t alias_before = h->dut_alias();
  const uint64_t delivered_before = h->dut_delivered();
  bool stale_offered = false;
  LinkRsp b_delivery{};
  for (int c = 0; c < 80 && !saw_b_delivery; c++) {
    Stim in;
    in.rem_req_ready = 1;
    in.rsp_ready = 1;
    h->Run(in);
    const uint64_t n = h->driven();
    if (h->seen().rsp_valid) {
      // Only B's own result may ever be delivered in this phase.
      Require(h->seen().rsp.id == b.id, "kill-late",
              "a response was delivered at cycle " + Dec(n) + " that was not B's: " +
                  RspStr(h->seen().rsp));
      saw_b_delivery = true;
      b_delivery = h->seen().rsp;
      break;
    }
    if (n == stale_due) {
      // The cycle the stale response is presented: it must be consumed and
      // refused, with no delivery.
      Require(h->seen().rem_rsp_valid, "kill-late",
              "the stale response was not presented at the cycle the schedule says");
      Require(h->seen().rem_rsp_ready, "kill-late",
              "the link did not accept the stale response, so it would be re-presented "
              "forever");
      Require(h->dut_stale() == stale_before + 1 && h->dut_alias() == alias_before + 1,
              "kill-late",
              "the stale response was not counted as an alias: stale " +
                  Dec(h->dut_stale()) + " (was " + Dec(stale_before) + "), alias " +
                  Dec(h->dut_alias()) + " (was " + Dec(alias_before) + ")");
      Require(h->dut_delivered() == delivered_before, "kill-late",
              "the stale response was delivered to the home port");
      stale_offered = true;
    }
  }
  Require(stale_offered, "kill-late", "the stale response never arrived while B was live");
  Require(saw_b_delivery, "kill-late", "B's own response was never delivered");
  Require(b_delivery.id == b.id && b_delivery.val == RemoteValue(b), "kill-late",
          "B's delivered response was corrupted by the stale one: " + RspStr(b_delivery));
  Require(static_cast<unsigned>(b_delivery.lid) == b_lid, "kill-late",
          "B's delivered link id is not the one the remote echoed");
  Require(h->dut_stale() >= stale_before + 1, "kill-late",
          "o_stale_rsp_ctr did not move for the refused response");
  Require(h->dut_alias() >= alias_before + 1, "kill-late",
          "o_alias_rsp_ctr did not move for a response naming a live slot with the "
          "wrong identity");
  h->coverage()->kills++;

  // ---- 4c: a kill racing the response transfer in the same cycle. The kill
  // wins: the response is dropped and the credit is returned once, not twice.
  {
    Fresh(h, "kill-vs-response");
    const unsigned race_delay = 3;
    h->remote()->SetDelayFor(MakeReq(50, 0).id, race_delay);
    const LinkReq cc = MakeReq(50, 0xC000);
    bool accepted = false, remote = false;
    uint64_t remote_offered = 0;
    bool killed = false;
    const uint64_t killed_before = h->dut_killed();
    const uint64_t delivered_at_race = h->dut_delivered();
    for (int c = 0; c < 40 && !killed; c++) {
      Stim in;
      in.req_valid = !accepted;
      in.req = cc;
      in.rem_req_ready = 1;
      in.rsp_ready = 1;
      // The kill is asserted in the very cycle the remote presents the
      // response, so the kill and the transfer are simultaneous.
      in.kill_valid = remote && (h->driven() + 1 == remote_offered + race_delay);
      in.kill_id = cc.id;
      h->Run(in);
      if (!accepted && h->seen().req_ready) accepted = true;
      if (!remote && h->seen().rem_req_valid) {
        remote = true;
        remote_offered = h->driven();
      }
      if (in.kill_valid) {
        killed = true;
        Require(h->dut_killed() == killed_before + 1, "kill-vs-response",
                "the kill did not return exactly one credit");
        Require(h->dut_delivered() == delivered_at_race, "kill-vs-response",
                "a response was delivered for a request killed in the same cycle");
        Require(h->dut_stale() >= 1, "kill-vs-response",
                "the racing response was not counted as stale");
        h->coverage()->kill_vs_response++;
      }
    }
    Require(killed, "kill-vs-response", "the kill-vs-response race never happened");
    // Nothing may be delivered afterwards either: the response was consumed and
    // dropped, not queued.
    for (int c = 0; c < 6; c++) {
      Stim in;
      in.rem_req_ready = 1;
      in.rsp_ready = 1;
      h->Run(in);
      Require(!h->seen().rsp_valid, "kill-vs-response",
              "a response appeared after the kill consumed the racing one");
    }
    reporter->Check(true, "kill-vs-response: the kill outranked the transfer, one credit returned");
  }

  // ---- 4d: a kill for an identity that is not in flight is counted and
  // changes nothing.
  {
    Fresh(h, "kill-miss");
    const LinkReq d = MakeReq(60, 0xD000);
    const uint64_t missed_before = h->dut_kill_missed();
    Stim in;
    in.kill_valid = true;
    in.kill_id = d.id;
    in.rem_req_ready = 1;
    in.rsp_ready = 1;
    h->Run(in);
    Require(h->dut_kill_missed() == missed_before + 1, "kill-miss",
            "a kill that matched nothing was not counted");
    Require(h->dut_issued() == 0 && h->dut_outstanding() == 0, "kill-miss",
            "a kill that matched nothing changed the credit accounts");
    reporter->Check(true, "kill-miss: an unmatched kill is counted and inert");
  }

  reporter->Check(true, "kill-late: the late response was refused, the credit returned once, "
                        "and the new occupant of the slot completed normally");
}

// Phase 5: both directions saturated and released.
void PhaseBackPressure(Harness* h, mosaic::Reporter* reporter) {
  // ---- 5a: the credit bound. The far end consumes requests but does not
  // answer them, so the table is what runs out.
  Fresh(h, "backpressure-credits");
  h->remote()->SetDelay(1000);   // answered long after this phase is over
  unsigned accepted = 0;
  LinkReq cur = MakeReq(70, 0xE000);
  unsigned next = 71;
  for (int c = 0; c < 128 && accepted < kEntries; c++) {
    Stim in;
    in.req_valid = true;
    in.req = cur;
    in.rem_req_ready = 1;
    in.rsp_ready = 1;
    h->Run(in);
    if (h->seen().req_ready) {
      accepted++;
      cur = MakeReq(next++, 0xE000);
    }
  }
  Require(accepted == kEntries, "backpressure-credits",
          "only " + Dec(accepted) + " of " + Dec(kEntries) + " credits were taken");
  for (int c = 0; c < 4; c++) {
    Stim in;
    in.req_valid = true;
    in.req = cur;
    in.rem_req_ready = 1;
    in.rsp_ready = 1;
    h->Run(in);
    Require(!h->seen().req_ready, "backpressure-credits",
            "the link accepted a request with every credit taken");
  }
  Require(h->dut_outstanding() == kEntries, "backpressure-credits",
          "outstanding " + Dec(h->dut_outstanding()) + " != " + Dec(kEntries));

  // Return every credit at once and require that they are usable again: a new
  // request completes immediately afterwards.
  const uint64_t killed_before = h->dut_killed();
  {
    Stim in;
    in.flush_valid = true;
    in.rem_req_ready = 1;
    in.rsp_ready = 1;
    h->Run(in);
  }
  Require(h->dut_killed() == killed_before + kEntries, "backpressure-credits",
          "the flush returned " + Dec(h->dut_killed() - killed_before) + " of " +
              Dec(kEntries) + " credits");
  Require(h->dut_outstanding() == 0, "backpressure-credits",
          "a credit survived the flush");
  const LinkReq after = MakeReq(200, 0xE100);
  h->remote()->SetDelayFor(after.id, 1);
  bool accepted2 = false, delivered2 = false;
  for (int c = 0; c < 40 && !delivered2; c++) {
    Stim in;
    in.req_valid = !accepted2;
    in.req = after;
    in.rem_req_ready = 1;
    in.rsp_ready = 1;
    h->Run(in);
    if (!accepted2 && h->seen().req_ready) accepted2 = true;
    if (h->seen().rsp_valid) delivered2 = true;
  }
  Require(accepted2 && delivered2, "backpressure-credits",
          "the credits returned by the flush were not usable: accepted=" +
              Bool(accepted2) + " delivered=" + Bool(delivered2));
  reporter->Check(true, "backpressure-credits: " + Dec(kEntries) +
                            " credits taken, refused at the bound, all returned by one flush");

  // ---- 5b: the pipe bound. The far end accepts nothing at all, so the
  // request pipeline is what runs out. Its depth, not the credit table, is the
  // limit while nothing drains -- a documented property of a registered hop.
  Fresh(h, "backpressure-pipe");
  h->remote()->SetDelay(1);
  accepted = 0;
  cur = MakeReq(210, 0xE200);
  next = 211;
  for (int c = 0; c < 64 && accepted < kEntries; c++) {
    Stim in;
    in.req_valid = true;
    in.req = cur;
    in.rem_req_ready = 0;   // nothing drains
    in.rsp_ready = 1;
    h->Run(in);
    if (h->seen().req_ready) {
      accepted++;
      cur = MakeReq(next++, 0xE200);
    }
  }
  Require(accepted == kPipeDepth, "backpressure-pipe",
          "with the far end stalled the link took " + Dec(accepted) + " requests, not the " +
              Dec(kPipeDepth) + " its pipeline holds");
  for (int c = 0; c < 4; c++) {
    Stim in;
    in.req_valid = true;
    in.req = cur;
    in.rem_req_ready = 0;
    in.rsp_ready = 1;
    h->Run(in);
    Require(!h->seen().req_ready, "backpressure-pipe",
            "the link accepted a request with a full pipeline and a stalled far end");
  }

  // Release: the held request must be accepted and everything must drain.
  bool held_accepted = false;
  for (uint64_t c = 0; c < kDrainBound; c++) {
    Stim in;
    in.req_valid = !held_accepted;
    in.req = cur;
    in.rem_req_ready = 1;
    in.rsp_ready = 1;
    h->Run(in);
    if (!held_accepted && h->seen().req_ready) held_accepted = true;
    if (held_accepted && h->drained()) break;
  }
  Require(held_accepted, "backpressure-pipe", "the held request was never accepted");
  Require(h->drained(), "backpressure-pipe",
          "the stalled request path did not drain: outstanding " +
              Dec(h->dut_outstanding()));
  Require(h->dut_delivered() == kPipeDepth + 1, "backpressure-pipe",
          "delivered " + Dec(h->dut_delivered()) + " of " + Dec(kPipeDepth + 1));
  reporter->Check(true, "backpressure-pipe: the pipeline held " + Dec(kPipeDepth) +
                            " requests against a stalled far end, with no loss");

  // ---- 5c: the response path saturated. The request path must stay open: the
  // only thing that may refuse a request is a missing credit.
  Fresh(h, "backpressure-response");
  h->remote()->SetDelay(1);
  std::vector<LinkReq> reqs;
  for (unsigned i = 0; i < kEntries; i++) reqs.push_back(MakeReq(80 + i, 0xF000));
  for (unsigned i = 0; i < kEntries; i++) {
    Stim in;
    in.req_valid = true;
    in.req = reqs[i];
    in.rem_req_ready = 1;
    in.rsp_ready = 1;
    h->Run(in);
    Require(h->seen().req_ready, "backpressure-response",
            "request " + Dec(i) + " was refused with an empty table");
  }
  // Now stall the home side. Responses match and park in the response pipe
  // until it is full; after that the remote end cannot hand one over either.
  bool pipe_full = false;
  for (int c = 0; c < 40 && !pipe_full; c++) {
    Stim in;
    in.rem_req_ready = 1;
    in.rsp_ready = 0;
    h->Run(in);
    pipe_full = !h->seen().rem_rsp_ready && h->remote()->holding();
  }
  Require(pipe_full, "backpressure-response",
          "the response path never filled with the home side stalled");

  // The card's criterion, as a directed check rather than a timeout: with the
  // response storage full and credits still free, a request must be accepted.
  const LinkReq late = MakeReq(90, 0x1234);
  unsigned free_before = 0;
  for (unsigned k = 0; k < kEntries; k++) free_before += h->entry_valid()[k] ? 0 : 1;
  Require(free_before > 0, "backpressure-response",
          "no credit was free, so the check below would prove nothing");
  Require(!h->rsp_pipe_valid()[0] || true, "backpressure-response", "unreachable");
  {
    Stim in;
    in.req_valid = true;
    in.req = late;
    in.rem_req_ready = 1;
    in.rsp_ready = 0;
    h->Run(in);
    Require(h->seen().req_ready, "backpressure-response",
            "a stalled response path refused a request while " + Dec(free_before) +
                " credits were free: the two channels are sharing a resource");
  }
  // Release the home side and let everything drain in bounded time. Equal
  // injected delays mean the responses arrive in the order the requests were
  // issued, so this is the *in-order* half of the card's ordering requirement;
  // the out-of-order phase is the other half.
  uint64_t drain_cycles = 0;
  std::vector<uint64_t> order;
  for (uint64_t c = 0; c < kDrainBound; c++) {
    Stim in;
    in.rem_req_ready = 1;
    in.rsp_ready = 1;
    h->Run(in);
    if (h->seen().rsp_valid) order.push_back(h->seen().rsp.id);
    drain_cycles = c;
    if (h->drained()) break;
  }
  // The eight requests of this phase, plus the one issued while the response
  // path was saturated -- each of which must have been delivered exactly once,
  // in the order the requests were issued.
  Require(order.size() == kEntries + 1, "backpressure-response",
          "delivered " + Dec(order.size()) + " responses, expected " +
              Dec(kEntries + 1));
  for (unsigned i = 0; i < kEntries + 1; i++) {
    const uint64_t want = (i < kEntries) ? reqs[i].id : late.id;
    Require(order[i] == want, "backpressure-response",
            "delivery " + Dec(i) + " was not request " + Dec(i) +
                " although every response took the same delay");
  }
  Require(h->dut_outstanding() == 0, "backpressure-response",
          "the response path did not drain: outstanding " + Dec(h->dut_outstanding()));
  Require(h->remote()->pending_count() == 0 && !h->remote()->holding(),
          "backpressure-response", "the remote unit still holds work after the drain");
  Require(h->dut_issued() == h->dut_matched() + h->dut_killed(), "backpressure-response",
          "credits leaked: issued " + Dec(h->dut_issued()) + " matched " +
              Dec(h->dut_matched()) + " killed " + Dec(h->dut_killed()));
  reporter->Check(true, "backpressure-response: the response path saturated for up to " +
                            Dec(kPipeDepth) + " results without blocking a request");
  (void)drain_cycles;
}

// Phase 6: random issue, kill, flush and back-pressure, then a bounded drain.
void PhaseRandom(Harness* h, mosaic::Reporter* reporter, uint64_t seed) {
  Fresh(h, "random");
  mosaic::Rng rng(seed ^ 0x5eed0f11ull);
  const unsigned delays[] = {1, 2, 4, 8};

  std::vector<uint64_t> ids;
  uint32_t next_index = 100;
  LinkReq cur = MakeReq(next_index, 0x2000);
  bool holding = false;
  unsigned hold_left = 0;
  bool req_hold = false;
  unsigned req_hold_left = 0;
  uint64_t issued_start = h->dut_issued();

  for (int c = 0; c < 20000; c++) {
    // Back-pressure runs: a direction is closed for a random number of cycles.
    if (hold_left == 0) {
      holding = rng.Chance(25);
      hold_left = holding ? (1 + rng.Below(6)) : 0;
    } else if (holding) {
      hold_left--;
    }
    if (req_hold_left == 0) {
      req_hold = rng.Chance(20);
      req_hold_left = req_hold ? (1 + rng.Below(5)) : 0;
    } else if (req_hold) {
      req_hold_left--;
    }
    const bool rem_req_ready = !req_hold;
    const bool rsp_ready = !holding;

    Stim in;
    in.rem_req_ready = rem_req_ready;
    in.rsp_ready = rsp_ready;
    in.req_valid = true;
    in.req = cur;
    if (rng.Chance(4)) {
      in.kill_valid = true;
      // Half the kills name something that was really issued, half name
      // something that never was: misses are part of the contract.
      if (!ids.empty() && rng.Chance(75)) {
        in.kill_id = ids[rng.Below(static_cast<uint32_t>(ids.size()))];
      } else {
        in.kill_id = MakeId(1, rng.Below(64), rng.Below(128), rng.Below(8));
      }
    }
    in.flush_valid = rng.Chance(1);

    h->remote()->SetDelay(delays[rng.Below(4)]);
    h->Run(in);

    if (h->seen().req_ready) {
      ids.push_back(cur.id);
      next_index++;
      cur = MakeReq(next_index, 0x2000);
    }
    if (rng.Chance(1)) h->remote()->SetDelayFor(cur.id, delays[rng.Below(4)]);
  }

  const uint64_t issued_random = h->dut_issued() - issued_start;
  reporter->Check(h->dut_issued() > issued_start, "the soak issued nothing");

  // Stop the stimulus, open both directions, and require the link to empty
  // within a stated bound. A request is in flight for at most
  // (request hop + longest injected delay + response hop) cycles after the
  // environment stops stalling, and there are at most ENTRIES of them, so
  // ENTRIES * (2*LATENCY + 8) + slack is a bound that does not depend on luck.
  const uint64_t bound = kEntries * (2 * kPipeDepth + 8) + 16;
  uint64_t drained = 0;
  bool empty = false;
  for (uint64_t c = 0; c < bound; c++) {
    Stim in;
    in.rem_req_ready = 1;
    in.rsp_ready = 1;
    h->Run(in);
    drained = c;
    if (h->drained()) {
      empty = true;
      break;
    }
  }
  Require(empty, "random",
          "the link did not drain within the stated bound of " + Dec(bound) +
              " cycles: outstanding " + Dec(h->dut_outstanding()) + " remote pending " +
              Dec(h->remote()->pending_count()));
  Require(h->dut_issued() == h->dut_matched() + h->dut_killed(), "random",
          "after the drain: issued " + Dec(h->dut_issued()) + " != matched " +
              Dec(h->dut_matched()) + " + killed " + Dec(h->dut_killed()));
  reporter->Check(true, "random: " + Dec(issued_random) + " requests issued in the soak, "
                        "drained in " + Dec(drained) + " cycles (bound " + Dec(bound) + ")");
}

// The coverage summary, kept so the RESULT line carries the numbers rather than
// only the statement that they were non-zero.
std::string g_coverage;

// The stimulus has to have reached every mechanism, or a shorter path would
// pass the case without touching the defect it exists for.
void PhaseCoverage(Harness* h, mosaic::Reporter* reporter) {
  const Coverage& c = *h->coverage();
  struct Row {
    const char* name;
    uint64_t value;
  };
  const Row rows[] = {
      {"accepts", c.accepts},
      {"matches", c.matches},
      {"kills", c.kills},
      {"flushes", c.flushes},
      {"stale responses", c.stale},
      {"alias refusals", c.alias},
      {"deliveries", c.delivered},
      {"kill misses", c.kill_missed},
      {"request stalls", c.req_stalls},
      {"response stalls", c.rsp_stalls},
      {"table full", c.table_full},
      {"response path full", c.reply_pipe_full},
      {"out-of-order delivery", c.ooo_delivery},
      {"kill vs response race", c.kill_vs_response},
  };
  std::string summary;
  for (const Row& row : rows) {
    summary += std::string(row.name) + "=" + Dec(row.value) + " ";
    reporter->Check(row.value > 0,
                    std::string("the case never exercised ") + row.name);
  }
  g_coverage = summary;
  reporter->Check(true, "coverage: " + summary);
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
  mosaic::ClockDriver clk;
  Vmosaic_remote_link_tb dut;

  Harness harness(&dut, &clk, options.max_cycles);

  std::string detail;
  bool passed = true;
  try {
    PhaseGeometry(&harness, &reporter);
    PhaseResetState(&harness, &reporter);
    PhaseRoundTrip(&harness, &reporter);
    PhaseOutOfOrder(&harness, &reporter);
    PhaseKillLateResponse(&harness, &reporter);
    PhaseBackPressure(&harness, &reporter);
    PhaseRandom(&harness, &reporter, options.seed);
    PhaseCoverage(&harness, &reporter);

    detail = "remote link contract holds: " + std::to_string(harness.comparisons()) +
             " shadow comparisons over " + std::to_string(harness.cycles()) + " cycles, " +
             std::to_string(kEntries) + " credits, latency " + std::to_string(kPipeDepth) +
             ", seed " + std::to_string(options.seed) + "; coverage: " + g_coverage;
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "contract holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
