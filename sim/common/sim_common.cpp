#include "sim_common.h"

#include <sys/stat.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace mosaic {
namespace {

bool ParseU64(const char* text, uint64_t* out) {
  if (text == nullptr || *text == '\0') return false;
  char* end = nullptr;
  unsigned long long value = std::strtoull(text, &end, 0);
  if (end == text || *end != '\0') return false;
  *out = static_cast<uint64_t>(value);
  return true;
}

void MakeDirs(const std::string& path) {
  if (path.empty()) return;
  std::string partial;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      if (!partial.empty()) mkdir(partial.c_str(), 0755);
    }
    if (i < path.size()) partial.push_back(path[i]);
  }
}

std::string JsonEscape(const std::string& text) {
  std::string out;
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

std::string Timestamp() {
  std::time_t now = std::time(nullptr);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
  return buffer;
}

}  // namespace

std::string Hex(uint64_t value, int digits) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "0x%0*llx", digits,
                static_cast<unsigned long long>(value));
  return buffer;
}

std::string HexSigned(int64_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%lld (0x%llx)",
                static_cast<long long>(value),
                static_cast<unsigned long long>(static_cast<uint64_t>(value)));
  return buffer;
}

bool Options::Parse(int argc, char** argv, Options* out, std::string* error) {
  for (int i = 1; i < argc; ++i) {
    std::string flag = argv[i];
    const bool has_value = (i + 1) < argc;
    const char* value = has_value ? argv[i + 1] : "";

    if (flag == "--case" && has_value) {
      out->case_id = value;
      ++i;
    } else if (flag == "--out" && has_value) {
      out->out_dir = value;
      ++i;
    } else if (flag == "--image" && has_value) {
      out->image = value;
      ++i;
    } else if (flag == "--expect" && has_value) {
      out->expect = value;
      ++i;
    } else if (flag == "--seed" && has_value) {
      if (!ParseU64(value, &out->seed)) {
        *error = "--seed expects a number, got '" + std::string(value) + "'";
        return false;
      }
      ++i;
    } else if (flag == "--max-cycles" && has_value) {
      if (!ParseU64(value, &out->max_cycles) || out->max_cycles == 0) {
        *error = "--max-cycles expects a positive number";
        return false;
      }
      ++i;
    } else if (flag == "--verbose") {
      out->verbose = true;
    } else {
      *error = "unrecognised argument '" + flag + "'";
      return false;
    }
  }
  if (out->case_id.empty()) {
    *error = "--case is required so the result can be attributed to a named case";
    return false;
  }
  return true;
}

void ClockDriver::BeginReset(int cycles) {
  reset_cycles_ = cycles;
  in_reset_ = true;
  cycle_ = 0;
}

uint64_t ClockDriver::RisingEdge() {
  ++cycle_;
  if (in_reset_ && cycle_ > static_cast<uint64_t>(reset_cycles_)) {
    in_reset_ = false;
  }
  return cycle_;
}

uint64_t ClockDriver::FallingEdge() { return cycle_; }

uint64_t ClockDriver::Tick() {
  const uint64_t cycle = RisingEdge();
  return cycle;
}

Reporter::Reporter(const Options& options, const std::string& tool_version)
    : options_(options), tool_version_(tool_version), checks_(0), failures_(0) {}

void Reporter::Check(bool passed, const std::string& what) {
  ++checks_;
  if (!passed) {
    ++failures_;
    if (messages_.size() < 32) messages_.push_back(what);
    std::fprintf(stderr, "CHECK FAILED: %s\n", what.c_str());
  } else if (options_.verbose) {
    std::fprintf(stderr, "check ok: %s\n", what.c_str());
  }
}

void Reporter::Mismatch(const std::string& where, const std::string& expected,
                        const std::string& actual) {
  if (!first_mismatch_.empty()) return;
  std::ostringstream text;
  text << where << ": expected " << expected << ", got " << actual;
  first_mismatch_ = text.str();
  std::fprintf(stderr, "MISMATCH %s\n", first_mismatch_.c_str());
}

int Reporter::Finish(const std::string& verdict, const std::string& detail) {
  std::printf("RESULT %s %s %s\n", verdict.c_str(), options_.case_id.c_str(),
              detail.c_str());
  std::fflush(stdout);

  if (!options_.out_dir.empty()) {
    MakeDirs(options_.out_dir);
    std::ostringstream json;
    json << "{\n";
    json << "  \"schema_version\": 1,\n";
    json << "  \"case\": \"" << JsonEscape(options_.case_id) << "\",\n";
    json << "  \"verdict\": \"" << JsonEscape(verdict) << "\",\n";
    json << "  \"detail\": \"" << JsonEscape(detail) << "\",\n";
    json << "  \"seed\": " << options_.seed << ",\n";
    json << "  \"max_cycles\": " << options_.max_cycles << ",\n";
    json << "  \"checks\": " << checks_ << ",\n";
    json << "  \"failures\": " << failures_ << ",\n";
    json << "  \"image\": \"" << JsonEscape(options_.image) << "\",\n";
    json << "  \"expect\": \"" << JsonEscape(options_.expect) << "\",\n";
    json << "  \"tool\": \"" << JsonEscape(tool_version_) << "\",\n";
    json << "  \"timestamp\": \"" << Timestamp() << "\",\n";
    json << "  \"first_mismatch\": \"" << JsonEscape(first_mismatch_) << "\"";
    if (!messages_.empty()) {
      json << ",\n  \"failed_checks\": [";
      for (size_t i = 0; i < messages_.size(); ++i) {
        json << (i ? ", " : "") << "\"" << JsonEscape(messages_[i]) << "\"";
      }
      json << "]";
    }
    json << "\n}\n";
    std::ofstream out(options_.out_dir + "/result.json");
    out << json.str();
  }

  if (verdict == "PASS") return kExitPass;
  if (verdict == "BLOCKED") return kExitUsage;
  return kExitFail;
}

}  // namespace mosaic