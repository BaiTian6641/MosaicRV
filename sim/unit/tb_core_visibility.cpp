// ============================================================================
// tb_core_visibility.cpp -- CASE=mem.visibility_provenance, work package V-018.
//
// The card (docs/validation-plan.md §5, V-018) says: separate the architectural
// memory from the memory that became *visible*. The DUT is the integrated p0
// core with the memory path wired in (the same top CASE=core.mem_program
// drives), and the deliverable is not a value comparison but a **provenance
// model**: for every byte that changes in the DUT's memory, name the store
// instruction that produced it and prove that producer was a committed store,
// that it committed before the byte appeared, and that it is the most recent
// committed store covering that byte.
//
// --------------------------------------------------------------- three memories
//
// Nothing below is derived from the DUT's own view of what it wrote. There are
// three memories, and the DUT only ever touches one of them:
//
//   1. `ref_mem`  -- the independent RV64IM interpreter's memory
//                    (sim/unit/mem_ref.h). It decodes the same instruction
//                    words from the same image and models loads and stores
//                    itself, so it is the *expectation*: every retired
//                    instruction's pc, destination and value, and the final
//                    byte image.
//   2. `dut_mem`  -- the memory the DUT's data port talks to (mem_ref.h's
//                    DataMem over memory_model.cpp). It is the *subject*: every
//                    accepted write transaction is a byte becoming visible, and
//                    it is instrumented byte by byte.
//   3. `arch_mem` -- the provenance ledger. It starts from the same immutable
//                    image bytes and is advanced **only** by stores that
//                    retired, in retire order, using the store payload the
//                    frozen event stream carries (ev_store_addr/data/size). It
//                    is never read from `dut_mem` or `ref_mem`.
//
// `arch_mem == ref_mem` at the end is what makes the ledger meaningful; a
// divergence is reported with the address, the expected byte, the ledger's byte
// and the instruction that produced it.
//
// ------------------------------------------------------------- the provenance rule
//
// A store's retirement (kind RETIRE, mem_is_store) and its visibility (kind
// MEM_VISIBLE) are two events, per config/contracts/event_v1.json's
// `store_drain_is_separate`. The case records both, with the store's
// `retire_seq` as the causal id. For each accepted write transaction on the
// data port it requires, in order:
//
//   (1) the transaction is the drain of the **oldest committed store not yet
//       drained** -- the store queue is an ordered compacted sequence and its
//       drain takes position 0, so the visible sequence must equal the commit
//       sequence. A transaction that matches an already-drained store is a
//       duplicate drain; one that matches a younger committed store is an
//       out-of-order drain; one that matches no committed store is a byte with
//       no legal producer (the wrong-path case the card names).
//   (2) the producer committed no later than the visibility cycle -- the
//       commit/drain separation the card's title is about;
//   (3) the transaction's byte strobes are exactly the bytes the store's size
//       owns, so a partial mask is checked rather than assumed;
//   (4) each strobed byte equals the store's own payload byte, and equals
//       `arch_mem`'s byte -- i.e. the byte's value is the value the most recent
//       committed store covering it holds.
//
// ------------------------------------------------------------ the load's source
//
// A load's value is checked against *allowed sources* rather than only against
// `rd`: the case tracks the architectural register file from the retirement
// stream, decodes each retiring load from the image word, computes
// `base + imm` and compares the retired value with `arch_mem`'s bytes extended
// by the load's size and signedness. Because retirement is in order, every
// older store has committed by the time a load retires, so `arch_mem` is
// exactly the set of sources the architecture allows: the image byte, or the
// bytes of the most recent committed store covering the address. A load that
// returns stale memory (a forwarded store that was dropped) or a value no store
// wrote fails here with the pc, the address and both values.
//
// ------------------------------------------------------------------- the scene
//
// The corpus programs p03_loadstore and p09_storeload cover every load/store
// width, sign extension, partial byte masks and same-hart forwarding. A
// harness-authored *scene* (written into RAM past the image, never into
// tests/programs/**) adds the two coverages the corpus cannot: a store on a
// wrong path directed at a writable RAM byte, and an access crossing the
// 8-byte line boundary the data port is organised in. The scene is entered by
// the reset-vector brick and hands over to the corpus program's own `main`.
//
// p0's frozen misalignment policy traps a crossing access before memory (see
// config/profiles/p0.json and mosaic_lsu_endpoint.sv), so the crossing access
// is covered as a *refusal*: the case requires zero data-port transactions at a
// misaligned address and the target bytes unchanged. The "not covered" section
// of results/reports/V-018-visibility.md says so explicitly.
//
// ------------------------------------------------------------------- controls
//
// Four negative controls, built by tools/run_visibility_controls.py, each from
// a deleted build directory with its own -D and required to differ from the
// shipping binary and to exit 1 naming the failure:
//
//   * MOSAIC_CORE_MUTANT_MEM_BEHIND_BRANCH + MOSAIC_SQ_MUTANT_VISIBLE_BEFORE_COMMIT
//       -- a wrong-path store is dispatched and drained before it commits.
//   * MOSAIC_SQ_MUTANT_DRAIN_DUPLICATE
//       -- an accepted drain does not remove the entry: one store's bytes
//          become visible twice.
//   * MOSAIC_LQ_MUTANT_MEMORY_OVER_STORE
//       -- a load that must forward from the store queue reads memory instead:
//          stale bytes no legal source produced.
//   * MOSAIC_VISIBILITY_RD_ONLY + MOSAIC_CORE_MUTANT_STORE_SIZE_WRONG
//       -- a *weakening* control: the driver drops every memory/provenance
//          comparison and keeps only the rd comparison, and must then pass on a
//          defect the real comparison catches, proving the byte comparison is
//          what carries the case.
//
// ------------------------------------------------------------- what it checks
//
//   per run:
//   1. the per-instruction retirement stream equals the reference, in order;
//   2. every retiring store carries its payload (ev_store valid) and the
//      ledger (`arch_mem`) built from it agrees with the reference's memory;
//   3. every byte that became visible names a committed producer, committed
//      before it appeared, and covered by the right byte mask (the rule above);
//   4. every retiring load's value equals an allowed source (`arch_mem`);
//   5. the final image equals the reference's, byte for byte, over every
//      address either side wrote, plus the signature area and the scene's
//      wrong-path addresses;
//   6. the four signature words the program's own stores left equal the host
//      oracle's (the run's published result);
//   7. no transaction is misaligned, none faults, the store queue is empty and
//      no store's bytes are missing;
//      across all runs:
//   8. coverage asserted by measurement: every load/store width, sign
//      extension, partial byte masks, same-hart forwarding, a boundary-crossing
//      access, and a wrong-path store that left no byte.
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "Vmosaic_core_tb.h"
#include "elf_loader.h"
#include "event_codec.h"
#include "mem_ref.h"
#include "memory_model.h"
#include "sim_common.h"

using mosaic_ref::DataMem;
using mosaic_ref::RefInsn;
using mosaic_ref::RefResult;

namespace {

// The program must reach its exit protocol within this many cycles; the runs are
// a few hundred cycles long and this is only reached if the machine stops
// making progress.
constexpr int kTraceCycles = 100000;
// How long the machine is given, after the predicted stream ends, for the stores
// that were already authorised to reach memory.
constexpr int kDrainCycles = 512;
// A stall is a defect with a location, not a timeout.
constexpr int kStallCycles = 20000;
constexpr int kResetCycles = 4;

// Where the harness-authored scene is placed. Inside the frozen RAM region
// (0x80000000..0x80200000, config/memory/p0.json) and past every segment the
// linker script produces, so it collides with no program byte.
constexpr uint64_t kSceneBase        = UINT64_C(0x80010000);
constexpr uint64_t kSceneScratch     = UINT64_C(0x80010200);
constexpr uint64_t kSceneScratchSpan = 16;

// The weakening control: when defined, the driver keeps the per-instruction
// comparison against the reference and drops *every* memory/provenance
// comparison. It must then pass on a defect the real comparison catches, which
// is what tools/run_visibility_controls.py demonstrates. It is a control on the
// driver, not on the RTL: nothing about the machine changes.
#ifdef MOSAIC_VISIBILITY_RD_ONLY
constexpr bool kCompareMemory = false;
#else
constexpr bool kCompareMemory = true;
#endif

// The codec's first live use: every record this case emits is encoded to the
// frozen binary layout and decoded back, field by field, before any comparison
// uses it. The counters are reported so the round trip is measured, not claimed.
uint64_t g_codec_records = 0;
uint64_t g_codec_round_trips = 0;

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value, int digits = 16) { return mosaic::Hex(value, digits); }

template <typename T>
std::string Dec(T value) {
  return std::to_string(value);
}

// ============================================================================
// Instruction encoders (the harness-authored scene and the reset bricks)
// ============================================================================
uint32_t EncU(uint32_t imm20, uint32_t rd, uint32_t opcode) {
  return (imm20 << 12) | (rd << 7) | opcode;
}
uint32_t EncI(int32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t opcode) {
  return ((static_cast<uint32_t>(imm) & 0xFFFu) << 20) | (rs1 << 15) | (f3 << 12) |
         (rd << 7) | opcode;
}
uint32_t EncS(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3) {
  const uint32_t u = static_cast<uint32_t>(imm);
  return (((u >> 5) & 0x7Fu) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
         ((u & 0x1Fu) << 7) | 0x23u;
}
uint32_t EncB(int32_t offset, uint32_t rs2, uint32_t rs1, uint32_t f3) {
  const uint32_t u = static_cast<uint32_t>(offset);
  return (((u >> 12) & 1u) << 31) | (((u >> 5) & 0x3Fu) << 25) | (rs2 << 20) |
         (rs1 << 15) | (f3 << 12) | (((u >> 1) & 0xFu) << 8) |
         (((u >> 11) & 1u) << 7) | 0x63u;
}
uint32_t EncJ(int32_t offset, uint32_t rd) {
  const uint32_t u = static_cast<uint32_t>(offset);
  const uint32_t imm = (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3FFu) << 21) |
                       (((u >> 11) & 1u) << 20) | (((u >> 12) & 0xFFu) << 12);
  return imm | (rd << 7) | 0x6Fu;
}

// The register names the scene uses (the corpus's own convention).
constexpr uint32_t kX0 = 0, kT0 = 5, kT1 = 6, kT2 = 7, kS3 = 19, kT3 = 28;

