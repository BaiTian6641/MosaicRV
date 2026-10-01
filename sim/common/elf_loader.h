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
  std::vector<uint8_t> data;  // filesz bytes; the memsz-filesz tail must be zero

  bool Contains(uint64_t address) const {
    return address >= vaddr && address < vaddr + memsz;
  }
};

struct Image {
  std::vector<Segment> segments;
  uint64_t entry = 0;
  std::string source;

  const Segment* Find(uint64_t address) const;
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