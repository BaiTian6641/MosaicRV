// ELF64 little-endian loader for the MosaicRV simulation harness.
//
// Deliberately small and strict. A harness that accepts a malformed image and
// then "passes" is worse than one that refuses to run: the card for I-004 names
// bad-image handling as a negative control, so every rejection below has a named
// reason that is reported rather than a generic failure.

#ifndef MOSAIC_ELF_LOADER_H_
#define MOSAIC_ELF_LOADER_H_

#include <cstdint>
#include <string>
#include <vector>

namespace mosaic {

struct Segment {
  uint64_t vaddr;
  uint64_t filesz;
  uint64_t memsz;
  uint32_t flags = 0;         // p_flags (PF_R=4, PF_W=2, PF_X=1)
  std::vector<uint8_t> data;  // filesz bytes; the memsz-filesz tail must be zero

  // Written so it cannot overflow: `vaddr + memsz` wraps for a segment that
  // claims to run off the top of the address space, and a wrapped bound would
  // make a wild address look mapped.
  bool Contains(uint64_t address) const {
    return address >= vaddr && (address - vaddr) < memsz;
  }
};

// One entry of .symtab, as far as a harness needs one. `value` is the symbol's
// address for the STT_FUNC/STT_OBJECT/STT_NOTYPE symbols a testbench looks up.
struct Symbol {
  std::string name;
  uint64_t value = 0;
  uint64_t size = 0;
  uint8_t info = 0;      // st_info: binding in the high nibble, type in the low
  uint16_t shndx = 0;
};

struct Image {
  std::vector<Segment> segments;
  uint64_t entry = 0;
  std::string source;
  // Best effort: a stripped image simply has no symbols, and that is not a load
  // failure -- the segment map is what the loader guarantees. A harness that
  // needs a symbol says so itself, with a named failure, when it is missing.
  std::vector<Symbol> symbols;

  const Segment* Find(uint64_t address) const;
  const Symbol* FindSymbol(const std::string& name) const;
  uint64_t LowestAddress() const;
};

// Reason codes are stable strings so the harness can report exactly why a load
// was refused and a negative control can assert on the reason.
enum class LoadStatus {
  kOk,
  kCannotRead,
  kBadMagic,
  kNotElf64,
  kNotLittleEndian,
  kNotRiscv,
  kTruncated,
  kNoLoadableSegment,
  kEntryNotMapped,
  kSegmentOverlap,
  kSegmentAlignment,
  kBadProgramHeader,
  // Appended after the original set so a stored status number, if any exists in
  // an older result file, still names the same reason.
  kSegmentOverflow,     // vaddr+memsz or offset+filesz wraps uint64
  kNotExecutable,       // e_type is not ET_EXEC (a dynamic/PIE image has no fixed load address)
  kEntryNotExecutable,  // the segment holding the entry declares flags and omits PF_X
};

const char* LoadStatusName(LoadStatus status);

// Loads an ELF64 RISC-V little-endian executable.
LoadStatus LoadElf(const std::string& path, Image* out, std::string* detail);

// Loads a flat binary at a fixed address. Used by negative controls and by
// hand-written instruction streams that are not worth assembling.
LoadStatus LoadFlatBinary(const std::string& path, uint64_t address, Image* out,
                          std::string* detail);

}  // namespace mosaic

#endif  // MOSAIC_ELF_LOADER_H_