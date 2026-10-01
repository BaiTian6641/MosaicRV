// Sparse physical memory plus the p0 platform devices.
//
// The memory model is driven entirely by the generated constants in
// `mosaic_platform.h`, so the harness, the firmware and the RTL read the same
// frozen map. Anything the map does not cover raises an access fault rather than
// silently reading as zero: a permissive memory model hides exactly the bugs
// this project is trying to find.

#ifndef MOSAIC_MEMORY_MODEL_H_
#define MOSAIC_MEMORY_MODEL_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "elf_loader.h"

namespace mosaic {

enum class AccessStatus {
  kOk,
  kAccessFault,       // no region covers the address, or the region denies it
  kReadOnly,          // write to a read-only region (boot_rom)
  kUnaligned,         // misaligned access under the frozen policy
  kDeviceError,       // device rejected the access
};

const char* AccessStatusName(AccessStatus status);

struct DeviceEvent {
  uint64_t address;
  uint64_t value;
  bool is_write;
};

class MemoryModel {
 public:
  MemoryModel();

  // Copies the image's loadable segments into memory. Returns false and fills
  // `detail` if a segment lands outside the declared map, which means the image
  // was linked against a different memory map than the one this profile froze.
  bool LoadImage(const Image& image, std::string* detail);

  void SetInputWord(uint64_t value) { input_word_ = value; }

  AccessStatus Read(uint64_t address, unsigned size, uint64_t* value);
  AccessStatus Write(uint64_t address, unsigned size, uint64_t value);

  // Reads the program signature words written at MOSAIC_SIGNATURE_ADDR.
  bool ReadSignature(std::vector<uint64_t>* words) const;

  bool finished() const { return finished_; }
  bool passed() const { return passed_; }
  uint64_t exit_code() const { return exit_code_; }
  uint64_t access_fault_count() const { return access_faults_; }

  const std::vector<DeviceEvent>& device_events() const { return device_events_; }
  const std::string& uart_output() const { return uart_; }

 private:
  enum class RegionKind { kRam, kRom, kUart, kTestHarness, kClint };

  struct Region {
    const char* name;
    uint64_t base;
    uint64_t size;
    RegionKind kind;
    bool writable;
    bool executable;
    bool readable;
  };

  const Region* Lookup(uint64_t address) const;

  uint8_t* BytePointer(uint64_t address);
  const uint8_t* BytePointer(uint64_t address) const;

  std::map<uint64_t, std::vector<uint8_t>> pages_;
  std::vector<Region> regions_;

  bool finished_ = false;
  bool passed_ = false;
  uint64_t exit_code_ = 0;
  uint64_t input_word_ = 0;
  uint64_t access_faults_ = 0;
  std::string uart_;
  std::vector<DeviceEvent> device_events_;
};

}  // namespace mosaic

#endif  // MOSAIC_MEMORY_MODEL_H_