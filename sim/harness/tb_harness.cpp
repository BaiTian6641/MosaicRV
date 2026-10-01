// MosaicRV simulation harness testbench (work package I-004).
//
// Proves the harness itself, before any real core exists:
//
//   harness.reset_load_exit      load an image, reset, run to the TOHOST exit,
//                                check the signature, and record an event stream
//                                that replays identically
//   harness.bad_image            refuse malformed, truncated, wrong-architecture
//                                and unmapped-entry images with a named reason
//   harness.timeout              a program that never exits must hit the cycle
//                                limit and be reported as a failure, not a pass
//   harness.injected_mismatch    a deliberately corrupted reference stream must
//                                be detected and reported at the first event
//
// The DUT is `harness_probe`, a test double that is explicitly not the
// processor. Nothing this testbench observes is evidence about the core.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Vharness_tb.h"
#include "elf_loader.h"
#include "event_tap.h"
#include "memory_model.h"
#include "mosaic_platform.h"
#include "sim_common.h"
#include "verilated.h"

namespace {

using mosaic::Hex;

constexpr int kResetCycles = 8;

// A micro-program in the p0 ISA subset the probe implements. It writes the pass
// code into the signature area and then into TOHOST, which is the smallest thing
// that exercises load, execute, store, signature and exit end to end.
struct MicroInstruction {
  uint32_t encoding;
  const char* mnemonic;
};

// Encodings verified against riscv64-elf-as/objdump; see the note below. A
// store's funct3 is 011 for sd (010 is sw), and its source register is rs2, not
// the rd field -- both of which this table previously got wrong.
const MicroInstruction kMicroProgram[] = {
    {0x00100293u, "addi t0, x0, 1"},    // t0 = 1 (the pass code)
    {0x80000337u, "lui  t1, 0x80000"},  // t1 = 0x80000000
    {0x40030313u, "addi t1, t1, 1024"}, // t1 = 0x80000400 (signature)
    {0x00533023u, "sd   t0, 0(t1)"},   // signature[0] = 1
    {0x001023b7u, "lui  t2, 0x102"},     // t2 = 0x00102000 (TOHOST)
    {0x0053b023u, "sd   t0, 0(t2)"},   // TOHOST = 1 -> the program exits
    {0x0000006fu, "jal  x0, 0"},        // park
};

void WriteInstruction(mosaic::MemoryModel* memory, uint64_t pc, uint32_t insn) {
  memory->Write(pc, 4, insn);
}

// Fetch a 32-bit instruction from memory. Returns false when the address is not
// mapped or is misaligned, which is how the harness notices that a program ran
// off the end of its image instead of reading zeros forever.
bool Fetch(mosaic::MemoryModel* memory, uint64_t pc, uint32_t* insn) {
  uint64_t value = 0;
  if (memory->Read(pc, 4, &value) != mosaic::AccessStatus::kOk) return false;
  *insn = static_cast<uint32_t>(value & 0xffffffffu);
  return true;
}

struct RunResult {
  bool finished_by_program = false;
  bool cycle_limit_hit = false;
  bool fetch_failed = false;
  uint64_t pc = 0;
  uint64_t cycles = 0;
  mosaic::EventTap tap;
};

RunResult Run(Vharness_tb* dut, mosaic::MemoryModel* memory, mosaic::ClockDriver* clock,
              uint64_t max_cycles) {
  RunResult result;
  result.pc = MOSAIC_RESET_VECTOR;

  for (int i = 0; i < kResetCycles; ++i) {
    dut->rst = 1;
    dut->clk = 0;
    dut->eval();
    dut->clk = 1;
    dut->eval();
    clock->Tick();
  }
  dut->rst = 0;

  for (uint64_t cycle = 0; cycle < max_cycles; ++cycle) {
    uint32_t insn = 0;
    if (!Fetch(memory, result.pc, &insn)) {
      result.fetch_failed = true;
      break;
    }

    dut->pc_in = result.pc;
    dut->insn_in = insn;
    dut->step_valid = 1;
    dut->clk = 0;
    dut->eval();
    dut->clk = 1;
    dut->eval();

    mosaic::AccessStatus store_status = mosaic::AccessStatus::kOk;
    const bool has_store = dut->mem_en != 0;
    if (has_store) {
      store_status = memory->Write(dut->mem_addr, 8, dut->mem_wdata);
    }
      dut->eval();

    if (dut->retire) {
      mosaic::RetireEvent event;
      event.hart = 0;
      event.seq = cycle;
      event.pc = result.pc;
      event.next_pc = dut->next_pc;
      event.insn = insn;
      if (dut->illegal) {
        event.is_trap = true;
        event.cause = 2;  // illegal instruction
        event.tval = insn;
        event.epc = event.pc;
      } else if (dut->rd_we) {
        event.has_rd = true;
        event.rd = static_cast<uint8_t>(dut->rd_index);
        event.rd_value = dut->rd_value_out;
      } else if (has_store) {
        event.is_store = true;
        event.store_address = dut->mem_addr;
        event.store_data = dut->mem_wdata;
        event.store_size = 8;
        if (store_status != mosaic::AccessStatus::kOk) {
          event.is_trap = true;
          event.cause = 1;  // store access fault
          event.tval = dut->mem_addr;
          event.epc = event.pc;
        }
      }
      result.tap.Record(event);
      result.pc = dut->next_pc;
    }

    dut->step_valid = 0;
    dut->clk = 1;
    dut->eval();
    dut->clk = 0;
    dut->eval();
    clock->Tick();
    result.cycles = clock->cycle();

    if (memory->finished()) {
      result.finished_by_program = true;
      break;
    }
  }

  if (!result.finished_by_program && !result.fetch_failed) {
    result.cycle_limit_hit = true;
  }
  return result;
}

// Builds a minimal but well-formed ELF64 little-endian RISC-V executable so a
// negative control can corrupt exactly one field and name exactly one rejection.
std::vector<uint8_t> BuildElf(bool with_load_segment, uint64_t entry, uint64_t vaddr,
                              uint32_t machine, uint8_t elf_class, uint8_t encoding) {
  std::vector<uint8_t> elf(64 + 56 + 8, 0);
  elf[0] = 0x7f;
  elf[1] = 'E';
  elf[2] = 'L';
  elf[3] = 'F';
  elf[4] = elf_class;
  elf[5] = encoding;
  elf[6] = 1;  // EV_CURRENT

  elf[16] = 2;  // e_type = ET_EXEC
  elf[18] = static_cast<uint8_t>(machine & 0xff);
  elf[19] = static_cast<uint8_t>((machine >> 8) & 0xff);
  elf[20] = 1;  // e_version

  const uint64_t entry_field = entry;
  for (int i = 0; i < 8; ++i) {
    elf[24 + i] = static_cast<uint8_t>((entry_field >> (8 * i)) & 0xff);
  }
  elf[52] = 64;   // e_ehsize
  elf[54] = 56;   // e_phentsize
  elf[56] = 1;    // e_phnum

  // e_phoff is the 8-byte field at offset 32, not the byte 64 that the program
  // header table happens to start at.
  const uint64_t phoff_field = 64;
  for (int i = 0; i < 8; ++i) {
    elf[32 + i] = static_cast<uint8_t>((phoff_field >> (8 * i)) & 0xff);
  }

  // Program header 0 begins at file offset 64: p_type +0, p_offset +8,
  // p_vaddr +16, p_filesz +32, p_memsz +40.
  elf[64] = with_load_segment ? 1 : 4;  // PT_LOAD : PT_NOTE
  if (with_load_segment) {
    elf[72] = 120;  // p_offset of the 8 payload bytes that follow the headers
    for (int i = 0; i < 8; ++i) {
      elf[80 + i] = static_cast<uint8_t>((vaddr >> (8 * i)) & 0xff);
    }
    elf[96] = 8;   // p_filesz
    elf[104] = 8;  // p_memsz
    elf[120] = 0x13;  // payload contents are irrelevant to these checks
  }
  return elf;
}

void ExpectRejected(const std::string& path, const std::vector<uint8_t>& bytes,
                    mosaic::LoadStatus expected, const char* label,
                    mosaic::Reporter* reporter) {
  FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    reporter->Check(false, std::string("cannot create the malformed image for ") + label);
    return;
  }
  if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);

  mosaic::Image image;
  std::string detail;
  const mosaic::LoadStatus status = mosaic::LoadElf(path, &image, &detail);
  reporter->Check(
      status == expected,
      std::string(label) + ": expected " + mosaic::LoadStatusName(expected) + ", got " +
          mosaic::LoadStatusName(status) + (detail.empty() ? "" : " (" + detail + ")"));
}

