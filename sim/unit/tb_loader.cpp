// ============================================================================
// tb_loader.cpp -- CASE=loader.elf_boundaries, work package V-011.
//
// What this case is for
// ---------------------
// The loader is the one piece of the harness that decides whether a run happens
// at all. If it accepts a malformed image, every later check is a check of the
// wrong program; if it accepts a well-formed image and puts the bytes somewhere
// else, every later check compares the DUT against a memory image nobody asked
// for. So the card's acceptance criteria are read literally here:
//
//   * a normal image -- several PT_LOAD segments, a BSS tail, a non-zero entry
//     in a segment that is not the first -- lands byte for byte at the addresses
//     the ELF declares, with the memsz-filesz tail written as zero and nothing
//     spilled into the bytes around it;
//   * a damaged image (bad magic, big-endian, truncated, no PT_LOAD, misaligned
//     vaddr, filesz > memsz, overlapping segments, a segment whose vaddr+memsz
//     wraps the address space, an image whose segment lies outside the frozen
//     p0 map, a dynamic (ET_DYN) image, an entry that is not in any segment, an
//     entry in a segment that declares itself non-executable) is refused with a
//     named reason, and the refusal happens *before any DUT cycle runs*: the
//     whole process exits 2 with dut_cycles=0, rather than loading what it can
//     and failing later;
//   * loads and stores have a defined byte order (little-endian) and a defined
//     boundary behaviour: an access that leaves its region faults, an unmapped
//     address faults, and the address one past the end of RAM faults rather than
//     wrapping around to the start of RAM.
//
// How the refusal is observed, not asserted
// -----------------------------------------
// "Exit 2 before any DUT cycle" is a property of the process, so it is observed
// as one: for each damaged image the case re-executes *itself* with `--image
// <path>` and reads the child's exit status and the child's own line. A child
// that refuses prints `REFUSE ... dut_cycles=0` and exits 2; the child that
// accepts the good image prints `ACCEPT ... dut_cycles=N` and exits 0. Both
// halves come from the same binary and the same code path, so `dut_cycles=0`
// means the loader returned before the clock was ever driven -- not that a
// counter was left unwired. The damaged inputs are the negative controls; the
// mutants in tools/run_loader_controls.py are the other half (a mutant must make
// this case exit 1).
//
// What this case does *not* do
// ---------------------------
// It does not read the corpus' golden signatures (another package owns them), it
// does not re-derive the RTL's memory behaviour (sim/tb/mosaic_bringup_tb.sv is
// the DUT), and it does not assert that the loader agrees with any external
// assembler: the expected image is the description this file hands to its own
// builder, and tools/check_elf_load.py re-parses the emitted ELF independently.
// ============================================================================

#include <verilated.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Vmosaic_bringup_tb.h"
#include "elf_loader.h"
#include "memory_model.h"
#include "mosaic_platform.h"
#include "sim_common.h"

namespace {

using mosaic::Hex;

// The any-of-these failure path. The first one thrown wins, so the report names
// the earliest check that disagreed rather than a later consequence of it.
struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& what) { throw Failure{what}; }

constexpr int kResetCycles = 5;
constexpr uint64_t kDutCycles = 40;

constexpr uint32_t kPfR = 4;
constexpr uint32_t kPfW = 2;
constexpr uint32_t kPfX = 1;

// ---------------------------------------------------------------------------
// The image builder.
//
// This is the case's independent description of what the ELF should contain.
// The checks below compare both the loader's view (`Image`) and the memory
// model's contents against *this* structure, never against the loader's own
// output -- otherwise "the loader loaded what it said it loaded" would be the
// only property under test.
// ---------------------------------------------------------------------------
struct SegSpec {
  uint64_t vaddr = 0;
  uint64_t memsz = 0;
  bool memsz_set = false;
  uint32_t flags = kPfR | kPfW | kPfX;
  std::vector<uint8_t> data;
};

struct ElfSpec {
  uint16_t type = 2;       // ET_EXEC
  uint8_t elf_class = 2;   // ELFCLASS64
  uint8_t encoding = 1;    // ELFDATA2LSB
  uint16_t machine = 243;  // EM_RISCV
  uint64_t entry = 0;
  std::vector<SegSpec> segments;
  uint16_t phnum_override = 0;  // 0 means "one entry per segment"
  bool loadable = true;         // false rewrites every p_type to PT_NOTE
};

struct BuiltElf {
  std::vector<uint8_t> bytes;
  std::vector<SegSpec> segments;  // memsz resolved
  uint64_t entry = 0;
};

void PutU16(std::vector<uint8_t>* out, size_t offset, uint16_t value) {
  for (int i = 0; i < 2; ++i) {
    (*out)[offset + static_cast<size_t>(i)] =
        static_cast<uint8_t>((value >> (8 * i)) & 0xff);
  }
}
void PutU32(std::vector<uint8_t>* out, size_t offset, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    (*out)[offset + static_cast<size_t>(i)] =
        static_cast<uint8_t>((value >> (8 * i)) & 0xff);
  }
}
void PutU64(std::vector<uint8_t>* out, size_t offset, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    (*out)[offset + static_cast<size_t>(i)] =
        static_cast<uint8_t>((value >> (8 * i)) & 0xff);
  }
}

// A byte pattern that differs in every position and is never zero, so a byte
// dropped, duplicated or transposed changes what a read returns.
uint8_t Pattern(uint32_t tag, uint64_t index) {
  return static_cast<uint8_t>((0x35u * (index + 1u) + 0x11u * tag) ^ (index >> 3));
}

