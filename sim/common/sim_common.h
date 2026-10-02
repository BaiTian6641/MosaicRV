// Shared support for every MosaicRV Verilator testbench.
//
// One convention for all testbenches, so a failing case looks the same whether
// it came from a unit test, a directed suite or a random campaign:
//
//   * the clock is driven explicitly, one rising edge at a time, so "cycle N"
//     means the same thing in every testbench;
//   * `rst` is asserted for a fixed, documented number of cycles before the DUT
//     is considered alive -- a DUT that needs a different reset length must say
//     so in its own testbench, not by trial and error;
//   * a testbench prints exactly one `RESULT <verdict> <case-id> <detail>` line
//     and exits 0 for PASS, 1 for FAIL, 2 for a usage/setup error. Anything else
//     is a broken harness and is treated as a failure by the runner;
//   * the result JSON records the exact command, the seed, the tool versions and
//     the first mismatch, so a failure can be replayed without guessing.
//
// No testbench may end by printing "OK" and returning 0 without a check behind
// it; the runner cross-checks the RESULT line against the exit status, so a
// silent pass is not possible.

#ifndef MOSAIC_SIM_COMMON_H_
#define MOSAIC_SIM_COMMON_H_

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// The reset-traffic rule (V-010), shared by every driver that models a bus.
// Included here so a new driver inherits it by including the common header it
// already includes, rather than by remembering to add a second include.
#include "bus_reset_gate.h"

namespace mosaic {

// Exit codes. Kept small and stable: tools/run_tests.py depends on them.
enum ExitCode {
  kExitPass = 0,
  kExitFail = 1,
  kExitUsage = 2,
};

// A 64-bit xorshift* generator. Every testbench takes its randomness from one of
// these seeded from --seed, so a failing seed reproduces exactly. std::mt19937 is
// deliberately avoided: its standardisation is portable but its distributions are
// not, and reproducibility across toolchains matters more here.
class Rng {
 public:
  explicit Rng(uint64_t seed) : state_(seed ? seed : 0x9e3779b97f4a7c15ull) {}

  uint64_t Next() {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 0x2545f4914f6cdd1dull;
  }

  // Uniform in [0, bound). Rejection sampled so the distribution does not depend
  // on the modulus, which would make a seed reproduce differently on a machine
  // with a different word size.
  uint32_t Below(uint32_t bound) {
    if (bound == 0) return 0;
    const uint64_t limit = UINT64_MAX - (UINT64_MAX % bound);
    uint64_t value;
    do {
      value = Next();
    } while (value >= limit);
    return static_cast<uint32_t>(value % bound);
  }

  bool Chance(uint32_t percent) { return Below(100) < percent; }

 private:
  uint64_t state_;
};

// Command-line options every testbench accepts. Parsing lives here so no
// testbench invents its own flag spelling.
struct Options {
  std::string case_id;
  std::string out_dir;
  uint64_t seed = 0;
  uint64_t max_cycles = 100000;
  bool verbose = false;
  std::string image;   // ELF or binary image, when the testbench loads one
  std::string expect;  // reference signature, when the testbench compares one

  // Returns false and fills `error` on a bad command line.
  static bool Parse(int argc, char** argv, Options* out, std::string* error);
};

// Drives clk/rst against any Verilated top. `eval` is called once per phase so a
// DUT never sees a half-updated clock, which is the classic source of
// off-by-one-cycle testbench bugs.
class ClockDriver {
 public:
  ClockDriver() : cycle_(0), reset_cycles_(0), in_reset_(true) {}

  // Hold reset for `cycles` rising edges, then release it.
  void BeginReset(int cycles);

  // One full clock period: low phase, rising edge, high phase, falling edge.
  // Returns the cycle number that just completed.
  uint64_t Tick();

  // One rising edge only, for testbenches that need to inspect between phases.
  uint64_t RisingEdge();
  uint64_t FallingEdge();

  uint64_t cycle() const { return cycle_; }
  bool in_reset() const { return in_reset_; }
  int reset_cycles() const { return reset_cycles_; }

 private:
  uint64_t cycle_;
  int reset_cycles_;
  bool in_reset_;
};

// Accumulates the single result line and the JSON run manifest.
class Reporter {
 public:
  Reporter(const Options& options, const std::string& tool_version);

  // Record a check. `passed` false makes the whole run fail.
  void Check(bool passed, const std::string& what);

  // Record the first (and most useful) mismatch in human-readable form.
  void Mismatch(const std::string& where, const std::string& expected,
                const std::string& actual);

  // Finish: write the manifest, print the RESULT line, return the exit code.
  int Finish(const std::string& verdict, const std::string& detail);

  int checks() const { return checks_; }
  int failures() const { return failures_; }

 private:
  Options options_;
  std::string tool_version_;
  std::vector<std::string> messages_;
  std::string first_mismatch_;
  int checks_;
  int failures_;
};

// Utility: format a 64-bit value as 0x... for mismatch messages.
std::string Hex(uint64_t value, int digits = 16);
std::string HexSigned(int64_t value);

}  // namespace mosaic

#endif  // MOSAIC_SIM_COMMON_H_