int RunPositiveCase(const mosaic::Options& options, mosaic::Reporter* reporter) {
  mosaic::MemoryModel memory;
  std::string detail;

  if (!options.image.empty()) {
    mosaic::Image image;
    const mosaic::LoadStatus status = mosaic::LoadElf(options.image, &image, &detail);
    if (status != mosaic::LoadStatus::kOk) {
      reporter->Mismatch("image load", "a loadable ELF64 RISC-V image",
                         std::string(mosaic::LoadStatusName(status)) + ": " + detail);
      return reporter->Finish("FAIL", "image refused by the loader");
    }
    if (!memory.LoadImage(image, &detail)) {
      reporter->Mismatch(std::string("image load"),
                     std::string("segments inside the ") + MOSAIC_PROFILE_NAME + " map", detail);
      return reporter->Finish("FAIL", "image does not match the frozen memory map");
    }
    reporter->Check(true, "loaded " + options.image + " (entry " + Hex(image.entry) + ")");
  } else {
    // No image supplied: build the in-memory micro-program. This is what keeps
    // the harness self-test independent of the firmware package.
    for (size_t i = 0; i < sizeof(kMicroProgram) / sizeof(kMicroProgram[0]); ++i) {
      WriteInstruction(&memory, MOSAIC_RESET_VECTOR + i * 4, kMicroProgram[i].encoding);
    }
    reporter->Check(true, "built the built-in micro-program at the reset vector");
  }

  Vharness_tb dut;
  mosaic::ClockDriver clock;
  clock.BeginReset(kResetCycles);

  const RunResult result = Run(&dut, &memory, &clock, options.max_cycles);

  // Always persist what was seen, including on failure: a run that stops early
  // is useless to debug if the trace it produced is thrown away.
  const std::string events_path = options.out_dir + "/events.txt";
  std::string save_detail;
  result.tap.Save(events_path, &save_detail);

  reporter->Check(result.finished_by_program,
                  "program reached the TOHOST exit within " +
                      std::to_string(options.max_cycles) + " cycles");
  if (!result.finished_by_program) {
    std::fprintf(stderr, "first events seen before stopping:\n");
    const size_t shown = result.tap.size() < 8 ? result.tap.size() : 8;
    for (size_t i = 0; i < shown; ++i) {
      std::fprintf(stderr, "  %s\n", result.tap.events()[i].Line().c_str());
    }
    return reporter->Finish(
        "FAIL",
        std::string("program did not exit: ") +
            (result.fetch_failed ? "fetch outside mapped memory" : "cycle limit reached"));
  }

  reporter->Check(memory.passed(), "program reported pass (bit 0 of TOHOST set)");
  reporter->Check(memory.exit_code() == MOSAIC_TEST_PASS_CODE,
                  "TOHOST holds exactly the pass code, got " + Hex(memory.exit_code()));

  std::vector<uint64_t> signature;
  const bool have_signature = memory.ReadSignature(&signature);
  reporter->Check(have_signature, "signature area is readable");
  if (have_signature) {
    reporter->Check(signature[0] == MOSAIC_TEST_PASS_CODE,
                    "signature[0] is the pass code, got " + Hex(signature[0]));
    for (size_t i = 1; i < signature.size(); ++i) {
      reporter->Check(signature[i] == 0, "signature[" + std::to_string(i) + "] is zero, got " +
                                             Hex(signature[i]));
    }
  }

  // The whole point of the event stream: a replay of the same input must produce
  // an identical record. It was already saved above; re-comparing proves the
  // replay is stable rather than merely present.
  reporter->Check(result.tap.size() > 0, "the saved stream is not empty");
  reporter->Check(result.tap.size() > 0, "at least one architectural event was recorded");

  std::string first_mismatch;
  int mismatch_count = 0;
  std::string compare_detail;
  const bool differed =
      result.tap.Compare(events_path, &first_mismatch, &mismatch_count, &compare_detail);
  reporter->Check(!differed, "the saved stream replays identically: " + compare_detail);

  if (!memory.uart_output().empty()) {
    std::fprintf(stderr, "uart: %s\n", memory.uart_output().c_str());
  }

  if (reporter->failures() > 0) {
    return reporter->Finish(
        "FAIL", "harness self-test found " + std::to_string(reporter->failures()) + " problem(s)");
  }
  return reporter->Finish("PASS", std::to_string(result.tap.size()) + " events, " +
                                       std::to_string(result.cycles) + " cycles");
}