BuiltElf BuildElf(const ElfSpec& spec) {
  const size_t kEhdrSize = 64;
  const size_t kPhdrSize = 56;
  const size_t count = spec.segments.size();

  BuiltElf built;
  built.entry = spec.entry;
  built.segments = spec.segments;
  for (SegSpec& segment : built.segments) {
    // No clamp upwards: a segment whose p_filesz exceeds its p_memsz is one of
    // the images this case must be able to construct, so the builder writes the
    // numbers it was given.
    if (!segment.memsz_set) segment.memsz = segment.data.size();
  }

  size_t cursor = kEhdrSize + count * kPhdrSize;
  cursor = (cursor + 7u) & ~static_cast<size_t>(7u);
  std::vector<uint64_t> offsets(count, 0);
  for (size_t i = 0; i < count; ++i) {
    offsets[i] = cursor;
    cursor += built.segments[i].data.size();
    cursor = (cursor + 7u) & ~static_cast<size_t>(7u);
  }

  std::vector<uint8_t>& elf = built.bytes;
  elf.assign(cursor, 0);

  elf[0] = 0x7f;
  elf[1] = 'E';
  elf[2] = 'L';
  elf[3] = 'F';
  elf[4] = spec.elf_class;
  elf[5] = spec.encoding;
  elf[6] = 1;  // EV_CURRENT
  PutU16(&elf, 16, spec.type);
  PutU16(&elf, 18, spec.machine);
  PutU32(&elf, 20, 1);  // EV_CURRENT
  PutU64(&elf, 24, spec.entry);
  PutU64(&elf, 32, kEhdrSize);  // e_phoff
  PutU64(&elf, 40, 0);          // e_shoff: no section headers
  PutU32(&elf, 48, 0);          // e_flags
  PutU16(&elf, 52, kEhdrSize);
  PutU16(&elf, 54, kPhdrSize);
  PutU16(&elf, 56, spec.phnum_override != 0 ? spec.phnum_override
                                            : static_cast<uint16_t>(count));
  PutU16(&elf, 58, 0);
  PutU16(&elf, 60, 0);
  PutU16(&elf, 62, 0);

  for (size_t i = 0; i < count; ++i) {
    const SegSpec& segment = built.segments[i];
    const size_t base = kEhdrSize + i * kPhdrSize;
    PutU32(&elf, base + 0, spec.loadable ? 1u : 4u);  // PT_LOAD : PT_NOTE
    PutU32(&elf, base + 4, segment.flags);
    PutU64(&elf, base + 8, offsets[i]);
    PutU64(&elf, base + 16, segment.vaddr);
    PutU64(&elf, base + 24, segment.vaddr);
    PutU64(&elf, base + 32, segment.data.size());
    PutU64(&elf, base + 40, segment.memsz);
    PutU64(&elf, base + 48, 0x1000);  // p_align
    std::memcpy(&elf[offsets[i]], segment.data.data(), segment.data.size());
  }
  return built;
}

bool WriteFile(const std::string& path, const std::vector<uint8_t>& bytes) {
  FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) return false;
  if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
  return true;
}

// ---------------------------------------------------------------------------
// The DUT plumbing. Deliberately the same three operations tb_bringup.cpp uses
// (word-granular image load, one-cycle readback, explicit clock) so a
// disagreement here is about the loader, not about a second clocking style.
// ---------------------------------------------------------------------------
bool InMappedRange(uint64_t address) {
  struct Region { uint64_t base, size; } regions[] = {
      {MOSAIC_BOOT_ROM_BASE, MOSAIC_BOOT_ROM_SIZE},
      {MOSAIC_UART_BASE, MOSAIC_UART_SIZE},
      {MOSAIC_TEST_HARNESS_BASE, MOSAIC_TEST_HARNESS_SIZE},
      {MOSAIC_CLINT_BASE, MOSAIC_CLINT_SIZE},
      {MOSAIC_RAM_BASE, MOSAIC_RAM_SIZE},
  };
  for (const Region& region : regions) {
    if (address >= region.base && address < region.base + region.size) return true;
  }
  return false;
}

class Dut {
 public:
  explicit Dut(mosaic::ClockDriver* clock) : clock_(clock) {
    dut_.h_clk = 0;
    dut_.h_rst = 1;
    dut_.h_img_we = 0;
    dut_.h_img_addr = 0;
    dut_.h_img_data = 0;
    dut_.h_clear_mem = 0;
    dut_.h_rb_req = 0;
    dut_.h_rb_addr = 0;
    dut_.h_dbg_csr_addr = 0;
    dut_.eval();
  }

  void Tick() {
    dut_.h_clk = 0;
    dut_.eval();
    dut_.h_clk = 1;
    dut_.eval();
    clock_->Tick();
    dut_.h_clk = 0;
    dut_.eval();
  }

  void ClearMemory() {
    dut_.h_clear_mem = 1;
    Tick();
    dut_.h_clear_mem = 0;
  }

  void WriteWord(uint64_t address, uint64_t value) {
    dut_.h_img_we = 1;
    dut_.h_img_addr = address;
    dut_.h_img_data = value;
    Tick();
    dut_.h_img_we = 0;
    dut_.h_img_addr = 0;
    dut_.h_img_data = 0;
  }

  // Only the bytes the ELF actually carries are pushed; the BSS tail is expected
  // to be what the clear pass left, which is the point of checking it.
  void LoadSegment(uint64_t vaddr, const uint8_t* data, size_t size) {
    std::map<uint64_t, uint64_t> words;
    for (size_t i = 0; i < size; ++i) {
      const uint64_t address = vaddr + i;
      words[address & ~UINT64_C(7)] |=
          static_cast<uint64_t>(data[i]) << (8 * (address & 7));
    }
    for (const auto& word : words) WriteWord(word.first, word.second);
  }

  struct RunResult {
    uint64_t cycles = 0;
    bool saw_retire = false;
    uint64_t first_pc = 0;
    uint32_t first_insn = 0;
    bool pc_escaped = false;
  };

  RunResult Run(uint64_t max_cycles) {
    dut_.h_rst = 1;
    for (int i = 0; i < kResetCycles; ++i) Tick();
    dut_.h_rst = 0;
    dut_.eval();

    RunResult result;
    while (result.cycles < max_cycles) {
      Tick();
      ++result.cycles;
      if (dut_.c_evt_valid && !result.saw_retire) {
        result.saw_retire = true;
        result.first_pc = dut_.c_evt_pc;
        result.first_insn = dut_.c_evt_insn;
      }
      if (dut_.h_tohost_written) break;
      if (!InMappedRange(dut_.c_dbg_pc)) {
        result.pc_escaped = true;
        break;
      }
    }
    return result;
  }

