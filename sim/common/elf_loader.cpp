#include "elf_loader.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace mosaic {
namespace {

// ELF identification and structures, little-endian, as laid out by the
// System V gABI. Values are read byte by byte so the loader does not depend on
// the host's struct packing.
const uint8_t kElfMagic[4] = {0x7f, 'E', 'L', 'F'};
const uint8_t kClass64 = 2;
const uint8_t kDataLittle = 1;
const uint16_t kMachineRiscv = 243;
const uint32_t kPtLoad = 1;
const uint16_t kEtExec = 2;   // e_type of a fixed-address executable
const uint32_t kPfX = 1;      // p_flags execute bit

uint16_t ReadU16(const std::vector<uint8_t>& data, size_t offset) {
  return static_cast<uint16_t>(data[offset]) |
         (static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t ReadU32(const std::vector<uint8_t>& data, size_t offset) {
  uint32_t value = 0;
  for (int i = 3; i >= 0; --i) {
    value = (value << 8) | data[offset + static_cast<size_t>(i)];
  }
  return value;
}

uint64_t ReadU64(const std::vector<uint8_t>& data, size_t offset) {
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) {
    value = (value << 8) | data[offset + static_cast<size_t>(i)];
  }
  return value;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return false;
  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  if (size < 0) return false;
  stream.seekg(0, std::ios::beg);
  out->resize(static_cast<size_t>(size));
  if (size > 0) stream.read(reinterpret_cast<char*>(out->data()), size);
  return stream.good() || stream.eof();
}

}  // namespace

const char* LoadStatusName(LoadStatus status) {
  switch (status) {
    case LoadStatus::kOk: return "ok";
    case LoadStatus::kCannotRead: return "cannot-read";
    case LoadStatus::kBadMagic: return "bad-magic";
    case LoadStatus::kNotElf64: return "not-elf64";
    case LoadStatus::kNotLittleEndian: return "not-little-endian";
    case LoadStatus::kNotRiscv: return "not-riscv";
    case LoadStatus::kTruncated: return "truncated";
    case LoadStatus::kNoLoadableSegment: return "no-loadable-segment";
    case LoadStatus::kEntryNotMapped: return "entry-not-mapped";
    case LoadStatus::kSegmentOverlap: return "segment-overlap";
    case LoadStatus::kSegmentAlignment: return "segment-alignment";
    case LoadStatus::kBadProgramHeader: return "bad-program-header";
    case LoadStatus::kSegmentOverflow: return "segment-overflow";
    case LoadStatus::kNotExecutable: return "not-executable";
    case LoadStatus::kEntryNotExecutable: return "entry-not-executable";
  }
  return "unknown";
}

const Segment* Image::Find(uint64_t address) const {
  for (const Segment& segment : segments) {
    if (segment.Contains(address)) return &segment;
  }
  return nullptr;
}

uint64_t Image::LowestAddress() const {
  uint64_t lowest = UINT64_MAX;
  for (const Segment& segment : segments) {
    lowest = std::min(lowest, segment.vaddr);
  }
  return lowest == UINT64_MAX ? 0 : lowest;
}

LoadStatus LoadFlatBinary(const std::string& path, uint64_t address, Image* out,
                          std::string* detail) {
  std::vector<uint8_t> raw;
  if (!ReadFile(path, &raw)) {
    *detail = "cannot read " + path;
    return LoadStatus::kCannotRead;
  }
  Segment segment;
  segment.vaddr = address;
  segment.filesz = raw.size();
  segment.memsz = raw.size();
  segment.data = raw;
  out->segments.assign(1, segment);
  out->entry = address;
  out->source = path;
  detail->clear();
  return LoadStatus::kOk;
}

LoadStatus LoadElf(const std::string& path, Image* out, std::string* detail) {
  out->segments.clear();
  out->entry = 0;
  out->source = path;

  std::vector<uint8_t> raw;
  if (!ReadFile(path, &raw)) {
    *detail = "cannot read " + path;
    return LoadStatus::kCannotRead;
  }

  // 64 bytes of ELF64 header, 56 bytes per program header.
  const size_t kEhdrSize = 64;
  const size_t kPhdrSize = 56;
  if (raw.size() < kEhdrSize) {
    *detail = "file is shorter than an ELF64 header";
    return LoadStatus::kTruncated;
  }
  if (std::memcmp(raw.data(), kElfMagic, sizeof(kElfMagic)) != 0) {
    *detail = "missing \\x7fELF magic";
    return LoadStatus::kBadMagic;
  }
  if (raw[4] != kClass64) {
    *detail = "ELF class is not ELFCLASS64";
    return LoadStatus::kNotElf64;
  }
  if (raw[5] != kDataLittle) {
    *detail = "ELF data encoding is not ELFDATA2LSB";
    return LoadStatus::kNotLittleEndian;
  }
  if (raw[6] != 1) {
    *detail = "ELF version is not EV_CURRENT";
    return LoadStatus::kBadProgramHeader;
  }

  // A dynamic object is position independent: its PT_LOAD p_vaddr is a link-time
  // address and the real load address is chosen by the loader at run time. This
  // harness has no relocation step, so accepting one would load the image at the
  // wrong address and the failure would appear much later as a wild fetch.
#ifndef MOSAIC_ELF_MUTANT_ACCEPT_DYNAMIC
  const uint16_t type = ReadU16(raw, 16);
  if (type != kEtExec) {
    char text[96];
    std::snprintf(text, sizeof(text), "e_type is %u, expected %u (ET_EXEC)",
                  type, kEtExec);
    *detail = text;
    return LoadStatus::kNotExecutable;
  }
#endif

  const uint16_t machine = ReadU16(raw, 18);
  if (machine != kMachineRiscv) {
    char text[96];
    std::snprintf(text, sizeof(text), "e_machine is %u, expected %u (EM_RISCV)",
                  machine, kMachineRiscv);
    *detail = text;
    return LoadStatus::kNotRiscv;
  }

  const uint64_t entry = ReadU64(raw, 24);
  const uint64_t phoff = ReadU64(raw, 32);
  const uint16_t phentsize = ReadU16(raw, 54);
  const uint16_t phnum = ReadU16(raw, 56);

  if (phnum == 0) {
    *detail = "no program headers";
    return LoadStatus::kNoLoadableSegment;
  }
  if (phentsize != kPhdrSize) {
    char text[96];
    std::snprintf(text, sizeof(text), "e_phentsize is %u, expected %zu",
                  phentsize, kPhdrSize);
    *detail = text;
    return LoadStatus::kBadProgramHeader;
  }
  if (phoff > raw.size() || phoff + static_cast<uint64_t>(phnum) * kPhdrSize > raw.size()) {
    *detail = "program header table runs past the end of the file";
    return LoadStatus::kTruncated;
  }

  for (uint16_t i = 0; i < phnum; ++i) {
    const size_t base = static_cast<size_t>(phoff) + static_cast<size_t>(i) * kPhdrSize;
    const uint32_t segment_type = ReadU32(raw, base + 0);
    if (segment_type != kPtLoad) continue;

    const uint64_t offset = ReadU64(raw, base + 8);
    const uint64_t vaddr = ReadU64(raw, base + 16);
    const uint64_t filesz = ReadU64(raw, base + 32);
    const uint64_t memsz = ReadU64(raw, base + 40);
    const uint32_t flags = ReadU32(raw, base + 4);

    if (memsz == 0) continue;
    if (filesz > memsz) {
      *detail = "PT_LOAD has p_filesz larger than p_memsz";
      return LoadStatus::kBadProgramHeader;
    }
    // Both bounds are sums of two attacker-controlled 64-bit fields. Checked
    // before they are formed, because `offset + filesz` wrapping below
    // `raw.size()` would make a segment that runs off the end of the file look
    // like it fits, and `vaddr + memsz` wrapping would make a wild segment look
    // like it maps the entry point.
#ifndef MOSAIC_ELF_MUTANT_NO_SEGMENT_OVERFLOW_CHECK
    if (UINT64_MAX - vaddr < memsz) {
      char text[112];
      std::snprintf(text, sizeof(text),
                    "PT_LOAD vaddr 0x%llx + memsz %llu wraps the address space",
                    static_cast<unsigned long long>(vaddr),
                    static_cast<unsigned long long>(memsz));
      *detail = text;
      return LoadStatus::kSegmentOverflow;
    }
    if (UINT64_MAX - offset < filesz) {
      *detail = "PT_LOAD p_offset + p_filesz wraps the file offset space";
      return LoadStatus::kSegmentOverflow;
    }
#endif
    if (vaddr % 8 != 0) {
      char text[96];
      std::snprintf(text, sizeof(text), "PT_LOAD vaddr 0x%llx is not 8-byte aligned",
                    static_cast<unsigned long long>(vaddr));
      *detail = text;
      return LoadStatus::kSegmentAlignment;
    }
    if (offset + filesz > raw.size()) {
      char text[128];
      std::snprintf(text, sizeof(text),
                    "PT_LOAD segment at 0x%llx needs %llu file bytes but the file is %zu",
                    static_cast<unsigned long long>(vaddr),
                    static_cast<unsigned long long>(filesz), raw.size());
      *detail = text;
      return LoadStatus::kTruncated;
    }

    Segment segment;
    segment.vaddr = vaddr;
    segment.filesz = filesz;
    segment.memsz = memsz;
    segment.flags = flags;
    segment.data.assign(raw.begin() + static_cast<long>(offset),
                        raw.begin() + static_cast<long>(offset + filesz));
    out->segments.push_back(std::move(segment));
  }

#ifdef MOSAIC_ELF_MUTANT_LOAD_FIRST_SEGMENT_ONLY
  // Negative control: a loader that silently drops every PT_LOAD after the
  // first. The case must notice that the second segment never reached memory.
  if (out->segments.size() > 1) out->segments.resize(1);
#endif

  if (out->segments.empty()) {
    *detail = "image contains no PT_LOAD segment";
    return LoadStatus::kNoLoadableSegment;
  }

#ifndef MOSAIC_ELF_MUTANT_SKIP_OVERLAP_CHECK
  std::sort(out->segments.begin(), out->segments.end(),
            [](const Segment& a, const Segment& b) { return a.vaddr < b.vaddr; });
  for (size_t i = 1; i < out->segments.size(); ++i) {
    const Segment& previous = out->segments[i - 1];
    const Segment& current = out->segments[i];
    if (previous.vaddr + previous.memsz > current.vaddr) {
      char text[160];
      std::snprintf(text, sizeof(text),
                    "PT_LOAD segments at 0x%llx and 0x%llx overlap",
                    static_cast<unsigned long long>(previous.vaddr),
                    static_cast<unsigned long long>(current.vaddr));
      *detail = text;
      return LoadStatus::kSegmentOverlap;
    }
  }
#else
  std::sort(out->segments.begin(), out->segments.end(),
            [](const Segment& a, const Segment& b) { return a.vaddr < b.vaddr; });
#endif

  const Segment* entry_segment = out->Find(entry);
  if (entry_segment == nullptr) {
    char text[128];
    std::snprintf(text, sizeof(text),
                  "entry point 0x%llx is not inside any PT_LOAD segment",
                  static_cast<unsigned long long>(entry));
    *detail = text;
    return LoadStatus::kEntryNotMapped;
  }
  // An image that declares segment permissions must mark the entry executable:
  // fetching from a read-only data segment is a link error, not a runtime
  // accident, and the harness has no way to report it later as anything but a
  // wild fetch. An image that declares no permissions at all (p_flags == 0) is
  // taken at face value: a hand-built or minimal image makes no claim to
  // contradict. The I-004 harness case builds exactly such an image.
#ifndef MOSAIC_ELF_MUTANT_ALLOW_NONEXEC_ENTRY
  if (entry_segment->flags != 0 && (entry_segment->flags & kPfX) == 0) {
    char text[144];
    std::snprintf(text, sizeof(text),
                  "entry point 0x%llx is in a PT_LOAD with p_flags 0x%x, which is not "
                  "executable",
                  static_cast<unsigned long long>(entry), entry_segment->flags);
    *detail = text;
    return LoadStatus::kEntryNotExecutable;
  }
#endif

#ifdef MOSAIC_ELF_MUTANT_IGNORE_ENTRY
  // Negative control: the header's e_entry is ignored and the image is entered
  // at its lowest address.
  out->entry = out->LowestAddress();
#else
  out->entry = entry;
#endif
  detail->clear();
  return LoadStatus::kOk;
}

}  // namespace mosaic