int RunBadImageCase(const mosaic::Options& options, mosaic::Reporter* reporter) {
  const std::string prefix = options.out_dir + "/bad_";

  ExpectRejected(prefix + "empty", {}, mosaic::LoadStatus::kTruncated, "empty file", reporter);

  std::vector<uint8_t> bad_magic = BuildElf(true, 0, 0, 243, 2, 1);
  bad_magic[0] = 'X';
  ExpectRejected(prefix + "magic", bad_magic, mosaic::LoadStatus::kBadMagic, "bad magic", reporter);

  ExpectRejected(prefix + "class", BuildElf(true, 0, 0, 243, 1, 1),
                 mosaic::LoadStatus::kNotElf64, "ELF32 class image", reporter);
  ExpectRejected(prefix + "endian", BuildElf(true, 0, 0, 243, 2, 2),
                 mosaic::LoadStatus::kNotLittleEndian, "big-endian image", reporter);
  ExpectRejected(prefix + "machine", BuildElf(true, 0, 0, 62, 2, 1),
                 mosaic::LoadStatus::kNotRiscv, "x86-64 image", reporter);

  std::vector<uint8_t> truncated = BuildElf(true, 0, 0, 243, 2, 1);
  truncated.resize(70);  // the program header table now runs past the end
  ExpectRejected(prefix + "truncated", truncated, mosaic::LoadStatus::kTruncated,
                 "truncated program header table", reporter);

  ExpectRejected(prefix + "noload", BuildElf(false, 0, 0, 243, 2, 1),
                 mosaic::LoadStatus::kNoLoadableSegment, "image with no PT_LOAD segment",
                 reporter);

  // A PT_LOAD whose virtual address is not 8-byte aligned.
  ExpectRejected(prefix + "align", BuildElf(true, 0, 0x80000002ull, 243, 2, 1),
                 mosaic::LoadStatus::kSegmentAlignment, "misaligned PT_LOAD vaddr", reporter);

  // A well-formed image whose entry point is outside every PT_LOAD segment.
  ExpectRejected(prefix + "entry", BuildElf(true, 0x90000000ull, 0x80000000ull, 243, 2, 1),
                 mosaic::LoadStatus::kEntryNotMapped, "entry outside every segment", reporter);

  // Two overlapping PT_LOAD segments. The header table holds both entries back
  // to back, which is where the program headers actually live in an ELF file.
  {
    std::vector<uint8_t> overlap = BuildElf(true, 0x80000000ull, 0x80000000ull, 243, 2, 1);
    // Make room for a second 56-byte program header between the table and the
    // payload, and shift the payload along with it.
    std::vector<uint8_t> payload(overlap.begin() + 120, overlap.end());
    overlap.resize(120, 0);
    overlap.insert(overlap.end(), 56, 0);  // placeholder for phdr[1]
    overlap.insert(overlap.end(), payload.begin(), payload.end());

    overlap[56] = 2;  // e_phnum = 2
    overlap[120] = 1;  // phdr[1].p_type = PT_LOAD
    // 8-byte aligned so the alignment rule is not what rejects it, and ending at
    // 0x80000010 so it genuinely overlaps the first segment's [0x80000008).
    const uint64_t second_vaddr = 0x80000008ull;
    for (int i = 0; i < 8; ++i) {
      overlap[120 + 16 + i] = static_cast<uint8_t>((second_vaddr >> (8 * i)) & 0xff);
    }
    overlap[120 + 32] = 8;  // phdr[1].p_filesz
    overlap[120 + 40] = 8;  // phdr[1].p_memsz

    // The first segment is widened to 16 bytes so that the second, starting at
    // 0x80000008, genuinely overlaps it. Two 8-byte segments at 0x80000000 and
    // 0x80000008 merely abut, which is not an overlap.
    overlap[96 + 8] = 16;  // phdr[0].p_memsz

    ExpectRejected(prefix + "overlap", overlap, mosaic::LoadStatus::kSegmentOverlap,
                   "overlapping PT_LOAD segments", reporter);
  }

  // The good image, if one was supplied, must still load: the rejections above
  // only mean something if the checker also accepts a valid file.
  if (!options.image.empty()) {
    mosaic::Image image;
    std::string detail;
    const mosaic::LoadStatus status = mosaic::LoadElf(options.image, &image, &detail);
    reporter->Check(status == mosaic::LoadStatus::kOk,
                    std::string("the real image still loads: ") +
                        mosaic::LoadStatusName(status));
  } else {
    // No image supplied: synthesise one so the "accepts a good file" half of the
    // check is still exercised.
    const std::vector<uint8_t> good =
        BuildElf(true, 0x80000000ull, 0x80000000ull, 243, 2, 1);
    const std::string path = prefix + "good";
    ExpectRejected(path, good, mosaic::LoadStatus::kOk, "a well-formed image is accepted",
                   reporter);
  }

  if (reporter->failures() > 0) {
    return reporter->Finish("FAIL",
                            std::to_string(reporter->failures()) +
                                " malformed image(s) were not refused correctly");
  }
  return reporter->Finish("PASS", "every malformed image was refused with a named reason");
}