  struct Readback {
    uint64_t value = 0;
    bool fault = false;
  };

  // One cycle of latency: request in cycle N, answer in cycle N+1.
  Readback ReadWord(uint64_t address) {
    dut_.h_rb_req = 1;
    dut_.h_rb_addr = address;
    dut_.eval();
    dut_.h_clk = 1;
    dut_.eval();
    clock_->Tick();
    dut_.h_clk = 0;
    dut_.h_rb_req = 0;
    dut_.h_rb_addr = 0;
    dut_.eval();
    Readback readback;
    readback.value = dut_.h_rb_data;
    readback.fault = dut_.h_rb_fault != 0;
    return readback;
  }

  uint64_t pc() const { return dut_.c_dbg_pc; }

 private:
  Vmosaic_bringup_tb dut_;
  mosaic::ClockDriver* clock_;
};

// Little-endian assembly of `size` bytes, used to state what a word read of an
// image segment must return without asking the loader what it thinks.
uint64_t AssembleLittleEndian(const std::vector<uint8_t>& bytes, size_t offset,
                              size_t size) {
  uint64_t value = 0;
  for (size_t i = 0; i < size; ++i) {
    value |= static_cast<uint64_t>(bytes[offset + i]) << (8 * i);
  }
  return value;
}

// ---------------------------------------------------------------------------
// A phase-scoped check. The name is what a mutant table greps for, so it is
// spelled out at every call site rather than derived from a loop variable.
// ---------------------------------------------------------------------------
void Expect(mosaic::Reporter* reporter, bool ok, const std::string& name,
            const std::string& expected, const std::string& actual) {
  reporter->Check(ok, name);
  if (!ok) {
    reporter->Mismatch(name, expected, actual);
    Fail(name + ": expected " + expected + ", got " + actual);
  }
}

std::string StatusText(mosaic::LoadStatus status, const std::string& detail) {
  return std::string(mosaic::LoadStatusName(status)) +
         (detail.empty() ? "" : " (" + detail + ")");
}

// Loads a written image and asserts the named status.
mosaic::LoadStatus LoadExpect(mosaic::Reporter* reporter, const std::string& path,
                              mosaic::LoadStatus expected, const std::string& name,
                              mosaic::Image* image) {
  std::string detail;
  const mosaic::LoadStatus status = mosaic::LoadElf(path, image, &detail);
  Expect(reporter, status == expected, name, mosaic::LoadStatusName(expected),
         StatusText(status, detail));
  return status;
}

// ---------------------------------------------------------------------------
// The positive image: two PT_LOAD segments, a BSS tail on each, a non-zero entry
// that lives in the second segment (so a loader that entered at the lowest
// address, or that kept only the first segment, is caught), and a first word
// that is an infinite self-loop so the bring-up core can be run against it
// without needing a whole program.
// ---------------------------------------------------------------------------
constexpr uint64_t kSegAVaddr = MOSAIC_RAM_BASE;          // 0x8000_0000
constexpr uint64_t kSegAFilesz = 8;
constexpr uint64_t kSegAMemsz = 8 + 0x40;                 // 64 bytes of BSS
constexpr uint64_t kSegBVaddr = MOSAIC_RAM_BASE + 0x800;  // 0x8000_0800
constexpr uint64_t kSegBFilesz = 32;
constexpr uint64_t kSegBMemsz = 32 + 16;                  // 16 bytes of BSS

constexpr uint32_t kSelfLoop = 0x0000006fu;  // jal x0, 0

ElfSpec PositiveSpec() {
  ElfSpec spec;
  spec.entry = kSegBVaddr;  // non-zero, inside the second segment

  SegSpec a;
  a.vaddr = kSegAVaddr;
  a.memsz = kSegAMemsz;
  a.memsz_set = true;
  a.flags = kPfR | kPfW | kPfX;
  a.data.resize(kSegAFilesz);
  for (size_t i = 0; i < a.data.size(); ++i) a.data[i] = Pattern(1, i);
  std::memcpy(a.data.data(), &kSelfLoop, 4);  // little-endian on the host
  spec.segments.push_back(a);

  SegSpec b;
  b.vaddr = kSegBVaddr;
  b.memsz = kSegBMemsz;
  b.memsz_set = true;
  b.flags = kPfR | kPfW | kPfX;
  b.data.resize(kSegBFilesz);
  for (size_t i = 0; i < b.data.size(); ++i) b.data[i] = Pattern(2, i);
  spec.segments.push_back(b);
  return spec;
}

// The DUT's memory must hold exactly what the ELF declared: the payload bytes at
// their addresses, zero across each BSS tail, and zero in the gap between the
// segments. Read through the DUT's own readback port, so this is evidence about
// what the image loader actually delivered, not about a shadow array in C++.
void CheckDutImage(mosaic::Reporter* reporter, Dut* dut, const BuiltElf& expected,
                   uint64_t* cycles) {
  for (size_t s = 0; s < expected.segments.size(); ++s) {
    const SegSpec& segment = expected.segments[s];
    for (uint64_t base = 0; base < segment.memsz; base += 8) {
      const Dut::Readback got = dut->ReadWord(segment.vaddr + base);
      uint64_t want = 0;
      for (uint64_t i = 0; i < 8 && base + i < segment.memsz; ++i) {
        const uint64_t offset = base + i;
        const uint8_t byte = offset < segment.data.size() ? segment.data[offset] : 0;
        want |= static_cast<uint64_t>(byte) << (8 * i);
      }
      Expect(reporter, !got.fault && got.value == want,
             "dut.image.segment" + std::to_string(s) + ".word" +
                 std::to_string(base / 8),
             Hex(want), got.fault ? "fault" : Hex(got.value));
      ++*cycles;
    }
  }
  // The bytes between the two segments are not part of any segment and must be
  // the cleared zero, not a copy of a neighbour.
  for (uint64_t address = kSegAVaddr + kSegAMemsz; address < kSegBVaddr; address += 8) {
    const Dut::Readback got = dut->ReadWord(address);
    Expect(reporter, !got.fault && got.value == 0,
           "dut.image.gap." + Hex(address), "0x0000000000000000",
           got.fault ? "fault" : Hex(got.value));
    ++*cycles;
  }
}