// A word is a store when its opcode is STORE and funct3 selects a width; the
// size in bytes is 1 << funct3.
bool IsStoreWord(uint32_t w, unsigned* size_bytes) {
  if ((w & 0x7Fu) != 0x23u) return false;
  const unsigned f3 = (w >> 12) & 0x7u;
  if (f3 > 3u) return false;
  if (size_bytes != nullptr) *size_bytes = 1u << f3;
  return true;
}

// ============================================================================
// The loaded program image
// ============================================================================
// The ELF's loadable segments as instruction words, with the harness's bricks
// written over them (the reset vector's jump, the words after it, and the
// scene). The instruction memory answers ECALL for anything not in the map, so a
// runaway fetch stops the machine cleanly instead of reading a don't-care.
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
    entry_ = image.entry;
    image_ = image;
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
    if (words_.empty()) {
      *detail = "the image has no words";
      return false;
    }
    return true;
  }

  uint32_t Word(uint64_t addr) const {
    auto it = words_.find(addr);
    if (it == words_.end()) return 0x00000073u;  // ECALL
    return it->second;
  }

  void WriteWord(uint64_t addr, uint32_t word) { words_[addr] = word; }

  uint64_t entry() const { return entry_; }
  const mosaic::Image& image() const { return image_; }

  const mosaic::Symbol* Symbol(const std::string& name) const {
    return image_.FindSymbol(name);
  }

 private:
  std::map<uint64_t, uint32_t> words_;
  mosaic::Image image_;
  uint64_t entry_ = 0;
};

// ============================================================================
// The instruction memory
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
    Request r;
    r.addr = addr;
    r.id = id;
    r.epoch = epoch;
    inflight_.push_back(Entry{r, 1});
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
// Geometry, taken from the elaborated DUT rather than re-derived
// ============================================================================
struct Geometry {
  uint32_t xlen = 0;
  uint32_t retire_width = 0;
  uint32_t rob_entries = 0;
  uint32_t seq_w = 0;
  uint32_t ret_id_w = 0;  // the per-lane {generation, tag} identity the core carries
  uint64_t reset_vector = 0;
};

Geometry ReadGeometry(Vmosaic_core_tb* dut) {
  Geometry g;
  g.xlen = dut->o_geom_xlen_o;
  g.retire_width = dut->o_geom_retire_width_o;
  g.rob_entries = dut->o_geom_rob_entries_o;
  g.seq_w = dut->o_geom_seq_w_o;
  g.ret_id_w = dut->o_geom_ret_id_w_o;
  g.reset_vector = dut->o_geom_reset_vector_o;
  return g;
}

// Verilator hands a wide port over as a `VlWide` indexed in 32-bit words, two
// per 64-bit lane; narrow ports arrive as plain scalars.
template <typename Wide>
uint64_t PayloadLane(const Wide& wide, uint32_t lane) {
  return static_cast<uint64_t>(wide[lane * 2]) |
         (static_cast<uint64_t>(wide[lane * 2 + 1]) << 32);
}
inline uint64_t PayloadLane(uint64_t value, uint32_t /*lane*/) { return value; }

uint64_t PackedLane(uint64_t packed, uint32_t lane, uint32_t width) {
  const uint64_t mask = (width >= 64) ? ~0ull : ((1ull << width) - 1ull);
  return (packed >> (lane * width)) & mask;
}

uint64_t SignExtend(uint64_t raw, unsigned bytes) {
  const int shift = 64 - static_cast<int>(8 * bytes);
  return static_cast<uint64_t>(static_cast<int64_t>(raw << shift) >> shift);
}

unsigned SizeBytes(unsigned size_code) { return 1u << size_code; }

// The frozen test protocol lives in ordinary RAM but is a *protocol register*,
// not bytes: memory_model.cpp's Write records the event and sets the exit
// status without storing the value, and its Read answers 0 ("still running").
// The producer of such a write is still attributed -- it is a real committed
// store and it really did reach the port -- but its byte value is compared only
// on the port, because neither the DUT's memory nor the reference's holds it.
bool IsProtocolAddress(uint64_t addr) {
  return addr == MOSAIC_TOHOST || addr == MOSAIC_FROMHOST;
}

// ============================================================================
// The architectural-memory ledger
// ============================================================================
// Bytes start at the immutable image value (read from a private seed model, so
// this class never observes the DUT's memory) and are advanced only by committed
// stores, in retire order, from the retirement stream's store payload.
class ArchMem {
 public:
  explicit ArchMem(mosaic::MemoryModel* seed) : seed_(seed) {}

  uint8_t Byte(uint64_t addr) const {
    auto it = bytes_.find(addr);
    if (it != bytes_.end()) return it->second;
    uint64_t v = 0;
    if (seed_->Read(addr, 1, &v) != mosaic::AccessStatus::kOk) return 0;
    return static_cast<uint8_t>(v);
  }

  void ApplyStore(uint64_t addr, uint64_t data, unsigned size) {
    for (unsigned k = 0; k < size; ++k) {
      bytes_[addr + k] = static_cast<uint8_t>((data >> (8 * k)) & 0xFFu);
    }
  }

  uint64_t RawValue(uint64_t addr, unsigned size) const {
    uint64_t value = 0;
    for (unsigned k = 0; k < size; ++k) {
      value |= static_cast<uint64_t>(Byte(addr + k)) << (8 * k);
    }
    return value;
  }

 private:
  mosaic::MemoryModel* seed_;
  std::map<uint64_t, uint8_t> bytes_;
};

// A load decoded from its image word: what the architecture says it reads.
struct LoadDecode {
  bool is_load = false;
  uint64_t pc = 0;
  uint64_t addr = 0;
  unsigned size = 0;  // bytes
  bool sign = false;
  uint32_t rs1 = 0;
};

LoadDecode DecodeLoad(const ProgImage& img, uint64_t pc, const uint64_t* regs) {
  LoadDecode d;
  d.pc = pc;
  const uint32_t w = img.Word(pc);
  if ((w & 0x7Fu) != 0x03u) return d;
  const uint32_t f3 = (w >> 12) & 0x7u;
  switch (f3) {
    case 0x0u: d.size = 1; d.sign = true; break;   // lb
    case 0x1u: d.size = 2; d.sign = true; break;   // lh
    case 0x2u: d.size = 4; d.sign = true; break;   // lw
    case 0x3u: d.size = 8; d.sign = true; break;   // ld
    case 0x4u: d.size = 1; d.sign = false; break;  // lbu
    case 0x5u: d.size = 2; d.sign = false; break;  // lhu
    case 0x6u: d.size = 4; d.sign = false; break;  // lwu
    default: return d;
  }
  d.rs1 = (w >> 15) & 0x1Fu;
  const int32_t imm = static_cast<int32_t>(w) >> 20;
  d.addr = regs[d.rs1] + static_cast<uint64_t>(static_cast<int64_t>(imm));
  d.is_load = true;
  return d;
}

// ============================================================================
// The declared programs and their host-oracle signatures
// ============================================================================
// `python3 tools/host_oracle.py --program p03_loadstore` and
// `--program p09_storeload` print exactly these rows (re-run at the time this
// case was written). They are transcribed so the signature comparison is against
// the host oracle's arithmetic, not against this driver's interpreter.
//                                              sig0                sig1                sig2                sig3
struct Expected {
  uint64_t a;
  uint64_t b;
  uint64_t c;
  uint64_t sig[4];
};

struct Program {
  const char* name;
  const char* elf;
  const Expected inputs[3];
};

const Program kPrograms[2] = {
    {"p03_loadstore",
     "tests/programs/build/p03_loadstore.i%d.elf",
     {
         {UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210), UINT64_C(0x7),
          {UINT64_C(0x0123456788888888), UINT64_C(0x000000000000ffff),
           UINT64_C(0xffffffffffffffff), UINT64_C(0xffffffffffffffe6)}},
         {UINT64_C(0x8000000000000001), UINT64_C(0x180), UINT64_C(0xffffffffffffffff),
          {UINT64_C(0x7fffffff80000001), UINT64_C(0x0000000000000181),
           UINT64_C(0x0000000000000081), UINT64_C(0x8000000000000180)}},
         {UINT64_C(0x0), UINT64_C(0xffffffffffffffff), UINT64_C(0x8000000000000000),
          {UINT64_C(0x0000000000000000), UINT64_C(0xffffffffffffffff),
           UINT64_C(0x00000000000000ff), UINT64_C(0x7fffffffffffffff)}},
     }},
    {"p09_storeload",
     "tests/programs/build/p09_storeload.i%d.elf",
     {
         {UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210),
          UINT64_C(0xaaaaaaaaaaaaaaaa),
          {UINT64_C(0x0123456789abcdef), UINT64_C(0xffffffff89abcdef),
           UINT64_C(0x000000000000ff00), UINT64_C(0xaaaaaaaaaaaaaaaa)}},
         {UINT64_C(0xdeadbeefcafebabe), UINT64_C(0xffffffffffffffff), UINT64_C(0x0),
          {UINT64_C(0xdeadbeefcafebabe), UINT64_C(0xffffffffcafebabe),
           UINT64_C(0x0000000000004500), UINT64_C(0x0000000000000000)}},
         {UINT64_C(0xffffffffffffffff), UINT64_C(0x0), UINT64_C(0x5555555555555555),
          {UINT64_C(0xffffffffffffffff), UINT64_C(0xffffffffffffffff),
           UINT64_C(0x000000000000ff00), UINT64_C(0x5555555555555555)}},
     }},
};

constexpr int kInputsPerProgram = 3;

