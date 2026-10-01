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
    const uint32_t type = ReadU32(raw, base + 0);
    if (type != kPtLoad) continue;

    const uint64_t offset = ReadU64(raw, base + 8);
    const uint64_t vaddr = ReadU64(raw, base + 16);
    const uint64_t filesz = ReadU64(raw, base + 32);
    const uint64_t memsz = ReadU64(raw, base + 40);

    if (memsz == 0) continue;
    if (filesz > memsz) {
      *detail = "PT_LOAD has p_filesz larger than p_memsz";
      return LoadStatus::kBadProgramHeader;
    }
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
    segment.data.assign(raw.begin() + static_cast<long>(offset),
                        raw.begin() + static_cast<long>(offset + filesz));
    out->segments.push_back(std::move(segment));
  }

  if (out->segments.empty()) {
    *detail = "image contains no PT_LOAD segment";
    return LoadStatus::kNoLoadableSegment;
  }

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

  if (out->Find(entry) == nullptr) {
    char text[128];
    std::snprintf(text, sizeof(text),
                  "entry point 0x%llx is not inside any PT_LOAD segment",
                  static_cast<unsigned long long>(entry));
    *detail = text;
    return LoadStatus::kEntryNotMapped;
  }

  out->entry = entry;
  detail->clear();
  return LoadStatus::kOk;
}

}  // namespace mosaic