// An address outside the frozen map, or past the end of a region, must fault --
// it must never silently alias to a mapped address. The sentinel at the start of
// RAM makes a wrap visible: a model that masked the index would return it.
void CheckBoundaryFaults(mosaic::Reporter* reporter, Dut* dut, uint64_t* cycles) {
  dut->WriteWord(MOSAIC_RAM_BASE, UINT64_C(0xfeedfacecafebeef));
  const uint64_t last_word = MOSAIC_RAM_BASE + MOSAIC_RAM_SIZE - 8;
  dut->WriteWord(last_word, UINT64_C(0x0123456789abcdef));

  struct Probe { uint64_t address; bool expect_fault; const char* label; };
  const Probe probes[] = {
      {MOSAIC_RAM_BASE, false, "ram.first"},
      {last_word, false, "ram.last"},
      {MOSAIC_RAM_BASE + MOSAIC_RAM_SIZE, true, "ram.one-past-end"},
      {MOSAIC_RAM_BASE + MOSAIC_RAM_SIZE - 4, true, "ram.straddle-end"},
      {MOSAIC_RAM_BASE - 8, true, "ram.before-start"},
      {MOSAIC_UART_BASE + MOSAIC_UART_SIZE - 4, true, "uart.straddle-end"},
      {MOSAIC_BOOT_ROM_BASE + MOSAIC_BOOT_ROM_SIZE, true, "rom.one-past-end"},
      {UINT64_C(0x40000000), true, "unmapped.mid"},
      {UINT64_C(0xfffffffffffffff8), true, "unmapped.top"},
  };
  for (const Probe& probe : probes) {
    const Dut::Readback got = dut->ReadWord(probe.address);
    Expect(reporter, got.fault == probe.expect_fault,
           std::string("dut.boundary.") + probe.label,
           probe.expect_fault ? "fault" : "value", got.fault ? "fault" : Hex(got.value));
    ++*cycles;
  }
  // The sentinel is still where it was: no probe moved it, and none of them
  // returned it from a wrapped address.
  const Dut::Readback sentinel = dut->ReadWord(MOSAIC_RAM_BASE);
  Expect(reporter, !sentinel.fault && sentinel.value == UINT64_C(0xfeedfacecafebeef),
         "dut.boundary.no-wrap", Hex(UINT64_C(0xfeedfacecafebeef)),
         sentinel.fault ? "fault" : Hex(sentinel.value));
  ++*cycles;
}

// ---------------------------------------------------------------------------
// The loader's own per-case checks, all in-process: the status for each damaged
// image and the structure of the accepted one.
// ---------------------------------------------------------------------------
struct WrittenImage {
  std::string path;
  BuiltElf built;
};