// ============================================================================
// The harness-authored scene
// ============================================================================
// Entered by the reset brick. It sets up a writable RAM base and a marker, then
// an always-taken branch whose fall-through is wrong path:
//
//   scene+0   auipc s3, 0            s3 = the scene's own pc (0x80010000)
//   scene+4   addi s3, s3, 0x200     s3 = 0x80010200 (scratch)
//   scene+8   addi t1, x0, 0x305     the marker a leak would write
//   scene+12  beq  x0, x0, +16       always taken -> scene+28
//   scene+16  sd   t1, 0(s3)         WRONG PATH, aligned: must leave no byte
//   scene+20  sd   t1, 4(s3)         WRONG PATH, crosses the 8-byte line
//   scene+24  sd   t1, 0(s3)         WRONG PATH again
//   scene+28  ld   t2, 0(s3)         the scratch must still read zero
//   scene+32  addi t0, x0, 0x11      the loop's first payload
//   scene+36  addi t2, x0, 2         two passes
//   scene+40  sd   t0, 8(s3)         SAME PC, SAME ADDRESS, different data each pass
//   scene+44  addi t0, t0, 1
//   scene+48  addi t2, t2, -1
//   scene+52  bne  t2, x0, -12       -> scene+40
//   scene+56  ld   t3, 8(s3)         must be the *second* pass's payload
//   scene+60  jal  x0, main          hand over to the corpus program
//
// The loop is the card's sharpest provenance case: two stores from the *same PC*
// to the *same address* with *different data*. A rule that named a producer by
// PC, or that keyed bytes by address alone, could not tell them apart and would
// pass a machine that replayed the first pass's bytes; this rule names them by
// retire_seq and by drain position, so the second pass's byte must come from the
// second pass's store.
struct Scene {
  uint64_t base = 0;
  uint64_t scratch = 0;
  uint64_t wrong_path_aligned = 0;
  uint64_t wrong_path_crossing = 0;
  uint64_t loop_store = 0;
};

Scene WriteScene(ProgImage* image, uint64_t main_entry, uint64_t reset_vector) {
  Scene s;
  s.base = kSceneBase;
  s.scratch = kSceneScratch;
  s.wrong_path_aligned = kSceneBase + 16;
  s.wrong_path_crossing = kSceneBase + 20;
  image->WriteWord(kSceneBase + 0, EncU(0, kS3, 0x17u));  // auipc s3, 0
  // The base is formed PC-relative on purpose: `lui 0x80010` would sign-extend
  // the 32-bit 0x80010000 and produce 0xFFFFFFFF80010000, and the harness must
  // build the same address the architecture does.
  if ((kSceneScratch - kSceneBase) != 0x200) {
    Fail("scene", "the scratch offset and the scene's addi immediate disagree");
  }
  image->WriteWord(kSceneBase + 4, EncI(0x200, kS3, 0x0u, kS3, 0x13u));
  image->WriteWord(kSceneBase + 8, EncI(0x305, kX0, 0x0u, kT1, 0x13u));
  image->WriteWord(kSceneBase + 12, EncB(16, kX0, kX0, 0x0u));
  image->WriteWord(kSceneBase + 16, EncS(0, kT1, kS3, 0x3u));
  image->WriteWord(kSceneBase + 20, EncS(4, kT1, kS3, 0x3u));
  image->WriteWord(kSceneBase + 24, EncS(0, kT1, kS3, 0x3u));
  image->WriteWord(kSceneBase + 28, EncI(0, kS3, 0x3u, kT2, 0x03u));
  image->WriteWord(kSceneBase + 32, EncI(0x11, kX0, 0x0u, kT0, 0x13u));
  image->WriteWord(kSceneBase + 36, EncI(2, kX0, 0x0u, kT2, 0x13u));
  image->WriteWord(kSceneBase + 40, EncS(8, kT0, kS3, 0x3u));
  image->WriteWord(kSceneBase + 44, EncI(1, kT0, 0x0u, kT0, 0x13u));
  image->WriteWord(kSceneBase + 48, EncI(-1, kT2, 0x0u, kT2, 0x13u));
  image->WriteWord(kSceneBase + 52, EncB(-12, kX0, kT2, 0x1u));  // bne t2, x0
  image->WriteWord(kSceneBase + 56, EncI(8, kS3, 0x3u, kT3, 0x03u));
  s.loop_store = kSceneBase + 40;
  const int64_t offset =
      static_cast<int64_t>(main_entry) - static_cast<int64_t>(kSceneBase + 60);
  if (offset < -(INT32_C(1) << 20) || offset >= (INT32_C(1) << 20)) {
    Fail("scene", "`main` is out of JAL range from the scene");
  }
  image->WriteWord(kSceneBase + 60, EncJ(static_cast<int32_t>(offset), kX0));
  if (kSceneBase <= reset_vector + 16) {
    Fail("scene", "the scene overlaps the reset bricks");
  }
  return s;
}

// ============================================================================
// The provenance records
// ============================================================================
struct CommittedStore {
  size_t index = 0;           // position in the committed sequence
  uint64_t retire_index = 0;  // position in the whole retire stream
  uint32_t seq = 0;           // the event stream's retire_seq (the causal id)
  uint64_t rob_id = 0;        // the core's per-lane {generation, tag} identity
  uint64_t pc = 0;
  uint64_t addr = 0;
  uint64_t data = 0;
  unsigned size = 0;          // bytes
  uint64_t commit_cycle = 0;
  bool drained = false;
  uint64_t drain_cycle = 0;
  uint64_t txn = 0;
};

struct VisibleByte {
  uint64_t addr = 0;
  uint8_t value = 0;
  size_t store = 0;
  uint64_t cycle = 0;
};

struct TraceLine {
  std::string text;
};

// The per-run and accumulated coverage the card names, measured rather than
// asserted in prose.
struct Coverage {
  uint64_t load_widths[4] = {0, 0, 0, 0};   // byte/half/word/double
  uint64_t store_widths[4] = {0, 0, 0, 0};
  uint64_t sign_extended = 0;
  uint64_t partial_masks = 0;
  uint64_t forwarded_bytes = 0;
  uint64_t directed_crossing_accesses = 0;
  uint64_t crossing_reached_memory = 0;
  uint64_t wrong_path_stores_directed = 0;
  uint64_t wrong_path_bytes_visible = 0;
  uint64_t same_pc_addr_pairs = 0;
  uint64_t bytes_written_twice = 0;
  uint64_t commit_before_drain = 0;
  uint64_t max_commit_drain_gap = 0;
  uint64_t min_commit_drain_gap = UINT64_MAX;
  uint64_t same_cycle_commit_drain = 0;
  uint64_t committed_without_drain_pairs = 0;
  uint64_t duplicate_drains = 0;
  uint64_t out_of_order_drains = 0;
  uint64_t unattributed_visibility = 0;
  uint64_t loads = 0;
  uint64_t load_source_checks = 0;
  uint64_t protocol_bytes = 0;
  uint64_t superseded_bytes = 0;
  uint64_t retires = 0;
  uint64_t stores = 0;

  void Add(const Coverage& o) {
    for (int i = 0; i < 4; i++) {
      load_widths[i] += o.load_widths[i];
      store_widths[i] += o.store_widths[i];
    }
    sign_extended += o.sign_extended;
    partial_masks += o.partial_masks;
    forwarded_bytes += o.forwarded_bytes;
    directed_crossing_accesses += o.directed_crossing_accesses;
    crossing_reached_memory += o.crossing_reached_memory;
    wrong_path_stores_directed += o.wrong_path_stores_directed;
    wrong_path_bytes_visible += o.wrong_path_bytes_visible;
    same_pc_addr_pairs += o.same_pc_addr_pairs;
    bytes_written_twice += o.bytes_written_twice;
    superseded_bytes += o.superseded_bytes;
    commit_before_drain += o.commit_before_drain;
    max_commit_drain_gap = std::max(max_commit_drain_gap, o.max_commit_drain_gap);
    min_commit_drain_gap = std::min(min_commit_drain_gap, o.min_commit_drain_gap);
    same_cycle_commit_drain += o.same_cycle_commit_drain;
    committed_without_drain_pairs += o.committed_without_drain_pairs;
    duplicate_drains += o.duplicate_drains;
    out_of_order_drains += o.out_of_order_drains;
    unattributed_visibility += o.unattributed_visibility;
    loads += o.loads;
    load_source_checks += o.load_source_checks;
    protocol_bytes += o.protocol_bytes;
    superseded_bytes += o.superseded_bytes;
    retires += o.retires;
    stores += o.stores;
  }
};

// ============================================================================
// The harness
// ============================================================================
class Harness {
 public:
  Harness(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, uint64_t max_cycles,
          const ProgImage* img, mosaic::MemoryModel* dut_mem,
          mosaic::MemoryModel* arch_seed, std::vector<TraceLine>* trace)
      : dut_(dut), reporter_(reporter), max_cycles_(max_cycles), imem_(img),
        dmem_(dut_mem), img_(img), arch_mem_(arch_seed), trace_(trace) {}

