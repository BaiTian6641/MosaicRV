// Canonical architectural event stream.
//
// The plan requires `execution-done`, `value-visible`, `macro-complete` and
// `retired` to stay distinct, and requires every architectural event to be
// comparable against an independent reference. This is the single serialisation
// used for that comparison: one line per event, fixed field order, hex values,
// no timestamps and no cycle numbers that would make a replay depend on timing.
//
// A reference event file is plain text on purpose. A differential failure has to
// be readable by a person and diffable by a tool, and it has to survive a Verilator
// upgrade.

#ifndef MOSAIC_EVENT_TAP_H_
#define MOSAIC_EVENT_TAP_H_

#include <cstdint>
#include <string>
#include <vector>

namespace mosaic {

struct RetireEvent {
  uint32_t hart = 0;
  uint64_t seq = 0;       // per-hart monotonic, wraps at the contracted modulus
  uint64_t pc = 0;
  uint64_t next_pc = 0;
  uint32_t insn = 0;

  bool has_rd = false;
  uint8_t rd = 0;
  uint64_t rd_value = 0;

  bool is_trap = false;
  uint64_t cause = 0;
  uint64_t tval = 0;
  uint64_t epc = 0;

  bool is_store = false;
  uint64_t store_address = 0;
  uint64_t store_data = 0;
  unsigned store_size = 0;

  std::string Line() const;
};

class EventTap {
 public:
  void Record(const RetireEvent& event);

  bool Save(const std::string& path, std::string* detail) const;

  // Compares this run's stream against a reference file. On the first difference
  // it fills `first_mismatch` with both lines and the event index, and reports
  // the total number of differing events.
  bool Compare(const std::string& reference_path, std::string* first_mismatch,
               int* mismatch_count, std::string* detail) const;

  size_t size() const { return events_.size(); }
  const std::vector<RetireEvent>& events() const { return events_; }

 private:
  std::vector<RetireEvent> events_;
};

}  // namespace mosaic

#endif  // MOSAIC_EVENT_TAP_H_