#include "event_tap.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace mosaic {

std::string RetireEvent::Line() const {
  char buffer[320];
  int written = std::snprintf(
      buffer, sizeof(buffer),
      "hart=%u seq=%016llx pc=%016llx next_pc=%016llx insn=%08x", hart,
      static_cast<unsigned long long>(seq), static_cast<unsigned long long>(pc),
      static_cast<unsigned long long>(next_pc), insn);
  std::string line(buffer, static_cast<size_t>(written < 0 ? 0 : written));

  char tail[256];
  if (has_rd) {
    std::snprintf(tail, sizeof(tail), " rd=x%u val=%016llx", rd,
                  static_cast<unsigned long long>(rd_value));
    line += tail;
  }
  if (is_trap) {
    std::snprintf(tail, sizeof(tail), " TRAP cause=%llx tval=%016llx epc=%016llx",
                  static_cast<unsigned long long>(cause),
                  static_cast<unsigned long long>(tval),
                  static_cast<unsigned long long>(epc));
    line += tail;
  }
  if (is_store) {
    std::snprintf(tail, sizeof(tail), " STORE addr=%016llx data=%016llx size=%u",
                  static_cast<unsigned long long>(store_address),
                  static_cast<unsigned long long>(store_data), store_size);
    line += tail;
  }
  return line;
}

void EventTap::Record(const RetireEvent& event) { events_.push_back(event); }

bool EventTap::Save(const std::string& path, std::string* detail) const {
  std::ofstream out(path);
  if (!out) {
    *detail = "cannot open " + path + " for writing";
    return false;
  }
  for (const RetireEvent& event : events_) {
    out << event.Line() << "\n";
  }
  detail->clear();
  return true;
}

bool EventTap::Compare(const std::string& reference_path, std::string* first_mismatch,
                       int* mismatch_count, std::string* detail) const {
  first_mismatch->clear();
  *mismatch_count = 0;

  std::ifstream reference(reference_path);
  if (!reference) {
    *detail = "cannot open reference " + reference_path;
    return false;
  }

  std::vector<std::string> reference_lines;
  std::string line;
  while (std::getline(reference, line)) {
    if (!line.empty()) reference_lines.push_back(line);
  }

  if (reference_lines.size() != events_.size()) {
    char text[192];
    std::snprintf(text, sizeof(text),
                  "event count differs: run has %zu, reference has %zu",
                  events_.size(), reference_lines.size());
    *detail = text;
    *mismatch_count = static_cast<int>(
        events_.size() > reference_lines.size() ? events_.size() - reference_lines.size()
                                               : reference_lines.size() - events_.size());
    return true;  // a difference was found; the caller decides the verdict
  }

  for (size_t i = 0; i < events_.size(); ++i) {
    const std::string actual = events_[i].Line();
    if (actual != reference_lines[i]) {
      ++*mismatch_count;
      if (first_mismatch->empty()) {
        std::ostringstream text;
        text << "event " << i << ": reference \"" << reference_lines[i]
             << "\" vs run \"" << actual << "\"";
        *first_mismatch = text.str();
      }
    }
  }

  if (*mismatch_count > 0) {
    char text[128];
    std::snprintf(text, sizeof(text), "%d of %zu events differ",
                  *mismatch_count, events_.size());
    *detail = text;
    return true;
  }

  *detail = "all " + std::to_string(events_.size()) + " events match";
  return false;
}

}  // namespace mosaic