  void Configure(const Geometry& g) { g_ = g; }
  void Phase(const std::string& name) { phase_ = name; }
  void Expect(const std::vector<RefInsn>* trace) { expected_ = trace; }

  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) Cycle(true);
  }

  bool Complete() const { return retires_.size() >= expected_->size(); }
  uint64_t cycles() const { return cycles_; }
  size_t retires() const { return retires_.size(); }
  const std::vector<CommittedStore>& committed() const { return committed_; }
  const std::vector<VisibleByte>& visible() const { return visible_; }
  const std::vector<DataMem::Txn>& txns() const { return dmem_.txns(); }
  uint64_t write_txns() const { return write_txns_; }
  uint64_t read_txns() const { return read_txns_; }
  size_t drain_next() const { return drain_next_; }
  Coverage& coverage() { return cov_; }
  const Coverage& coverage() const { return cov_; }
  // The ledger's byte after every committed store has been applied; the final
  // per-byte comparison uses it.
  uint8_t LedgerByte(uint64_t addr) const { return arch_mem_.Byte(addr); }

  void Cycle(bool rst) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_ + " at cycle " + Dec(cycles_),
           "max-cycles (" + Dec(max_cycles_) + ") exhausted: " + State());
    }
    dut_->rst = rst ? 1 : 0;
    dut_->imem_req_ready_i = 1;
    if (imem_.HasResponse()) {
      const Imem::Request& r = imem_.Response();
      dut_->imem_rsp_valid_i = 1;
      dut_->imem_rsp_rdata_i = static_cast<uint32_t>(imem_.ResponseWord());
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

    if (!rst && (dut_->dmem_req_valid_o != 0) && (dut_->dmem_req_ready_i != 0)) {
      DataMem::Request r;
      r.we = dut_->dmem_req_we_o != 0;
      r.addr = dut_->dmem_req_addr_o;
      r.size = dut_->dmem_req_size_o;
      r.wstrb = dut_->dmem_req_wstrb_o;
      r.wdata = dut_->dmem_req_wdata_o;
      // The memory model performs the access on acceptance (the endpoint's own
      // "the transaction is owned from acceptance" rule), so the transaction is
      // applied first and then observed: the byte the port carried and the byte
      // memory now holds are compared against each other.
      dmem_.Accept(r, cycles_);
      ObserveTransaction(r);
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
    ++cycles_;
  }

  std::string State() const {
    return "head_pc=" + U64(dut_->o_dbg_head_pc_o) +
           " head_valid=" + Dec(dut_->o_dbg_head_valid_o) +
           " occupied=" + Dec(dut_->o_rob_occupied_o) +
           " retired=" + Dec(dut_->o_commit_o) + " stopped=" + Dec(dut_->o_stopped_o) +
           " lq=" + Dec(dut_->o_mem_lq_occupied_o) + " sq=" +
           Dec(dut_->o_mem_sq_occupied_o) + " sq_alloc=" + Dec(dut_->o_mem_sq_alloc_o) +
           " sq_auth=" + Dec(dut_->o_mem_sq_auth_o) + " sq_drain=" +
           Dec(dut_->o_mem_sq_drain_o);
  }

  void Check(const std::string& what, bool ok, const std::string& detail) {
    reporter_->Check(ok, phase_ + ": " + what + (ok ? "" : " -- " + detail));
    if (!ok) Fail(phase_ + " at cycle " + Dec(cycles_), what + ": " + detail);
  }

 private:
  struct Retire {
    uint64_t pc = 0;
    uint32_t rd = 0;
    bool reg_we = false;
    uint64_t value = 0;
    uint32_t seq = 0;
    uint64_t rob_id = 0;
    bool is_store = false;
    uint64_t store_addr = 0;
    uint64_t store_data = 0;
    unsigned store_size = 0;
  };

  // ------------------------------------------------------------- the trace
  std::string FormatRecord(const mosaic::EventRecord& record) const {
    uint8_t encoded[mosaic::kEventRecordBytes];
    size_t written = 0;
    const mosaic::EncodeStatus status =
        mosaic::EncodeEvent(record, MOSAIC_EVENT_SCHEMA_VERSION, encoded,
                            sizeof(encoded), &written);
    if (status != mosaic::EncodeStatus::kOk || written != mosaic::kEventRecordBytes) {
      Fail(phase_ + " trace", std::string("the event encoder refused a record: ") +
                                  mosaic::EncodeStatusName(status));
    }
    // The round trip: decode the octets back and require every declared field to
    // survive, so the trace is the frozen schema and not a lookalike.
    mosaic::EventRecord back{};
    std::memset(&back, 0, sizeof(back));
    const mosaic::EncodeStatus dstatus =
        mosaic::DecodeEvent(encoded, written, MOSAIC_EVENT_SCHEMA_VERSION, &back);
    if (dstatus != mosaic::EncodeStatus::kOk) {
      Fail(phase_ + " trace", std::string("the event decoder refused a record: ") +
                                  mosaic::EncodeStatusName(dstatus));
    }
    for (size_t i = 0; i < mosaic::EventFieldCount(); ++i) {
      if (mosaic::EventFieldValue(back, i) != mosaic::EventFieldValue(record, i)) {
        Fail(phase_ + " trace", std::string("the codec round trip changed a field: ") +
                                    mosaic::EventFieldName(i));
      }
    }
    g_codec_records++;
    g_codec_round_trips++;
    char text[1024];
    const size_t n = mosaic::FormatEvent(record, text, sizeof(text));
    if (n == 0) Fail(phase_ + " trace", "the event formatter refused a record");
    return std::string(text, n);
  }

  // Name a producer by the identity the machine itself carries: its position in
  // the architectural retire order (retire_seq, the schema's causal id) and the
  // per-lane {generation, tag} the core presents on ev_id. The pc is printed
  // too, but the rule never keys on it: two stores to the same address from the
  // same pc are told apart by retire_seq and by their position in the drain
  // order.
  static std::string StoreName(const CommittedStore& s) {
    return "retire_seq=" + Dec(s.seq) + " rob_id=" + U64(s.rob_id, 4) + " pc=" + U64(s.pc);
  }

  static uint8_t SizeCode(unsigned bytes) {
    return static_cast<uint8_t>(bytes == 8   ? 3u
                                : bytes == 4 ? 2u
                                : bytes == 2 ? 1u
                                             : 0u);
  }

  void EmitStoreCommit(const CommittedStore& s) {
    if (trace_ == nullptr) return;
    mosaic::EventRecord record{};
    std::memset(&record, 0, sizeof(record));
    record.retire_seq = static_cast<uint8_t>(s.seq & 0xFFu);
    record.hart_id = 0;
    record.cycle = s.commit_cycle;
    record.kind = static_cast<uint8_t>(mosaic::kEventKindRETIRE);
    record.pc_before = s.pc;
    record.insn_bits = img_->Word(s.pc);
    record.insn_len = 4;
    record.mem_is_store = 1;
    record.mem_addr = s.addr;
    record.mem_data = s.data;
    record.mem_size = SizeCode(s.size);
    trace_->push_back(
        TraceLine{"store-commit " + FormatRecord(record) + " lane_id=" + U64(s.rob_id, 4)});
  }

  void EmitVisible(const CommittedStore& s) {
    if (trace_ == nullptr) return;
    mosaic::EventRecord record{};
    std::memset(&record, 0, sizeof(record));
    record.retire_seq = static_cast<uint8_t>(s.seq & 0xFFu);
    record.hart_id = 0;
    record.cycle = s.drain_cycle;
    record.kind = static_cast<uint8_t>(mosaic::kEventKindMEM_VISIBLE);
    record.pc_before = s.pc;
    record.mem_is_store = 1;
    record.mem_addr = s.addr;
    record.mem_data = s.data;
    record.mem_size = SizeCode(s.size);
    trace_->push_back(
        TraceLine{"mem-visible " + FormatRecord(record) + " lane_id=" + U64(s.rob_id, 4)});
  }

  void EmitLoad(const LoadDecode& d, uint32_t seq, uint64_t value,
                const std::string& source) {
    if (trace_ == nullptr) return;
    trace_->push_back(TraceLine{"load retire_seq=" + Dec(seq) + " pc=" + U64(d.pc) +
                                " addr=" + U64(d.addr) + " size=" + Dec(d.size) +
                                (d.sign ? " signed=1" : " signed=0") +
                                " value=" + U64(value) + " source=" + source});
  }

  // ------------------------------------------------------------- the observer
  void Observe() {
    Check("the retire counter equals the event stream published so far",
          dut_->o_commit_o == retires_.size(),
          "counter=" + Dec(dut_->o_commit_o) + " events=" + Dec(retires_.size()));
    if (kCompareMemory) {
      Check("the store queue's drains and its occupancy are within its allocations",
            dut_->o_mem_sq_drain_o + dut_->o_mem_sq_occupied_o <= dut_->o_mem_sq_alloc_o,
            "drain=" + Dec(dut_->o_mem_sq_drain_o) + " occupied=" +
                Dec(dut_->o_mem_sq_occupied_o) + " alloc=" + Dec(dut_->o_mem_sq_alloc_o));
      // The per-cycle form of the central rule: the number of write transactions
      // memory has seen can never exceed the number of stores that have committed.
      Check("no store is visible before a store has committed",
            write_txns_ <= committed_.size(),
            "write transactions=" + Dec(write_txns_) + " committed stores=" +
                Dec(committed_.size()));
    }

    const uint32_t mask =
        (g_.retire_width >= 32) ? 0xFFFFFFFFu : ((1u << g_.retire_width) - 1u);
    const uint32_t got_mask = static_cast<uint32_t>(dut_->ev_valid_o) & mask;
    for (uint32_t lane = 0; lane < g_.retire_width && lane < 32; lane++) {
      if ((got_mask & (1u << lane)) == 0) continue;
      Retire r;
      r.pc = PayloadLane(dut_->ev_pc_o, lane);
      r.value = PayloadLane(dut_->ev_value_o, lane);
      r.rd = static_cast<uint32_t>(PackedLane(dut_->ev_rd_o, lane, 5));
      r.seq = static_cast<uint32_t>(PackedLane(dut_->ev_seq_o, lane, g_.seq_w));
      r.rob_id = (g_.ret_id_w > 0 && g_.ret_id_w <= 64)
                     ? PackedLane(dut_->ev_id_o, lane, g_.ret_id_w)
                     : 0;
      r.reg_we = PackedLane(dut_->ev_reg_we_o, lane, 1) != 0;
      r.is_store = PackedLane(dut_->ev_store_o, lane, 1) != 0;
      r.store_addr = PayloadLane(dut_->ev_store_addr_o, lane);
      r.store_data = PayloadLane(dut_->ev_store_data_o, lane);
      r.store_size = static_cast<unsigned>(PackedLane(dut_->ev_store_size_o, lane, 3));

      const RefInsn* expected = nullptr;
      if (expected_ != nullptr && retires_.size() < expected_->size()) {
        expected = &(*expected_)[retires_.size()];
      }
      if (expected != nullptr && r.pc != expected->pc) {
        Fail(phase_ + " at cycle " + Dec(cycles_),
             "the retirement stream follows the reference: retire " +
                 Dec(retires_.size()) + " pc expected " + U64(expected->pc) + ", got " +
                 U64(r.pc));
      }
      // A store's retirement carries its payload; a retiring store with no
      // payload has no producer and is a failure of the record, not a pass.
      if (kCompareMemory && expected != nullptr && expected->is_store && !r.is_store) {
        Fail(phase_ + " at cycle " + Dec(cycles_),
             "a retiring store carries its payload, so its bytes have a producer: "
             "retire " +
                 Dec(retires_.size()) + " at " + U64(r.pc) +
                 " retired with the store payload valid bit clear");
      }
      if (kCompareMemory && expected != nullptr && !expected->is_store && r.is_store) {
        Fail(phase_ + " at cycle " + Dec(cycles_),
             "the store payload bus is valid only for a store: retire " +
                 Dec(retires_.size()) + " at " + U64(r.pc) +
                 " is not a store but the payload is valid");
      }

      if (r.is_store && !kCompareMemory) {
        // The rd-only control keeps the retirement stream and nothing else. The
        // store's payload is not recorded because nothing compares it.
      } else if (r.is_store) {
        CommittedStore s;
        s.index = committed_.size();
        s.retire_index = retires_.size();
        s.seq = r.seq;
        s.rob_id = r.rob_id;
        s.pc = r.pc;
        s.addr = r.store_addr;
        s.data = r.store_data;
        s.size = SizeBytes(r.store_size);
        s.commit_cycle = cycles_;
        if (s.seq >= 256u) {
          Fail(phase_ + " at cycle " + Dec(cycles_),
               "the run retires more than the retire_seq field's 256 values, so the "
               "causal id would wrap; shorten the run");
        }
        committed_.push_back(s);
        arch_mem_.ApplyStore(s.addr, s.data, s.size);
        EmitStoreCommit(s);
      } else if (expected != nullptr && expected->is_load) {
        const LoadDecode d = DecodeLoad(*img_, r.pc, regs_);
        if (!d.is_load) {
          Fail(phase_ + " at cycle " + Dec(cycles_),
               "the reference says retire " + Dec(retires_.size()) + " at " + U64(r.pc) +
                   " is a load but the image word does not decode as one");
        }
        if (IsProtocolAddress(d.addr)) {
          // A load of the protocol register is not a memory read and is not
          // covered by this ledger; no corpus program performs one.
          retires_.push_back(r);
          continue;
        }
        const uint64_t raw = arch_mem_.RawValue(d.addr, d.size);
        const uint64_t want = (d.sign && d.size < 8) ? SignExtend(raw, d.size) : raw;
        cov_.loads++;
        cov_.load_source_checks++;
        cov_.load_widths[d.size == 1 ? 0u : d.size == 2 ? 1u : d.size == 4 ? 2u : 3u]++;
        if (d.sign && d.size < 8 && want != raw) cov_.sign_extended++;
        EmitLoad(d, r.seq, r.value, LoadSourceName(d.addr, d.size));
        if (kCompareMemory && r.reg_we && r.value != want) {
          Fail(phase_ + " at cycle " + Dec(cycles_),
               "a load's returned value matches an allowed source: retire " +
                   Dec(retires_.size()) + " at " + U64(r.pc) + " size=" + Dec(d.size) +
                   (d.sign ? " signed" : " unsigned") + " addr=" + U64(d.addr) +
                   " returned " + U64(r.value) + ", but the " +
                   LoadSourceName(d.addr, d.size) + " holds " + U64(want));
        }
      }
      if (r.reg_we && r.rd != 0) regs_[r.rd] = r.value;

      // The value comparison against the reference, for every retirement.
      if (expected != nullptr) {
        if (expected->reg_we != r.reg_we || expected->rd != r.rd) {
          Fail(phase_ + " at cycle " + Dec(cycles_),
               "the retirement stream follows the reference: retire " +
                   Dec(retires_.size()) + " at " + U64(r.pc) +
                   " destination expected rd=" + Dec(expected->rd) + " we=" +
                   Dec(expected->reg_we) + ", got rd=" + Dec(r.rd) + " we=" +
                   Dec(r.reg_we));
        }
        if (expected->reg_we && r.value != expected->value) {
          Fail(phase_ + " at cycle " + Dec(cycles_),
               "the retirement stream follows the reference: retire " +
                   Dec(retires_.size()) + " at " + U64(r.pc) + " value for x" +
                   Dec(r.rd) + " expected " + U64(expected->value) + ", got " +
                   U64(r.value) + (expected->is_load ? " (a load)" : ""));
        }
      }
      retires_.push_back(r);
    }
    ProgressCheck();
  }

  // The committed store (or the image) the byte at `addr` most recently came
  // from, for a failure message.
  std::string LoadSourceName(uint64_t addr, unsigned size) const {
    for (auto it = committed_.rbegin(); it != committed_.rend(); ++it) {
      if (addr >= it->addr && addr + size <= it->addr + it->size) {
        return "most recent committed store " + StoreName(*it);
      }
    }
    return "memory image";
  }

  // ------------------------------------------------- the data-port observer
  void ObserveTransaction(const DataMem::Request& req) {
    if (!kCompareMemory) {
      // The rd-only control: the data port is still exercised (the memory model
      // performs every transaction) but nothing about it is compared.
      if (req.we) {
        ++write_txns_;
      } else {
        ++read_txns_;
      }
      return;
    }
    const unsigned bytes = SizeBytes(req.size);
    if ((req.addr % bytes) != 0) {
      cov_.crossing_reached_memory++;
      Fail(phase_ + " at cycle " + Dec(cycles_),
           "an access crossing a line boundary never reaches memory: a " +
               std::string(req.we ? "store" : "load") + " at " + U64(req.addr) +
               " size=" + Dec(bytes) + " was offered to the data port misaligned");
    }
    if (!req.we) {
      ++read_txns_;
      return;
    }
    ++write_txns_;

    // The producer is identified by *position*, not by content: the store queue's
    // drain takes position 0, so the k-th visible store must be the k-th committed
    // store. Matching by payload instead would be ambiguous -- two stores to the
    // same address can carry the same data (the scene's loop and p09's
    // round-trip both do) -- and ambiguity is exactly what a provenance rule must
    // not have. The payload match below is then a check on an identity already
    // established by position.
    if (drain_next_ >= committed_.size()) {
      cov_.unattributed_visibility++;
      Fail(phase_ + " at cycle " + Dec(cycles_),
           "every visible byte has a legal, committed producer: a write to " +
               U64(req.addr) + " size=" + Dec(bytes) +
               " reached memory with no committed store to attribute it to" +
               DescribeOutstanding());
    }
    CommittedStore& s = committed_[drain_next_];
    if (!TxMatches(req, s)) {
      const size_t other = FindMatch(req);
      if (other != kNoMatch && other < drain_next_) {
        cov_.duplicate_drains++;
        const CommittedStore& d = committed_[other];
        Fail(phase_ + " at cycle " + Dec(cycles_),
             "a byte written twice by two producers: the store " + StoreName(d) +
                 " was already drained at cycle " + Dec(d.drain_cycle) +
                 " and its bytes are visible again at " + U64(req.addr));
      }
      if (other != kNoMatch) {
        cov_.out_of_order_drains++;
        Fail(phase_ + " at cycle " + Dec(cycles_),
             "the visible store is the oldest committed store not yet drained: a store "
             "at " +
                 U64(req.addr) + " size=" + Dec(bytes) + " drained while the older " +
                 "committed store " + StoreName(s) + " had not");
      }
      cov_.unattributed_visibility++;
      Fail(phase_ + " at cycle " + Dec(cycles_),
           "every visible byte has a legal, committed producer: a write to " +
               U64(req.addr) + " size=" + Dec(bytes) +
               " reached memory and does not match the committed store " + StoreName(s) +
               " that is next in commit order" + DescribeOutstanding());
    }
    // (3) the strobes are exactly the bytes the store's size owns.
    const unsigned low = static_cast<unsigned>(s.addr & 7u);
    const unsigned expected_strb = ((1u << s.size) - 1u) << low;
    if (req.wstrb != (expected_strb & 0xFFu) || bytes != s.size) {
      Fail(phase_ + " at cycle " + Dec(cycles_),
           "the drain's byte mask is the store's own size: store " + StoreName(s) +
               " size=" + Dec(s.size) +
               " expected strobes " + U64(expected_strb & 0xFFu) +
               ", the data port carried strobes " + U64(req.wstrb) + " size=" +
               Dec(bytes));
    }
    // (2) the producer committed no later than the visibility cycle.
    if (cycles_ < s.commit_cycle) {
      Fail(phase_ + " at cycle " + Dec(cycles_),
           "no byte becomes visible before its commit: store " + StoreName(s) +
               " committed at cycle " + Dec(s.commit_cycle) +
               " but its bytes are visible at cycle " + Dec(cycles_));
    }
    cov_.commit_before_drain++;
    const uint64_t gap = cycles_ - s.commit_cycle;
    cov_.max_commit_drain_gap = std::max(cov_.max_commit_drain_gap, gap);
    cov_.min_commit_drain_gap = std::min(cov_.min_commit_drain_gap, gap);
    if (gap == 0) cov_.same_cycle_commit_drain++;
    // How many committed-and-not-yet-drained stores the queue is holding at this
    // drain: the "commit is ahead of drain" depth the card's separation measures.
    if (committed_.size() > drain_next_ + 1) {
      cov_.committed_without_drain_pairs += committed_.size() - (drain_next_ + 1);
    }
    if (s.size < 8) cov_.partial_masks++;
    cov_.store_widths[s.size == 1 ? 0u : s.size == 2 ? 1u : s.size == 4 ? 2u : 3u]++;

    // (4) every byte: the payload byte, the ledger's byte and the memory's byte.
    const uint64_t base = req.addr & ~UINT64_C(7);
    for (unsigned lane = 0; lane < 8; ++lane) {
      if (((req.wstrb >> lane) & 1u) == 0u) continue;
      const uint64_t addr = base + lane;
      const unsigned offset = lane - low;
      const uint8_t payload = static_cast<uint8_t>((s.data >> (8 * offset)) & 0xFFu);
      const uint8_t from_port = static_cast<uint8_t>((req.wdata >> (8 * lane)) & 0xFFu);
      if (from_port != payload) {
        Fail(phase_ + " at cycle " + Dec(cycles_),
             "the visible byte is the producing store's own byte: store " +
                 StoreName(s) + " byte at " + U64(addr) +
                 " expected " + U64(payload) + ", the data port carried " +
                 U64(from_port));
      }
      if (IsProtocolAddress(addr)) {
        cov_.protocol_bytes++;
        continue;
      }
      // The ledger's *final* value for this byte. When it differs from the
      // payload, a newer committed store covering the same byte has committed but
      // not drained yet: the in-order drain legitimately writes the older
      // producer's byte first, and the newer store's drain will overwrite it. That
      // is measured (and required to end up right by the final comparison), not
      // treated as a mismatch.
      const uint8_t ledger = arch_mem_.Byte(addr);
      if (ledger != payload) cov_.superseded_bytes++;
      uint64_t visible = 0;
      if (dmem_.model()->Read(addr, 1, &visible) != mosaic::AccessStatus::kOk) {
        Fail(phase_ + " at cycle " + Dec(cycles_),
             "the visible byte at " + U64(addr) + " is readable");
      }
      if (static_cast<uint8_t>(visible) != payload) {
        Fail(phase_ + " at cycle " + Dec(cycles_),
             "the byte memory holds after the drain is the producer's byte: byte at " +
                 U64(addr) + " expected " + U64(payload) + ", memory holds " +
                 U64(visible));
      }
      VisibleByte vb;
      vb.addr = addr;
      vb.value = payload;
      vb.store = s.index;
      vb.cycle = cycles_;
      visible_.push_back(vb);
      auto& list = producers_[addr];
      list.push_back(s.index);
      if (list.size() > 1) cov_.bytes_written_twice++;
    }
    s.drained = true;
    s.drain_cycle = cycles_;
    s.txn = write_txns_ - 1;
    drain_next_ = s.index + 1;
    EmitVisible(s);
  }

  static constexpr size_t kNoMatch = static_cast<size_t>(-1);

  // Does this transaction carry exactly this store's bytes?
  static bool TxMatches(const DataMem::Request& req, const CommittedStore& s) {
    if (s.addr != req.addr || s.size != SizeBytes(req.size)) return false;
    const unsigned low = static_cast<unsigned>(s.addr & 7u);
    const unsigned strb = ((1u << s.size) - 1u) << low;
    if (req.wstrb != (strb & 0xFFu)) return false;
    for (unsigned lane = 0; lane < 8; ++lane) {
      if (((req.wstrb >> lane) & 1u) == 0u) continue;
      const unsigned offset = lane - low;
      const uint8_t payload = static_cast<uint8_t>((s.data >> (8 * offset)) & 0xFFu);
      const uint8_t from_port = static_cast<uint8_t>((req.wdata >> (8 * lane)) & 0xFFu);
      if (payload != from_port) return false;
    }
    return true;
  }

  // A diagnosis-only search: which committed store, if any, does this
  // transaction's payload belong to? Used only after the positional match has
  // already failed, to say whether the transaction is a duplicate drain (a store
  // already drained) or an out-of-order one.
  size_t FindMatch(const DataMem::Request& req) const {
    for (size_t i = 0; i < committed_.size(); ++i) {
      if (TxMatches(req, committed_[i])) return i;
    }
    return kNoMatch;
  }

  std::string DescribeOutstanding() const {
    if (drain_next_ >= committed_.size()) {
      return " (no committed store is outstanding)";
    }
    const CommittedStore& s = committed_[drain_next_];
    return " (the oldest un-drained committed store is " + StoreName(s) +
           " addr=" + U64(s.addr) + " size=" + Dec(s.size) + ")";
  }

  void ProgressCheck() {
    const bool progressed = (dut_->o_commit_o != last_commit_) ||
                            (dut_->o_wb_pub_valid_o != 0) ||
                            (dut_->o_dbg_alloc_ctr_o != last_alloc_);
    last_commit_ = dut_->o_commit_o;
    last_alloc_ = dut_->o_dbg_alloc_ctr_o;
    if (progressed) {
      last_progress_ = cycles_;
      return;
    }
    if (cycles_ - last_progress_ > kStallCycles) {
      Fail(phase_ + " at cycle " + Dec(cycles_), "stalled: " + State());
    }
  }

  Vmosaic_core_tb* dut_;
  mosaic::Reporter* reporter_;
  uint64_t max_cycles_;
  Imem imem_;
  DataMem dmem_;
  const ProgImage* img_;
  ArchMem arch_mem_;
  std::vector<TraceLine>* trace_;
  Geometry g_;
  std::string phase_;
  uint64_t cycles_ = 0;
  const std::vector<RefInsn>* expected_ = nullptr;
  std::vector<Retire> retires_;
  std::vector<CommittedStore> committed_;
  std::vector<VisibleByte> visible_;
  std::map<uint64_t, std::vector<size_t>> producers_;
  uint64_t regs_[32] = {};
  size_t drain_next_ = 0;
  uint64_t write_txns_ = 0;
  uint64_t read_txns_ = 0;
  uint64_t last_commit_ = 0;
  uint64_t last_alloc_ = 0;
  uint64_t last_progress_ = 0;
  Coverage cov_;
};