void RunLoaderChecks(mosaic::Reporter* reporter, const std::string& dir,
                     BuiltElf* good) {
  *good = BuildElf(PositiveSpec());
  const std::string good_path = dir + "/good.elf";
  if (!WriteFile(good_path, good->bytes)) Fail("cannot write " + good_path);
  good->bytes.clear();  // keep the file as the only copy of the raw bytes
  {
    // Re-derive the raw bytes from disk so the DUT and memory checks below use
    // exactly what the loader will read, not the builder's in-memory copy.
    std::ifstream in(good_path, std::ios::binary);
    if (!in) Fail("cannot read back " + good_path);
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    good->bytes.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(good->bytes.data()), size);
  }

  // ---- the normal path ----------------------------------------------------
  mosaic::Image image;
  std::string detail;
  const mosaic::LoadStatus status = mosaic::LoadElf(good_path, &image, &detail);
  Expect(reporter, status == mosaic::LoadStatus::kOk, "loader.accept.multi-segment",
         "ok", StatusText(status, detail));
  Expect(reporter, image.entry == kSegBVaddr, "loader.accept.entry-is-e_entry",
         Hex(kSegBVaddr), Hex(image.entry));
  Expect(reporter, image.segments.size() == good->segments.size(),
         "loader.accept.segment-count", std::to_string(good->segments.size()),
         std::to_string(image.segments.size()));
  for (size_t i = 0; i < image.segments.size() && i < good->segments.size(); ++i) {
    const mosaic::Segment& got = image.segments[i];
    const SegSpec& want = good->segments[i];
    const std::string tag = "loader.accept.segment" + std::to_string(i);
    Expect(reporter, got.vaddr == want.vaddr, tag + ".vaddr", Hex(want.vaddr),
           Hex(got.vaddr));
    Expect(reporter, got.filesz == want.data.size(), tag + ".filesz",
           std::to_string(want.data.size()), std::to_string(got.filesz));
    Expect(reporter, got.memsz == want.memsz, tag + ".memsz", std::to_string(want.memsz),
           std::to_string(got.memsz));
    Expect(reporter, got.flags == want.flags, tag + ".flags", Hex(want.flags, 8),
           Hex(got.flags, 8));
    Expect(reporter, got.data == want.data, tag + ".bytes", "the builder's bytes",
           "different bytes");
  }

  // ---- the loaded memory, byte for byte -----------------------------------
  {
    // A manifest of the loader's own view, for tools/check_elf_load.py to
    // compare against an independent parse of the same file. The C++ checks
    // above compare the loader against *this* file's builder; the checker
    // compares it against a second, independent ELF reader. Neither one alone
    // proves the loader read the file correctly.
    std::ofstream manifest(dir + "/image.txt");
    if (!manifest) Fail("cannot write " + dir + "/image.txt");
    // vaddr is hexadecimal, sizes and flags are decimal, the payload is hex
    // bytes. Spelled out on the first line so the checker does not have to guess
    // (it guessed once, in the wrong direction, and the mismatch it reported was
    // the manifest's fault, not the loader's).
    manifest << "# entry and vaddr are hex; filesz, memsz and flags are decimal; "
                "payload is hex\n";
    manifest << "entry " << std::hex << image.entry << std::dec << "\n";
    for (const mosaic::Segment& segment : image.segments) {
      manifest << "seg " << std::hex << segment.vaddr << std::dec << " "
               << segment.filesz << " " << segment.memsz << " " << segment.flags << " ";
      for (const uint8_t byte : segment.data) {
        char text[3];
        std::snprintf(text, sizeof(text), "%02x", byte);
        manifest << text;
      }
      manifest << "\n";
    }
  }

  mosaic::MemoryModel memory;
  if (!memory.LoadImage(image, &detail)) Fail("LoadImage refused the good image: " + detail);
  for (size_t s = 0; s < good->segments.size(); ++s) {
    const SegSpec& segment = good->segments[s];
    for (uint64_t offset = 0; offset < segment.memsz; ++offset) {
      uint64_t value = 0;
      const mosaic::AccessStatus access =
          memory.Read(segment.vaddr + offset, 1, &value);
      const uint8_t want = offset < segment.data.size() ? segment.data[offset] : 0;
      Expect(reporter, access == mosaic::AccessStatus::kOk && value == want,
             "memory.image.segment" + std::to_string(s) + ".byte" +
                 std::to_string(offset),
             Hex(want, 2),
             std::string(mosaic::AccessStatusName(access)) + " " + Hex(value, 2));
    }
    // The word view: the same bytes read back as a little-endian word. This is
    // the byte-order check for a load, stated against the builder's bytes.
    if (segment.data.size() >= 8) {
      uint64_t word = 0;
      Expect(reporter,
             memory.Read(segment.vaddr, 8, &word) == mosaic::AccessStatus::kOk &&
                 word == AssembleLittleEndian(segment.data, 0, 8),
             "memory.image.segment" + std::to_string(s) + ".word-byte-order",
             Hex(AssembleLittleEndian(segment.data, 0, 8)), Hex(word));
    }
  }

  // ---- byte order and boundary faults, through the memory model ------------
  {
    // Deliberately not RAM_BASE + 0x1000: that is TOHOST, which the model
    // answers with a fixed zero (it means "still running") and which would turn
    // the round-trip check into a check of nothing.
    const uint64_t base = MOSAIC_RAM_BASE + 0x2000;
    const uint64_t pattern = UINT64_C(0x0123456789abcdef);
    uint64_t word = 0;
    Expect(reporter,
           memory.Write(base, 8, pattern) == mosaic::AccessStatus::kOk &&
               memory.Read(base, 8, &word) == mosaic::AccessStatus::kOk &&
               word == pattern,
           "memory.store-load.size8.round-trip", Hex(pattern), Hex(word));

    const uint8_t want_bytes[8] = {0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01};
    for (unsigned i = 0; i < 8; ++i) {
      uint64_t byte = 0;
      Expect(reporter,
             memory.Read(base + i, 1, &byte) == mosaic::AccessStatus::kOk &&
                 byte == want_bytes[i],
             "memory.byte-order.byte" + std::to_string(i), Hex(want_bytes[i], 2),
             Hex(byte, 2));
    }
    // A byte store must update exactly one byte of the containing word.
    uint64_t after = 0;
    Expect(reporter,
           memory.Write(base + 1, 1, 0x77) == mosaic::AccessStatus::kOk &&
               memory.Read(base, 8, &after) == mosaic::AccessStatus::kOk &&
               after == UINT64_C(0x0123456789ab77ef),
           "memory.byte-store.one-byte-only", Hex(UINT64_C(0x0123456789ab77ef)),
           Hex(after));

    // Alignment is checked before the region, by the model's fixed order, so an
    // unaligned access to a boundary address is reported as unaligned -- there
    // is no aligned access that can straddle a power-of-two region's end, and
    // that is exactly why the model's own straddle rule only ever fires for a
    // request that is aligned *and* crosses a region no power-of-two size could
    // describe. Both are asserted here so the precedence is pinned, not assumed.
    struct Fault { uint64_t address; unsigned size; mosaic::AccessStatus want;
                   const char* label; };
    const Fault faults[] = {
        {MOSAIC_RAM_BASE + MOSAIC_RAM_SIZE, 8, mosaic::AccessStatus::kAccessFault,
         "ram.one-past-end"},
        {MOSAIC_RAM_BASE - 8, 8, mosaic::AccessStatus::kAccessFault, "ram.before-start"},
        {MOSAIC_RAM_BASE + MOSAIC_RAM_SIZE - 4, 8, mosaic::AccessStatus::kUnaligned,
         "ram.unaligned-at-corner"},
        {MOSAIC_UART_BASE + MOSAIC_UART_SIZE, 8, mosaic::AccessStatus::kAccessFault,
         "uart.one-past-end"},
        {MOSAIC_TEST_HARNESS_BASE + MOSAIC_TEST_HARNESS_SIZE, 8,
         mosaic::AccessStatus::kAccessFault, "harness.one-past-end"},
        {UINT64_C(0x40000000), 8, mosaic::AccessStatus::kAccessFault, "unmapped.mid"},
        {UINT64_C(0xfffffffffffffff8), 8, mosaic::AccessStatus::kAccessFault,
         "unmapped.top"},
        {MOSAIC_BOOT_ROM_BASE, 8, mosaic::AccessStatus::kReadOnly, "rom.write"},
        {MOSAIC_RAM_BASE + 2, 4, mosaic::AccessStatus::kUnaligned, "ram.misaligned"},
        {MOSAIC_RAM_BASE, 3, mosaic::AccessStatus::kUnaligned, "size-3"},
    };
    const uint64_t faults_before = memory.access_fault_count();
    uint64_t first_before = 0;
    Expect(reporter, memory.Read(MOSAIC_RAM_BASE, 8, &first_before) == mosaic::AccessStatus::kOk,
           "memory.boundary.first-word-readable", "ok", "fault");
    uint64_t expected_fault_increments = 0;
    for (const Fault& fault : faults) {
      uint64_t scratch = 0;
      mosaic::AccessStatus got;
      if (fault.want == mosaic::AccessStatus::kReadOnly) {
        got = memory.Write(fault.address, fault.size, 0);
      } else {
        got = memory.Read(fault.address, fault.size, &scratch);
      }
      Expect(reporter, got == fault.want,
             std::string("memory.boundary.") + fault.label,
             mosaic::AccessStatusName(fault.want), mosaic::AccessStatusName(got));
      if (fault.want == mosaic::AccessStatus::kAccessFault ||
          fault.want == mosaic::AccessStatus::kReadOnly) {
        ++expected_fault_increments;
      }
    }
    // The counter is the model's own claim that these were faults rather than
    // silent reads, so a model that "handled" one without noticing is caught.
    Expect(reporter,
           memory.access_fault_count() == faults_before + expected_fault_increments,
           "memory.boundary.fault-counter",
           std::to_string(faults_before + expected_fault_increments),
           std::to_string(memory.access_fault_count()));

    // The first word of RAM is unchanged: a probe that left its region neither
    // wrapped back onto the start of RAM nor returned it by mistake.
    uint64_t first_after = 0;
    Expect(reporter,
           memory.Read(MOSAIC_RAM_BASE, 8, &first_after) == mosaic::AccessStatus::kOk &&
               first_after == first_before,
           "memory.boundary.no-wrap", Hex(first_before), Hex(first_after));
  }
}