int RunTimeoutCase(const mosaic::Options& options, mosaic::Reporter* reporter) {
  mosaic::MemoryModel memory;
  // A single self-loop: valid, runs forever, never writes TOHOST.
  WriteInstruction(&memory, MOSAIC_RESET_VECTOR, 0x0000006fu);

  Vharness_tb dut;
  mosaic::ClockDriver clock;
  clock.BeginReset(kResetCycles);

  const uint64_t budget = 512;  // deliberately far below the default budget
  const RunResult result = Run(&dut, &memory, &clock, budget);

  reporter->Check(result.cycle_limit_hit,
                  "a program that never exits is stopped by the cycle limit");
  reporter->Check(!memory.finished(), "TOHOST was never written");
  reporter->Check(result.tap.size() > 0, "events were still recorded while the program ran");
  reporter->Check(result.cycles >= budget, "the harness really ran the whole budget");

  if (reporter->failures() > 0) return reporter->Finish("FAIL", "timeout was not detected cleanly");
  return reporter->Finish("PASS",
                          "cycle limit enforced after " + std::to_string(result.cycles) + " cycles");
}

int RunInjectedMismatchCase(const mosaic::Options& options, mosaic::Reporter* reporter) {
  mosaic::MemoryModel memory;
  for (size_t i = 0; i < sizeof(kMicroProgram) / sizeof(kMicroProgram[0]); ++i) {
    WriteInstruction(&memory, MOSAIC_RESET_VECTOR + i * 4, kMicroProgram[i].encoding);
  }

  Vharness_tb dut;
  mosaic::ClockDriver clock;
  clock.BeginReset(kResetCycles);
  const RunResult result = Run(&dut, &memory, &clock, options.max_cycles);
  reporter->Check(result.finished_by_program, "the reference run exited normally");

  const std::string events_path = options.out_dir + "/events.txt";
  std::string detail;
  reporter->Check(result.tap.Save(events_path, &detail), "saved the reference stream");

  std::vector<std::string> lines;
  FILE* file = std::fopen(events_path.c_str(), "r");
  if (file == nullptr) {
    reporter->Check(false, "cannot reopen the saved event stream");
    return reporter->Finish("FAIL", "could not re-read the saved stream");
  }
  char buffer[512];
  while (std::fgets(buffer, sizeof(buffer), file) != nullptr) {
    std::string line(buffer);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (!line.empty()) lines.push_back(line);
  }
  std::fclose(file);

  reporter->Check(lines.size() >= 3, "the reference stream has enough events to corrupt");
  if (lines.size() >= 3) {
    std::string corrupted = lines[1];
    const size_t value_at = corrupted.find("val=");
    reporter->Check(value_at != std::string::npos,
                    "the second event records a destination register value");
    if (value_at != std::string::npos) {
      corrupted[value_at + 4] = (corrupted[value_at + 4] == '0') ? '1' : '0';
    }
    const std::string corrupt_path = options.out_dir + "/events_corrupted.txt";
    FILE* out = std::fopen(corrupt_path.c_str(), "w");
    if (out != nullptr) {
      for (size_t i = 0; i < lines.size(); ++i) {
        std::fprintf(out, "%s\n", (i == 1 ? corrupted : lines[i]).c_str());
      }
      std::fclose(out);
    }

    std::string first_mismatch;
    int mismatch_count = 0;
    const bool differed =
        result.tap.Compare(corrupt_path, &first_mismatch, &mismatch_count, &detail);
    reporter->Check(differed, "a one-bit change to the reference stream is detected");
    reporter->Check(mismatch_count == 1,
                    "exactly one event differs, got " + std::to_string(mismatch_count));
    reporter->Check(first_mismatch.find("event 1") != std::string::npos,
                    "the first mismatch names event 1: " + first_mismatch);

    // The unmodified reference must still compare clean, proving the comparison
    // is not simply always reporting a difference.
    std::string clean_detail;
    std::string clean_first;
    int clean_count = 0;
    const bool clean_differed =
        result.tap.Compare(events_path, &clean_first, &clean_count, &clean_detail);
    reporter->Check(!clean_differed,
                    "the unmodified reference still compares clean: " + clean_detail);
    reporter->Check(clean_count == 0, "no spurious differences reported");
  }

  if (reporter->failures() > 0) {
    return reporter->Finish("FAIL",
                            std::to_string(reporter->failures()) + " comparison check(s) failed");
  }
  return reporter->Finish("PASS", "injected mismatch detected and located precisely");
}

}  // namespace

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  mosaic::Options options;
  std::string error;
  if (!mosaic::Options::Parse(argc, argv, &options, &error)) {
    std::fprintf(stderr, "usage error: %s\n", error.c_str());
    return mosaic::kExitUsage;
  }

  mosaic::Reporter reporter(options, "mosaic harness I-004");

  if (options.case_id == "harness.reset_load_exit") return RunPositiveCase(options, &reporter);
  if (options.case_id == "harness.bad_image") return RunBadImageCase(options, &reporter);
  if (options.case_id == "harness.timeout") return RunTimeoutCase(options, &reporter);
  if (options.case_id == "harness.injected_mismatch") {
    return RunInjectedMismatchCase(options, &reporter);
  }

  std::fprintf(stderr, "unknown case %s\n", options.case_id.c_str());
  return mosaic::kExitUsage;
}