// ============================================================================
// The repository root
// ============================================================================
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

// ============================================================================
// One run
// ============================================================================
struct RunOutcome {
  uint64_t cycles = 0;
  size_t retires = 0;
  uint64_t loads = 0;
  uint64_t stores = 0;
  uint64_t write_txns = 0;
  uint64_t read_txns = 0;
};

void RunOnce(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, uint64_t max_cycles,
             const std::string& elf_path, const Expected& input, int program_index,
             int input_index, bool scene_run, const Geometry& geometry,
             std::vector<TraceLine>* trace, Coverage* totals) {
  RunOutcome outcome;

  // ---- the image, the reset bricks, and (for a scene run) the scene ----
  ProgImage image;
  std::string load_detail;
  if (!image.Load(elf_path, &load_detail)) {
    Fail("program", load_detail + " (build the corpus with `make -C tests/programs all`)");
  }
  const mosaic::Symbol* main_symbol = image.Symbol("main");
  if (main_symbol == nullptr) {
    Fail("program", elf_path + " has no `main` symbol, so the harness has no entry "
                               "point to jump to");
  }
  const uint64_t entry = main_symbol->value;
  if (geometry.reset_vector == entry) {
    Fail("geometry", "the reset vector and the program's entry are the same address");
  }
  Scene scene;
  if (scene_run) scene = WriteScene(&image, entry, geometry.reset_vector);
  const uint64_t target = scene_run ? scene.base : entry;
  const int64_t offset =
      static_cast<int64_t>(target) - static_cast<int64_t>(geometry.reset_vector);
  if (offset < -(INT32_C(1) << 20) || offset >= (INT32_C(1) << 20)) {
    Fail("program", "the entry is out of JAL range from the reset vector");
  }
  image.WriteWord(geometry.reset_vector, EncJ(static_cast<int32_t>(offset), 0));
  // The two words after the entry branch are its fall-through, which is wrong
  // path. They are a system instruction the machine refuses while no trap vector
  // is installed (`ecall`, dispatch's l0_trap_unarmed), so they are fetched, held
  // by the branch barrier and discarded by the redirect, and nothing else is
  // claimed for them. A *store* here is deliberately not used: address 0 is the
  // read-only boot ROM, so a store there cannot become a visible byte even if it
  // leaked (the memory model refuses the write and the endpoint may refuse the
  // access), and the card's wrong-path coverage is therefore placed in the scene,
  // where the target is writable RAM the wrong-path store owns.
  image.WriteWord(geometry.reset_vector + 4, 0x00000073u);  // ecall (refused, wrong path)
  image.WriteWord(geometry.reset_vector + 8, 0x00000073u);  // ecall (refused, wrong path)

  // ---- the three memories: the DUT's, the reference's, the ledger's seed ----
  mosaic::MemoryModel dut_mem;
  mosaic::MemoryModel ref_mem;
  mosaic::MemoryModel arch_seed;
  {
    std::string detail;
    if (!dut_mem.LoadImage(image.image(), &detail)) Fail("run", detail);
    if (!ref_mem.LoadImage(image.image(), &detail)) Fail("run", detail);
    if (!arch_seed.LoadImage(image.image(), &detail)) Fail("run", detail);
  }

  // ---- the independent expectation ----
  const RefResult reference =
      mosaic_ref::RunReference(image, geometry.reset_vector, &ref_mem);
  if (!reference.exited) {
    Fail("run", std::string("the reference did not reach the program's exit protocol: ") +
                    reference.stop_reason + " at " + U64(reference.stop_pc));
  }

  const std::string ph = "run-" + std::string(kPrograms[program_index].name) + "-in" +
                         Dec(input_index) + (scene_run ? "-scene" : "");
  Harness harness(dut, reporter, max_cycles, &image, &dut_mem, &arch_seed, trace);
  harness.Configure(geometry);
  harness.Phase(ph);
  harness.Expect(&reference.trace);

  dut->clk = 0;
  dut->rst = 1;
  dut->eval();
  harness.Reset(kResetCycles);

  while (!harness.Complete()) {
    harness.Cycle(false);
    if (harness.cycles() > kTraceCycles) {
      Fail(ph, "the program did not reach its exit protocol within " + Dec(kTraceCycles) +
                   " cycles: " + harness.State());
    }
  }
  // A few settle cycles with comparisons still armed, so the last recovery's
  // counters land.
  for (int i = 0; i < 4; i++) harness.Cycle(false);

  const uint64_t cycles_at_end = harness.cycles();
  const uint64_t sq_fault = dut->o_mem_sq_fault_o;
  const uint64_t sq_occupied = dut->o_mem_sq_occupied_o;
  const uint64_t sq_auth = dut->o_mem_sq_auth_o;
  const uint64_t lq_fault = dut->o_mem_lq_fault_o;
  const uint64_t lq_fwd_bytes = kCompareMemory ? dut->o_mem_lq_fwd_bytes_o : 0;
  const uint64_t lsu_misaligned = dut->o_mem_lsu_misaligned_o;
  const uint64_t lsu_access_fault = dut->o_mem_lsu_access_fault_o;
  const uint64_t lsu_txn = dut->o_mem_lsu_txn_o;
  const uint64_t pmp_deny = dut->o_pmp_deny_ctr_o;
  const uint64_t pmp_store_deny = dut->o_pmp_store_deny_ctr_o;

  // Everything after the predicted stream is the program's own park loop: run
  // with the trace comparison off until the authorised stores have reached
  // memory (the exit store may still be draining).
  for (int i = 0; i < kDrainCycles && !dut_mem.finished(); i++) harness.Cycle(false);

  outcome.cycles = harness.cycles();
  outcome.retires = harness.retires();
  outcome.loads = reference.loads;
  outcome.stores = reference.stores;
  outcome.write_txns = harness.write_txns();
  outcome.read_txns = harness.read_txns();

  // ---- 1. the whole predicted stream retired ----
  harness.Check("the per-instruction retire stream matches the independent RV64IM "
                "interpretation (" +
                    Dec(reference.trace.size()) + " instructions, " +
                    Dec(reference.loads) + " loads, " + Dec(reference.stores) + " stores)",
                harness.retires() == reference.trace.size(),
                "retired " + Dec(harness.retires()) + ", predicted " +
                    Dec(reference.trace.size()));

  // ---- 2. the commit/drain separation, measured ----
  if (kCompareMemory) {
  size_t undrained = 0;
  for (const CommittedStore& s : harness.committed()) {
    if (!s.drained) undrained++;
  }
  harness.Check("every committed store drained exactly once: writes become visible in "
                "commit order, one drain each",
                harness.write_txns() == harness.committed().size() && undrained == 0,
                "committed=" + Dec(harness.committed().size()) + " drained=" +
                    Dec(harness.write_txns()) + " still resident=" + Dec(undrained));
  harness.Check("no visible byte appeared before its producer committed",
                harness.coverage().commit_before_drain == harness.write_txns(),
                "checked=" + Dec(harness.coverage().commit_before_drain) + " writes=" +
                    Dec(harness.write_txns()));
  harness.Check("the store queue is empty at the end and no store faulted",
                sq_occupied == 0 && sq_auth == 0 && sq_fault == 0,
                "occupied=" + Dec(sq_occupied) + " auth=" + Dec(sq_auth) + " fault=" +
                    Dec(sq_fault));
  }

  // ---- 3. the machine's memory equals the reference's, byte for byte ----
  if (kCompareMemory) {
  // The address set is every byte that became visible, plus the signature area
  // and (for a scene run) the scene's wrong-path addresses, so a byte only the
  // DUT wrote is compared too.
  std::set<uint64_t> compare_addrs;
  for (const VisibleByte& v : harness.visible()) compare_addrs.insert(v.addr);
  for (uint64_t a = MOSAIC_SIGNATURE_ADDR;
       a < MOSAIC_SIGNATURE_ADDR + 8 * MOSAIC_SIGNATURE_WORDS; ++a) {
    compare_addrs.insert(a);
  }
  if (scene_run) {
    for (uint64_t a = kSceneScratch; a < kSceneScratch + kSceneScratchSpan; ++a) {
      compare_addrs.insert(a);
    }
  }
  uint64_t compared = 0;
  uint64_t diverged = 0;
  uint64_t ledger_diverged = 0;
  std::string first_divergence;
  std::string first_ledger_divergence;
  for (uint64_t addr : compare_addrs) {
    uint64_t want = 0, got = 0;
    if (ref_mem.Read(addr, 1, &want) != mosaic::AccessStatus::kOk) continue;
    if (dut_mem.Read(addr, 1, &got) != mosaic::AccessStatus::kOk) continue;
    compared++;
    if (want != got) {
      diverged++;
      if (first_divergence.empty()) {
        first_divergence = "address " + U64(addr) + " expected " + U64(want) +
                           " (the reference), the machine left " + U64(got);
      }
    }
    const uint8_t ledger = (IsProtocolAddress(addr)) ? static_cast<uint8_t>(want)
                                                     : harness.LedgerByte(addr);
    if (ledger != static_cast<uint8_t>(want)) {
      ledger_diverged++;
      if (first_ledger_divergence.empty()) {
        first_ledger_divergence = "address " + U64(addr) + " expected " + U64(want) +
                                  " (the reference), the committed ledger holds " +
                                  U64(ledger);
      }
    }
  }
  harness.Check("the machine's memory equals the independent reference's, byte for "
                "byte, over every address either wrote",
                diverged == 0,
                first_divergence.empty()
                    ? ("compared " + Dec(compared) + " bytes")
                    : (Dec(diverged) + " of " + Dec(compared) + " bytes differ: " +
                       first_divergence));
  // Every byte's final value is the value the *newest* committed store covering
  // it wrote: no superseded producer's byte is left standing, which is the card's
  // "most recent committed store covering that byte" property over the run.
  harness.Check("every byte's final value is the newest committed store's byte (the "
                "ledger) and the reference's",
                ledger_diverged == 0,
                first_ledger_divergence.empty()
                    ? ("compared " + Dec(compared) + " bytes")
                    : (Dec(ledger_diverged) + " of " + Dec(compared) +
                       " bytes differ: " + first_ledger_divergence));

  }
  // ---- 4. the signature, the run's published result ----
  {
    std::vector<uint64_t> signature;
    if (!dut_mem.ReadSignature(&signature) || signature.size() != 4) {
      Fail(ph, "the signature area is not readable in the memory model");
    }
    for (int k = 0; k < 4; k++) {
      harness.Check("signature word " + Dec(k) + " equals the host oracle's",
                    signature[k] == input.sig[k],
                    "expected " + U64(input.sig[k]) + ", the machine's stores left " +
                        U64(signature[k]));
    }
    harness.Check("the program wrote TOHOST with the PASS bit set",
                  dut_mem.finished() && dut_mem.passed() && dut_mem.exit_code() == 1,
                  "finished=" + Dec(dut_mem.finished() ? 1 : 0) + " passed=" +
                      Dec(dut_mem.passed() ? 1 : 0) + " tohost=" + U64(dut_mem.exit_code()));
  }

  // ---- 5. the data port: exact transaction accounting ----
  if (kCompareMemory) {
  harness.Check("exactly one data transaction per load and per store reached the data "
                "port",
                harness.txns().size() == reference.loads + reference.stores &&
                    lsu_txn == reference.loads + reference.stores,
                "data transactions=" + Dec(harness.txns().size()) + " (lsu=" +
                    Dec(lsu_txn) + "), loads=" + Dec(reference.loads) +
                    " stores=" + Dec(reference.stores));
  harness.Check("no access was misaligned, none faulted and no PMP entry refused one",
                lsu_misaligned == 0 && lq_fault == 0 && lsu_access_fault == 0 &&
                    pmp_deny == 0 && pmp_store_deny == 0,
                "misaligned=" + Dec(lsu_misaligned) + " lq_fault=" + Dec(lq_fault) +
                    " access_fault=" + Dec(lsu_access_fault) + " pmp_deny=" +
                    Dec(pmp_deny) + " pmp_store_deny=" + Dec(pmp_store_deny));

  }
  // ---- 6. the scene's coverage, measured ----
  if (kCompareMemory) {
  if (scene_run) {
    // The scene's scratch is written by exactly one thing: the same-pc loop. A
    // wrong-path store that leaked would either be unattributed (and already
    // reported by the drain rule) or would show up here as a byte in the scratch
    // whose producer is not the loop's store. The aligned wrong-path store's
    // window is scratch+0..7, which nothing on the correct path writes, so that
    // window must receive no visible byte at all.
    uint64_t leaked = 0;
    uint64_t foreign_producer = 0;
    for (const VisibleByte& v : harness.visible()) {
      if (v.addr < kSceneScratch || v.addr >= kSceneScratch + kSceneScratchSpan) continue;
      if (harness.committed()[v.store].pc != scene.loop_store) foreign_producer++;
      if (v.addr < kSceneScratch + 8) leaked++;
    }
    harness.coverage().wrong_path_bytes_visible += leaked + foreign_producer;
    harness.Check("only the loop's own stores wrote the scene's scratch: no byte has a "
                  "wrong-path producer",
                  foreign_producer == 0,
                  Dec(foreign_producer) + " visible byte(s) in the scratch have a "
                                          "producer that is not the loop's store");
    harness.Check("the wrong-path store leaves no byte: its aligned window is untouched",
                  leaked == 0,
                  "the wrong-path store's bytes are visible at " + Dec(leaked) +
                      " address(es) in the window it targeted");
    for (const DataMem::Txn& t : harness.txns()) {
      if (t.req.addr == scene.wrong_path_aligned ||
          t.req.addr == scene.wrong_path_crossing) {
        Fail(ph, "a transaction from the wrong path reached memory: " + U64(t.req.addr));
      }
    }

    // The loop: two committed stores that share a pc and an address. The rule
    // must tell them apart by the machine's own identity and by drain order, so
    // a machine that replayed the first pass's bytes would be caught.
    std::vector<const CommittedStore*> loop;
    for (const CommittedStore& st : harness.committed()) {
      if (st.pc == scene.loop_store) loop.push_back(&st);
    }
    harness.Check("the scene's loop committed its store twice (same pc, same address)",
                  loop.size() == 2,
                  "committed " + Dec(loop.size()) + " stores at the loop's pc " +
                      U64(scene.loop_store));
    if (loop.size() == 2) {
      harness.Check("the two stores to the same address from the same pc carry "
                    "different retire_seq",
                    loop[0]->seq != loop[1]->seq,
                    "both have retire_seq=" + Dec(loop[0]->seq));
      harness.Check("the two stores to the same address from the same pc carry "
                    "different rob_id",
                    loop[0]->rob_id != loop[1]->rob_id,
                    "both have rob_id=" + U64(loop[0]->rob_id, 4));
      harness.Check("the two stores to the same address from the same pc carry "
                    "different data",
                    loop[0]->data != loop[1]->data,
                    "both carry " + U64(loop[0]->data));
      uint64_t final_byte = 0;
      if (dut_mem.Read(scene.scratch + 8, 1, &final_byte) != mosaic::AccessStatus::kOk) {
        Fail(ph, "the loop's scratch byte is not readable");
      }
      harness.Check("the loop's byte holds the second pass's payload, produced by the "
                    "second pass's store",
                    final_byte == (loop[1]->data & 0xFFu) && loop[0]->drained &&
                        loop[1]->drained,
                    "the byte holds " + U64(final_byte) + ", the first pass stored " +
                        U64(loop[0]->data & 0xFFu) + " (retire_seq=" +
                        Dec(loop[0]->seq) + ") and the second " +
                        U64(loop[1]->data & 0xFFu) + " (retire_seq=" +
                        Dec(loop[1]->seq) + ")");
    }
  }

  }
  // ---- the coverage this run contributes ----
  if (kCompareMemory) {
    Coverage& c = harness.coverage();
    for (const DataMem::Txn& t : harness.txns()) {
      if ((t.req.addr % SizeBytes(t.req.size)) != 0) c.crossing_reached_memory++;
    }
    c.forwarded_bytes += lq_fwd_bytes;
    c.retires += harness.retires();
    c.stores += harness.committed().size();
    // Stores that share both a pc and an address: the provenance rule must tell
    // them apart by identity and drain order, not by either alone.
    const std::vector<CommittedStore>& cs = harness.committed();
    for (size_t i = 0; i < cs.size(); ++i) {
      for (size_t j = i + 1; j < cs.size(); ++j) {
        if (cs[i].pc == cs[j].pc && cs[i].addr == cs[j].addr) c.same_pc_addr_pairs++;
      }
    }
  }
  totals->Add(harness.coverage());

  std::printf("  [%s] %zu retires, %llu loads, %llu stores, %llu writes, %llu reads, "
              "%llu committed stores, cycles=%llu\n",
              ph.c_str(), harness.retires(),
              static_cast<unsigned long long>(reference.loads),
              static_cast<unsigned long long>(reference.stores),
              static_cast<unsigned long long>(harness.write_txns()),
              static_cast<unsigned long long>(harness.read_txns()),
              static_cast<unsigned long long>(harness.committed().size()),
              static_cast<unsigned long long>(cycles_at_end));
  (void)outcome;
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

  bool passed = true;
  std::string detail;
  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();
    const Geometry geometry = ReadGeometry(&dut);

    const std::string repo = FindRepoRoot();
    if (geometry.reset_vector != 0x80000000ull) {
      Fail("geometry", "the reset vector is not the corpus link base: " +
                           U64(geometry.reset_vector));
    }
    if (geometry.xlen != 64) Fail("geometry", "the profile is not 64-bit");
    reporter.Check(dut.o_geom_retire_width_o >= 2,
                   "the profile retires at least two lanes, so a store and a load can "
                   "retire together");
    reporter.Check(dut.o_geom_uop_id_w_o > 0,
                   "the profile has a memory-transaction identity");

    // The scene is a *directed* coverage: assert by construction that the two
    // wrong-path words really are stores and that the second crosses the 8-byte
    // boundary the data port is organised in.
    {
      unsigned size0 = 0, size1 = 0;
      if (!IsStoreWord(EncS(0, kT1, kS3, 0x3u), &size0) ||
          !IsStoreWord(EncS(4, kT1, kS3, 0x3u), &size1)) {
        Fail("scene", "the scene's wrong-path words are not stores");
      }
      reporter.Check(size0 == 8 && size1 == 8,
                     "the scene's wrong-path stores are doubleword stores");
      reporter.Check((kSceneScratch + 4) % size1 != 0,
                     "the scene's second wrong-path store crosses the 8-byte line "
                     "boundary (its address is not a multiple of its size)");
      unsigned size2 = 0;
      if (!IsStoreWord(EncS(8, kT0, kS3, 0x3u), &size2) || size2 != 8) {
        Fail("scene", "the scene's loop store is not a doubleword store");
      }
    }

    Coverage total;
    std::vector<TraceLine> trace;
    uint64_t scene_runs = 0;

    std::printf("mem.visibility_provenance: %zu programs x %d inputs (+ scenes) from %s\n",
                sizeof(kPrograms) / sizeof(kPrograms[0]), kInputsPerProgram,
                (repo + "/tests/programs/build").c_str());
    std::printf("provenance rule: every visible byte names a committed store; a load "
                "matches an allowed source; a cancelled store leaks nothing\n");

    struct Schedule {
      int program;
      int input;
      bool scene;
    };
    std::vector<Schedule> schedule;
    // The directed scene runs first: it is the case's sharpest coverage, so a
    // control's *first* failure names the scene's wrong-path address or its
    // same-pc loop rather than a corpus store.
    schedule.push_back({0, 0, true});  // the scene over p03
    schedule.push_back({1, 0, true});  // the scene over p09
    for (size_t p = 0; p < sizeof(kPrograms) / sizeof(kPrograms[0]); p++) {
      for (int i = 0; i < kInputsPerProgram; i++) {
        schedule.push_back({static_cast<int>(p), i, false});
      }
    }

    for (const Schedule& s : schedule) {
      char elf_name[256];
      std::snprintf(elf_name, sizeof(elf_name), kPrograms[s.program].elf, s.input);
      const std::string elf_path = repo + "/" + elf_name;
      RunOnce(&dut, &reporter, options.max_cycles, elf_path,
              kPrograms[s.program].inputs[s.input], s.program, s.input, s.scene,
              geometry, &trace, &total);
      if (s.scene) scene_runs++;
    }
    total.directed_crossing_accesses = scene_runs;
    total.wrong_path_stores_directed = scene_runs * 2;

    // ---- the coverage the card names, measured across the run set ----
    if (kCompareMemory) {
    const char* size_names[4] = {"byte", "half", "word", "double"};
    for (int s = 0; s < 4; s++) {
      reporter.Check(total.load_widths[s] > 0,
                     std::string("the run set exercises a ") + size_names[s] +
                         " load (" + Dec(total.load_widths[s]) + " retiring loads)");
      reporter.Check(total.store_widths[s] > 0,
                     std::string("the run set exercises a ") + size_names[s] +
                         " store (" + Dec(total.store_widths[s]) + " visible stores)");
    }
    reporter.Check(total.sign_extended > 0,
                   "at least one load's returned value is sign-extended, not the raw "
                   "bytes (" +
                       Dec(total.sign_extended) +
                       " retirements); coverage of lb/lh/lw sign extension");
    reporter.Check(total.partial_masks > 0,
                   "at least one store's byte mask is partial, so the bytes it did not "
                   "own were left alone (" +
                       Dec(total.partial_masks) + " partial-mask drains)");
    reporter.Check(total.forwarded_bytes > 0,
                   "at least one load byte was served by an older store: same-hart "
                   "forwarding is exercised through the core (" +
                       Dec(total.forwarded_bytes) + " bytes)");
    reporter.Check(total.load_source_checks > 0,
                   "every retiring load's value was checked against its allowed sources "
                   "(" +
                       Dec(total.load_source_checks) + " loads)");
    reporter.Check(total.directed_crossing_accesses > 0,
                   "the run set contains an access crossing the 8-byte line boundary "
                   "(" +
                       Dec(total.directed_crossing_accesses) +
                       "; p0's policy traps it before memory, so it is covered as a "
                       "refusal)");
    reporter.Check(total.crossing_reached_memory == 0,
                   "no boundary-crossing access reached memory");
    reporter.Check(total.wrong_path_stores_directed > 0,
                   "the run set directs wrong-path stores at writable RAM and at the "
                   "line boundary (" + Dec(total.wrong_path_stores_directed) + ")");
    reporter.Check(total.wrong_path_bytes_visible == 0,
                   "the wrong-path store left no byte visible");
    reporter.Check(total.commit_before_drain > 0,
                   "every visible byte's producer had already committed (" +
                       Dec(total.commit_before_drain) +
                       " drains checked, largest commit-to-drain gap " +
                       Dec(total.max_commit_drain_gap) + " cycles)");
    reporter.Check(total.same_pc_addr_pairs > 0,
                   "the run set contains two committed stores that share both a pc and "
                   "an address, and their bytes were still attributed to the right one "
                   "(" +
                       Dec(total.same_pc_addr_pairs) + " such pairs)");
    reporter.Check(total.duplicate_drains == 0 && total.out_of_order_drains == 0 &&
                       total.unattributed_visibility == 0,
                   "no duplicate drain, no out-of-order drain and no visible byte "
                   "without a committed producer");
    }  // kCompareMemory

    // ---- the visibility trace, through the frozen event codec ----
    if (kCompareMemory && !options.out_dir.empty()) {
      const std::string path = options.out_dir + "/visibility.events.txt";
      std::ofstream out(path);
      if (!out) Fail("trace", "cannot write " + path);
      out << "# mem.visibility_provenance: the load/store/visibility trace with causal "
             "ids.\n";
      out << "# store-commit and mem-visible lines are config/contracts/"
             "event_v1.json records (schema v2) produced by sim/common/event_codec.h;\n";
      out << "# a store-commit RETIRE and its mem-visible MEM_VISIBLE share the store's "
             "retire_seq.\n";
      out << "# load lines carry the pc, the effective address, the size, the signedness, "
             "the value and the allowed source.\n";
      for (const TraceLine& line : trace) out << line.text << "\n";
      out.close();
      reporter.Check(static_cast<bool>(out), "the visibility trace was written");
      std::printf("  visibility trace: %s (%zu lines)\n", path.c_str(), trace.size());
    }

    std::printf("  commit/drain: %llu drains checked, %llu at the same cycle as their "
                "commit, largest gap %llu cycles, smallest %llu; %llu byte(s) were the "
                "older of two committed writers (the newer still to drain)\n",
                static_cast<unsigned long long>(total.commit_before_drain),
                static_cast<unsigned long long>(total.same_cycle_commit_drain),
                static_cast<unsigned long long>(total.max_commit_drain_gap),
                static_cast<unsigned long long>(
                    total.min_commit_drain_gap == UINT64_MAX ? 0
                                                             : total.min_commit_drain_gap),
                static_cast<unsigned long long>(total.superseded_bytes));
    std::printf("  coverage: sign-extended loads %llu, partial-mask drains %llu, "
                "forwarded bytes %llu, load-source checks %llu, same-pc+address store "
                "pairs %llu, protocol bytes %llu\n",
                static_cast<unsigned long long>(total.sign_extended),
                static_cast<unsigned long long>(total.partial_masks),
                static_cast<unsigned long long>(total.forwarded_bytes),
                static_cast<unsigned long long>(total.load_source_checks),
                static_cast<unsigned long long>(total.same_pc_addr_pairs),
                static_cast<unsigned long long>(total.protocol_bytes));
    std::printf("  note: I-036 (speculative load replay / late alias) is not enabled, so "
                "replay/rollback evidence is not claimed; see "
                "results/reports/V-018-visibility.md\n");
    std::printf("  bytes written by more than one producer: %llu (legal when the newer "
                "store is the most recent committed one)\n",
                static_cast<unsigned long long>(total.bytes_written_twice));
    std::printf("  the codec's first live use: %llu records encoded to %d octets and "
                "decoded back field for field, %llu round trips\n",
                static_cast<unsigned long long>(g_codec_records),
                static_cast<int>(mosaic::kEventRecordBytes),
                static_cast<unsigned long long>(g_codec_round_trips));
    if (g_codec_records > 0) {
      reporter.Check(g_codec_round_trips == g_codec_records,
                     "every record the trace carries survived an encode/decode round "
                     "trip through the frozen codec");
    }

    detail = "checks=" + Dec(reporter.checks()) + " retires=" + Dec(total.retires) +
             " loads=" + Dec(total.loads) + " stores=" + Dec(total.stores) +
             " drains=" + Dec(total.commit_before_drain) + " seed=" + Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the visibility contract", "violated");
    passed = false;
    detail = "visibility contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