// A damaged image, the status the loader must give it, and -- when the damage is
// only visible against the frozen map -- the harness-level refusal that follows.
struct BadImage {
  std::string name;
  std::string path;
  mosaic::LoadStatus status;  // kOk when the refusal happens at LoadImage
  std::string reason;         // token the child must print
};

void RunBadImageChecks(mosaic::Reporter* reporter, const std::string& dir,
                       std::vector<BadImage>* out) {
  // Each entry writes its file, then asserts the status in-process.
  auto emit = [&](const std::string& name, BuiltElf built,
                  mosaic::LoadStatus expected, const std::string& reason) {
    const std::string path = dir + "/bad-" + name + ".elf";
    if (!WriteFile(path, built.bytes)) Fail("cannot write " + path);
    mosaic::Image image;
    LoadExpect(reporter, path, expected, "loader.reject." + name, &image);
    out->push_back(BadImage{name, path, expected, reason});
  };

  // Bad magic.
  {
    BuiltElf built = BuildElf(PositiveSpec());
    built.bytes[0] = 'X';
    emit("magic", built, mosaic::LoadStatus::kBadMagic, "bad-magic");
  }
  // Big-endian.
  {
    BuiltElf built = BuildElf(PositiveSpec());
    built.bytes[5] = 2;  // ELFDATA2MSB
    emit("endian", built, mosaic::LoadStatus::kNotLittleEndian, "not-little-endian");
  }
  // Truncated: the program header table runs past the end of the file.
  {
    ElfSpec spec = PositiveSpec();
    spec.phnum_override = 8;
    emit("truncated", BuildElf(spec), mosaic::LoadStatus::kTruncated, "truncated");
  }
  // No PT_LOAD segment at all.
  {
    ElfSpec spec = PositiveSpec();
    spec.loadable = false;
    emit("noload", BuildElf(spec), mosaic::LoadStatus::kNoLoadableSegment,
         "no-loadable-segment");
  }
  // A PT_LOAD whose vaddr is not 8-byte aligned.
  {
    ElfSpec spec = PositiveSpec();
    spec.segments[0].vaddr = kSegAVaddr + 2;
    emit("align", BuildElf(spec), mosaic::LoadStatus::kSegmentAlignment,
         "segment-alignment");
  }
  // filesz > memsz.
  {
    ElfSpec spec = PositiveSpec();
    spec.segments[0].memsz = 4;  // smaller than the 8 payload bytes
    spec.segments[0].memsz_set = true;
    emit("filesz-gt-memsz", BuildElf(spec), mosaic::LoadStatus::kBadProgramHeader,
         "bad-program-header");
  }
  // Two overlapping PT_LOAD segments.
  {
    ElfSpec spec = PositiveSpec();
    spec.segments[1].vaddr = kSegAVaddr + 0x10;  // inside segment 0's memsz
    spec.entry = kSegAVaddr + 0x10;
    emit("overlap", BuildElf(spec), mosaic::LoadStatus::kSegmentOverlap,
         "segment-overlap");
  }
  // vaddr + memsz wraps the address space.
  {
    ElfSpec spec = PositiveSpec();
    spec.segments[0].vaddr = UINT64_C(0xfffffffffffff000);
    spec.segments[0].memsz = UINT64_C(0x2000);
    spec.segments[0].memsz_set = true;
    spec.entry = UINT64_C(0xfffffffffffff000);
    emit("vaddr-overflow", BuildElf(spec), mosaic::LoadStatus::kSegmentOverflow,
         "segment-overflow");
  }
  // p_offset + p_filesz wraps: vaddr stays small so the vaddr check does not
  // fire first, and the file-offset check has to catch it.
  {
    ElfSpec spec;
    spec.entry = 0;
    SegSpec a;
    a.vaddr = 0;
    a.memsz = UINT64_C(0xfffffffffffffff8);
    a.memsz_set = true;
    a.data.resize(8);
    for (size_t i = 0; i < a.data.size(); ++i) a.data[i] = Pattern(3, i);
    spec.segments.push_back(a);
    BuiltElf built = BuildElf(spec);
    const size_t phdr = 64;
    PutU64(&built.bytes, phdr + 8, UINT64_C(0xffffffffffffffe0));  // p_offset
    PutU64(&built.bytes, phdr + 32, UINT64_C(0xfffffffffffffff8));  // p_filesz
    emit("file-overflow", built, mosaic::LoadStatus::kSegmentOverflow,
         "segment-overflow");
  }
  // A dynamic (position-independent) image.
  {
    BuiltElf built = BuildElf(PositiveSpec());
    PutU16(&built.bytes, 16, 3);  // ET_DYN
    emit("dynamic", built, mosaic::LoadStatus::kNotExecutable, "not-executable");
  }
  // Entry outside every segment.
  {
    ElfSpec spec = PositiveSpec();
    spec.entry = MOSAIC_RAM_BASE + 0x100000;
    emit("entry-unmapped", BuildElf(spec), mosaic::LoadStatus::kEntryNotMapped,
         "entry-not-mapped");
  }
  // Entry inside a segment that declares itself non-executable.
  {
    ElfSpec spec = PositiveSpec();
    spec.entry = kSegAVaddr;
    spec.segments[0].flags = kPfR | kPfW;  // declared, but no PF_X
    emit("entry-not-executable", BuildElf(spec),
         mosaic::LoadStatus::kEntryNotExecutable, "entry-not-executable");
  }
  // Well formed, but linked against a different memory map: the loader cannot
  // know this, so the refusal is the harness's job and happens at LoadImage.
  {
    ElfSpec spec = PositiveSpec();
    const uint64_t elsewhere = UINT64_C(0x40000000);
    spec.segments[0].vaddr = elsewhere;
    spec.segments[1].vaddr = elsewhere + 0x800;
    spec.entry = elsewhere;
    BuiltElf built = BuildElf(spec);
    const std::string path = dir + "/bad-out-of-map.elf";
    if (!WriteFile(path, built.bytes)) Fail("cannot write " + path);
    mosaic::Image image;
    LoadExpect(reporter, path, mosaic::LoadStatus::kOk, "loader.accept.out-of-map-elf",
               &image);
    mosaic::MemoryModel memory;
    std::string detail;
    Expect(reporter, !memory.LoadImage(image, &detail), "memory.reject.out-of-range",
           "segments outside the " + std::string(MOSAIC_PROFILE_NAME) + " map",
           "accepted: " + detail);
    out->push_back(BadImage{"out-of-map", path, mosaic::LoadStatus::kOk,
                            "image-outside-memory-map"});
  }
}

