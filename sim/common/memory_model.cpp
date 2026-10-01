#include "memory_model.h"

#include "mosaic_platform.h"

namespace mosaic {
namespace {

const uint64_t kPageSize = 4096;

}  // namespace

const char* AccessStatusName(AccessStatus status) {
  switch (status) {
    case AccessStatus::kOk: return "ok";
    case AccessStatus::kAccessFault: return "access-fault";
    case AccessStatus::kReadOnly: return "read-only";
    case AccessStatus::kUnaligned: return "unaligned";
    case AccessStatus::kDeviceError: return "device-error";
  }
  return "unknown";
}

MemoryModel::MemoryModel() {
  regions_ = {
      {"boot_rom", MOSAIC_BOOT_ROM_BASE, MOSAIC_BOOT_ROM_SIZE, RegionKind::kRom, false, true, true},
      {"uart", MOSAIC_UART_BASE, MOSAIC_UART_SIZE, RegionKind::kUart, true, false, true},
      {"test_harness", MOSAIC_TEST_HARNESS_BASE, MOSAIC_TEST_HARNESS_SIZE,
       RegionKind::kTestHarness, true, false, true},
      {"clint", MOSAIC_CLINT_BASE, MOSAIC_CLINT_SIZE, RegionKind::kClint, true, false, true},
      {"ram", MOSAIC_RAM_BASE, MOSAIC_RAM_SIZE, RegionKind::kRam, true, true, true},
  };
}

const MemoryModel::Region* MemoryModel::Lookup(uint64_t address) const {
  for (const Region& region : regions_) {
    if (address >= region.base && address < region.base + region.size) return &region;
  }
  return nullptr;
}

uint8_t* MemoryModel::BytePointer(uint64_t address) {
  const uint64_t page_index = address / kPageSize;
  auto it = pages_.find(page_index);
  if (it == pages_.end()) {
    std::vector<uint8_t> page(kPageSize, 0);
    it = pages_.emplace(page_index, std::move(page)).first;
  }
  return &it->second[address % kPageSize];
}

const uint8_t* MemoryModel::BytePointer(uint64_t address) const {
  const uint64_t page_index = address / kPageSize;
  auto it = pages_.find(page_index);
  if (it == pages_.end()) return nullptr;
  return &it->second[address % kPageSize];
}

bool MemoryModel::LoadImage(const Image& image, std::string* detail) {
  for (const Segment& segment : image.segments) {
    for (uint64_t offset = 0; offset < segment.memsz; ++offset) {
      const uint64_t address = segment.vaddr + offset;
      const Region* region = Lookup(address);
      if (region == nullptr) {
        char text[192];
        std::snprintf(text, sizeof(text),
                      "image segment at 0x%llx covers 0x%llx, which is outside the %s memory map",
                      static_cast<unsigned long long>(segment.vaddr),
                      static_cast<unsigned long long>(address), MOSAIC_PROFILE_NAME);
        *detail = text;
        return false;
      }
    }
    for (uint64_t i = 0; i < segment.filesz; ++i) {
      *BytePointer(segment.vaddr + i) = segment.data[i];
    }
    // The memsz-filesz tail is .bss: explicit zero, not a whole-memory reset.
    for (uint64_t i = segment.filesz; i < segment.memsz; ++i) {
      *BytePointer(segment.vaddr + i) = 0;
    }
  }
  detail->clear();
  return true;
}

AccessStatus MemoryModel::Read(uint64_t address, unsigned size, uint64_t* value) {
  if (size != 1 && size != 2 && size != 4 && size != 8) return AccessStatus::kUnaligned;
  if (address % size != 0) return AccessStatus::kUnaligned;

  const Region* region = Lookup(address);
  if (region == nullptr || !region->readable) {
    ++access_faults_;
    return AccessStatus::kAccessFault;
  }
  // The whole access must lie inside one region; straddling a boundary would let
  // a single instruction see two different PMA rules.
  if (address + size > region->base + region->size) {
    ++access_faults_;
    return AccessStatus::kAccessFault;
  }

  if (region->kind == RegionKind::kTestHarness) {
    if (address == MOSAIC_TOHOST) {
      *value = 0;  // reads as "still running"
      return AccessStatus::kOk;
    }
    if (address == MOSAIC_FROMHOST) {
      *value = input_word_;
      return AccessStatus::kOk;
    }
    ++access_faults_;
    return AccessStatus::kAccessFault;
  }
  if (region->kind == RegionKind::kClint) {
    *value = 0;
    return AccessStatus::kOk;
  }

  uint64_t result = 0;
  for (unsigned i = 0; i < size; ++i) {
    const uint8_t byte = *BytePointer(address + i);
    result |= static_cast<uint64_t>(byte) << (8 * i);
  }
  *value = result;
  return AccessStatus::kOk;
}

AccessStatus MemoryModel::Write(uint64_t address, unsigned size, uint64_t value) {
  if (size != 1 && size != 2 && size != 4 && size != 8) return AccessStatus::kUnaligned;
  if (address % size != 0) return AccessStatus::kUnaligned;

  const Region* region = Lookup(address);
  if (region == nullptr) {
    ++access_faults_;
    return AccessStatus::kAccessFault;
  }
  if (!region->writable) {
    ++access_faults_;
    return AccessStatus::kReadOnly;
  }
  if (address + size > region->base + region->size) {
    ++access_faults_;
    return AccessStatus::kAccessFault;
  }

  device_events_.push_back({address, value, true});

  switch (region->kind) {
    case RegionKind::kUart:
      if (size == 1) uart_.push_back(static_cast<char>(value & 0xff));
      else if (address == MOSAIC_UART_BASE) {
        for (unsigned i = 0; i < size; ++i) {
          uart_.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
        }
      }
      return AccessStatus::kOk;

    case RegionKind::kTestHarness:
      if (address == MOSAIC_TOHOST) {
        if (value != 0) {
          finished_ = true;
          passed_ = (value & 1u) != 0;
          exit_code_ = value;
        }
        return AccessStatus::kOk;
      }
      if (address == MOSAIC_FROMHOST) {
        input_word_ = value;  // lets a program hand data back to the harness
        return AccessStatus::kOk;
      }
      ++access_faults_;
      return AccessStatus::kAccessFault;

    case RegionKind::kClint:
    case RegionKind::kRam:
    case RegionKind::kRom:
      for (unsigned i = 0; i < size; ++i) {
        *BytePointer(address + i) = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
      }
      return AccessStatus::kOk;
  }
  return AccessStatus::kAccessFault;
}

bool MemoryModel::ReadSignature(std::vector<uint64_t>* words) const {
  words->clear();
  for (int i = 0; i < MOSAIC_SIGNATURE_WORDS; ++i) {
    const uint64_t address = MOSAIC_SIGNATURE_ADDR + static_cast<uint64_t>(i) * 8;
    const Region* region = Lookup(address);
    if (region == nullptr || !region->readable) return false;
    uint64_t value = 0;
    for (unsigned b = 0; b < 8; ++b) {
      const uint8_t* byte = BytePointer(address + b);
      if (byte == nullptr) return false;
      value |= static_cast<uint64_t>(*byte) << (8 * b);
    }
    words->push_back(value);
  }
  return true;
}

}  // namespace mosaic