// ---------------------------------------------------------------------------
// Running one image through the DUT, in the mode the parent forks.
// ---------------------------------------------------------------------------
struct ImageRunReport {
  bool refused = false;
  std::string reason;
  uint64_t dut_cycles = 0;
  uint64_t entry = 0;
  bool dut_ok = false;
};

// Loads, checks against the frozen map, and only then drives the clock. The
// dut_cycles counter is what distinguishes "refused before running" from
// "refused after running", so it is incremented only after a successful load.
ImageRunReport RunImage(const std::string& path) {
  ImageRunReport report;
  mosaic::Image image;
  std::string detail;
  const mosaic::LoadStatus status = mosaic::LoadElf(path, &image, &detail);
  if (status != mosaic::LoadStatus::kOk) {
    report.refused = true;
    report.reason = std::string(mosaic::LoadStatusName(status)) +
                    (detail.empty() ? "" : ": " + detail);
    return report;
  }
  mosaic::MemoryModel memory;
  if (!memory.LoadImage(image, &detail)) {
    report.refused = true;
    report.reason = "image-outside-memory-map: " + detail;
    return report;
  }
  report.entry = image.entry;

  mosaic::ClockDriver clock;
  Dut dut(&clock);
  dut.ClearMemory();
  for (const mosaic::Segment& segment : image.segments) {
    dut.LoadSegment(segment.vaddr, segment.data.data(), segment.data.size());
  }
  const Dut::RunResult run = dut.Run(kDutCycles);
  report.dut_cycles = run.cycles;
  // The accepted image's first word is an infinite self-loop at the reset
  // vector, so the core must retire it from the address the ELF declared.
  report.dut_ok = !run.pc_escaped && run.saw_retire && run.first_pc == MOSAIC_RESET_VECTOR &&
                  run.first_insn == kSelfLoop;
  return report;
}

// ---------------------------------------------------------------------------
// The observed refusals: fork/exec this same binary once per damaged image and
// read its exit status and its own line.
// ---------------------------------------------------------------------------
int SpawnSelf(const std::string& self, const std::string& case_id,
              const std::string& image, const std::string& max_cycles,
              std::string* output) {
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) Fail("pipe() failed");
  const pid_t pid = fork();
  if (pid < 0) Fail("fork() failed");
  if (pid == 0) {
    if (dup2(pipe_fds[1], STDOUT_FILENO) < 0 || dup2(pipe_fds[1], STDERR_FILENO) < 0) {
      _exit(3);
    }
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    std::string case_flag = "--case";
    std::string image_flag = "--image";
    std::string cycles_flag = "--max-cycles";
    char* child_argv[] = {const_cast<char*>(self.c_str()), const_cast<char*>(case_flag.c_str()),
                          const_cast<char*>(case_id.c_str()),
                          const_cast<char*>(image_flag.c_str()),
                          const_cast<char*>(image.c_str()),
                          const_cast<char*>(cycles_flag.c_str()),
                          const_cast<char*>(max_cycles.c_str()), nullptr};
    execv(self.c_str(), child_argv);
    std::fprintf(stderr, "execv(%s) failed: %s\n", self.c_str(), std::strerror(errno));
    _exit(3);
  }
  close(pipe_fds[1]);
  output->clear();
  char buffer[4096];
  ssize_t got = 0;
  while ((got = read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
    output->append(buffer, static_cast<size_t>(got));
  }
  close(pipe_fds[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) Fail("waitpid() failed");
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return 128 + WTERMSIG(status);
}

std::string FirstLine(const std::string& text) {
  const size_t end = text.find('\n');
  return end == std::string::npos ? text : text.substr(0, end);
}

// ---------------------------------------------------------------------------
// The child half: `--image` was given, so this process loads exactly one image,
// refuses before touching the clock if it must, and otherwise runs the DUT.
// ---------------------------------------------------------------------------
int RunImageMode(const mosaic::Options& options) {
  const ImageRunReport report = RunImage(options.image);
  if (report.refused) {
    std::printf("REFUSE %s %s dut_cycles=0\n", options.case_id.c_str(),
                report.reason.c_str());
    std::fflush(stdout);
    return mosaic::kExitUsage;  // 2
  }
  std::printf("ACCEPT %s entry=%s dut_cycles=%llu dut_ok=%d\n",
              options.case_id.c_str(), Hex(report.entry).c_str(),
              static_cast<unsigned long long>(report.dut_cycles),
              report.dut_ok ? 1 : 0);
  std::fflush(stdout);
  return report.dut_ok ? mosaic::kExitPass : mosaic::kExitFail;
}

}  // namespace

int main(int argc, char** argv) {
  mosaic::Options options;
  std::string error;
  if (!mosaic::Options::Parse(argc, argv, &options, &error)) {
    std::fprintf(stderr, "usage error: %s\n", error.c_str());
    return mosaic::kExitUsage;
  }
  Verilated::commandArgs(argc, argv);

  // Single-image mode is the mode the parent forks, and it must not fork again.
  if (!options.image.empty()) return RunImageMode(options);

  char self_buffer[4096];
  const char* self = realpath(argv[0], self_buffer);
  if (self == nullptr) {
    std::fprintf(stderr, "cannot resolve %s: %s\n", argv[0], std::strerror(errno));
    return mosaic::kExitUsage;
  }

  mosaic::Reporter reporter(options,
                            std::string("Verilator ") + Verilated::productVersion());

  std::string detail;
  uint64_t dut_cycles = 0;
  int controls = 0;
  int controls_ok = 0;

  try {
    if (mkdir(options.out_dir.c_str(), 0755) != 0 && errno != EEXIST) {
      Fail("cannot create the output directory " + options.out_dir);
    }
    const std::string dir = options.out_dir;

    // ---- 1. the loader, in process --------------------------------------
    BuiltElf good;
    RunLoaderChecks(&reporter, dir, &good);

    // ---- 2. the loaded image through the DUT ----------------------------
    {
      mosaic::Image image;
      std::string load_detail;
      const mosaic::LoadStatus status =
          mosaic::LoadElf(dir + "/good.elf", &image, &load_detail);
      Expect(&reporter, status == mosaic::LoadStatus::kOk,
             "dut.load.good-image-again", "ok", StatusText(status, load_detail));

      mosaic::ClockDriver clock;
      Dut dut(&clock);
      dut.ClearMemory();
      for (const mosaic::Segment& segment : image.segments) {
        dut.LoadSegment(segment.vaddr, segment.data.data(), segment.data.size());
      }
      const Dut::RunResult run = dut.Run(kDutCycles);
      dut_cycles += run.cycles;
      Expect(&reporter, !run.pc_escaped, "dut.run.pc-stays-mapped",
             "pc inside the frozen map (last is not " + Hex(MOSAIC_RAM_BASE +
                                                            MOSAIC_RAM_SIZE) + ")",
             Hex(dut.pc()));
      Expect(&reporter, run.saw_retire, "dut.run.retires-an-instruction",
             "at least one retire event", "none in " + std::to_string(run.cycles) +
                                              " cycles");
      Expect(&reporter, run.first_pc == MOSAIC_RESET_VECTOR,
             "dut.run.first-retire-at-reset-vector", Hex(MOSAIC_RESET_VECTOR),
             Hex(run.first_pc));
      // The image's first word is the infinite self-loop the builder placed
      // there; the DUT executing exactly those bytes is the end-to-end proof
      // that the load delivered the ELF and nothing else.
      Expect(&reporter, run.first_insn == kSelfLoop, "dut.run.executes-elf-bytes",
             Hex(kSelfLoop, 8), Hex(run.first_insn, 8));

      CheckDutImage(&reporter, &dut, good, &dut_cycles);
      CheckBoundaryFaults(&reporter, &dut, &dut_cycles);
    }

    // ---- 3. the damaged images, in process and by re-exec -----------------
    std::vector<BadImage> bad;
    RunBadImageChecks(&reporter, dir, &bad);

    std::vector<std::pair<std::string, int>> control_table;

    // The accepted image through the same child path, so "dut_cycles=0" on a
    // refusal is a decision the loader made, not a counter that is never used.
    {
      std::string output;
      const int code = SpawnSelf(self, options.case_id, dir + "/good.elf", "40", &output);
      std::printf("  control %-20s exit=%d  %s\n", "accepts-good-image", code,
                  FirstLine(output).c_str());
      ++controls;
      Expect(&reporter, code == 0, "control.accepts-good-image.exit", "0",
             std::to_string(code));
      Expect(&reporter, output.find("dut_cycles=0") == std::string::npos,
             "control.accepts-good-image.runs-dut", "dut_cycles>0", FirstLine(output));
      controls_ok += (code == 0) ? 1 : 0;
      control_table.emplace_back("accepts-good-image", code);
    }

    for (const BadImage& image : bad) {
      std::string output;
      const int code = SpawnSelf(self, options.case_id, image.path, "40", &output);
      std::printf("  control %-20s exit=%d  %s\n", image.name.c_str(), code,
                  FirstLine(output).c_str());
      ++controls;
      Expect(&reporter, code == mosaic::kExitUsage, "control." + image.name + ".exit",
             "2", std::to_string(code));
      Expect(&reporter, output.find("REFUSE") != std::string::npos,
             "control." + image.name + ".refused", "a REFUSE line", FirstLine(output));
      Expect(&reporter, output.find("dut_cycles=0") != std::string::npos,
             "control." + image.name + ".before-any-dut-cycle", "dut_cycles=0",
             FirstLine(output));
      controls_ok += (code == mosaic::kExitUsage) ? 1 : 0;
      control_table.emplace_back(image.name, code);
    }

    // The control table is written next to the report so the claim "each damaged
    // image exits 2" can be read off without re-running anything.
    std::ofstream table(dir + "/controls.txt");
    if (table) {
      for (const auto& row : control_table) {
        table << row.first << " " << row.second << "\n";
      }
    }

    detail = std::to_string(reporter.checks()) + " checks, " +
             std::to_string(dut_cycles) + " DUT cycles, " +
             std::to_string(controls_ok) + "/" + std::to_string(controls) +
             " controls observed";
    if (reporter.failures() > 0) {
      return reporter.Finish("FAIL", detail + ", " +
                                          std::to_string(reporter.failures()) +
                                          " check(s) failed");
    }
    return reporter.Finish("PASS", detail);
  } catch (const Failure& failure) {
    return reporter.Finish("FAIL", failure.what + " (" +
                                       std::to_string(reporter.checks()) +
                                       " checks, " + std::to_string(dut_cycles) +
                                       " DUT cycles)");
  }
}
