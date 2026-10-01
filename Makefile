# MosaicRV build and verification entry points.
#
# Contract (docs/implementation-plan.md I-003, section 2):
#   * a command that succeeds exits 0; a mismatch, assertion, timeout or an
#     unimplemented configuration exits non-zero;
#   * an unknown profile, a missing tool or an unwritable output directory is a
#     hard failure -- never a silent fallback to a different configuration;
#   * every build writes build/<profile>/manifest.json, and a configuration that
#     does not check out writes no manifest at all;
#   * profiles build into separate directories and never share objects.
#
# GNU Make 3.81 compatible: no `.ONESHELL`, no `!=` shell assignment.

PROFILE ?= p0
VALID_PROFILES := p0 p1 p2 p3

BUILD_DIR := build/$(PROFILE)
RESULTS_DIR := results/$(PROFILE)
PYTHON ?= python3
JOBS ?= 0

RTL_SRCS := $(shell find rtl -name '*.sv' -not -path 'build/*' 2>/dev/null | sort)
TB_SRCS := $(shell find sim -name '*_tb.sv' 2>/dev/null | sort)

# Every Verilator and Yosys invocation gets the generated config package first.
GEN_CFG := $(BUILD_DIR)/rtl/mosaic_cfg_pkg.svh
GEN_ID := $(BUILD_DIR)/rtl/mosaic_id_pkg.svh
GEN_DEPS := $(GEN_CFG) $(GEN_ID)

VERILATOR ?= verilator
CXX ?= c++
# Verilator's own headers must be on the include path when linting the
# testbenches, because they include the generated model headers.
VERILATOR_ROOT := $(shell verilator -getenv VERILATOR_ROOT 2>/dev/null)
YOSYS ?= yosys
SLANG_TIDY ?= slang-tidy

# -Wall is not advisory in this project: a warning is a defect that a later
# toolchain will turn into a silently different netlist.
VERILATOR_LINT_FLAGS := --lint-only -Wall -Wno-DECLFILENAME

.PHONY: all help check check-config check-contracts manifest lint lint-slang lint-cpp \
        unit sim synth-generic test clean distclean verify-tools

all: check manifest lint unit

help:
	@echo "MosaicRV targets (PROFILE=$(PROFILE)):"
	@echo "  make check             config + contracts + lint + unit"
	@echo "  make check-config      validate profiles, PMA, CSRs, geometry"
	@echo "  make check-contracts   validate interface contracts and tag arithmetic"
	@echo "  make manifest          generate build/$(PROFILE)/manifest.json and RTL config"
	@echo "  make lint              Verilator lint of the whole design"
	@echo "  make lint-slang        slang AST/tidy pass over the RTL"
	@echo "  make unit [CASE=...]   run registered unit testbench cases"
	@echo "  make unit-all          run every registered unit case"
	@echo "  make sim               run the directed simulation suites"
	@echo "  make synth-generic     Yosys generic synthesis smoke test"
	@echo "  make test              everything a profile must pass before a gate"
	@echo "  make clean             remove build/ and results/ for this profile"

verify-tools:
	@missing=""; \
	for tool in $(VERILATOR) $(PYTHON); do \
	  command -v $$tool >/dev/null 2>&1 || missing="$$missing $$tool"; \
	done; \
	if [ -n "$$missing" ]; then \
	  echo "ERROR missing required tool(s):$$missing" >&2; exit 2; \
	fi; \
	$(VERILATOR) --version

# ---------------------------------------------------------------- guard rails

check-valid-profile:
	@case " $(VALID_PROFILES) " in \
	  *" $(PROFILE) "*) ;; \
	  *) echo "ERROR unknown PROFILE='$(PROFILE)'; valid profiles: $(VALID_PROFILES)" >&2; \
	     echo "       refusing to build; there is no fallback configuration." >&2; \
	     exit 2 ;; \
	esac

# ------------------------------------------------------------------ contracts

check-config: check-valid-profile
	$(PYTHON) tools/check_profile.py --all

check-contracts: check-valid-profile
	$(PYTHON) tools/check_contracts.py --all

# ------------------------------------------------------------- config -> RTL

$(GEN_CFG) $(GEN_ID): config/profiles/$(PROFILE).json config/capability_ladder.json \
                     config/contracts/interfaces.json config/contracts/counters.json \
                     tools/gen_manifest.py tools/mosaic/config_check.py
	@$(PYTHON) tools/check_profile.py --profile $(PROFILE)
	@$(PYTHON) tools/check_contracts.py --profile $(PROFILE)
	@$(PYTHON) tools/gen_manifest.py --profile $(PROFILE)

manifest: check-valid-profile $(GEN_CFG) $(GEN_ID)
	@echo "manifest: $(BUILD_DIR)/manifest.json"

# ---------------------------------------------------------------------- lint

lint: check-valid-profile manifest
	@if [ -z "$(RTL_SRCS)" ]; then \
	  echo "lint: no RTL sources yet; nothing to check" >&2; exit 1; \
	fi
	$(VERILATOR) $(VERILATOR_LINT_FLAGS) --top-module mosaic_top \
	  -I$(BUILD_DIR)/rtl -Irtl/common -Irtl/core $(RTL_SRCS)

# slang parses and type-checks the SystemVerilog AST directly, which catches
# elaboration-time type errors Verilator's lint mode reports differently. It is a
# second opinion on the same source, not a second set of rules.
lint-slang: check-valid-profile manifest
	@if [ -z "$(RTL_SRCS)" ]; then \
	  echo "lint-slang: no RTL sources yet" >&2; exit 1; \
	fi
	$(SLANG_TIDY) --std 1800-2017 -I $(BUILD_DIR)/rtl $(RTL_SRCS)

# Our own C++ is held to a stricter standard than the simulator's runtime, which
# is compiled with the same CFLAGS and is not -Wextra clean.
CPP_SRCS := $(filter %.cpp,$(shell find sim -name '*.cpp' 2>/dev/null | sort))

lint-cpp: manifest
	@if [ -z "$(CPP_SRCS)" ]; then \
	  echo "lint-cpp: no C++ sources yet" >&2; exit 1; \
	fi
	@incs="-I$(BUILD_DIR)/sim -Isim/common -I$(VERILATOR_ROOT)/include"; \
	for d in build/$(PROFILE)/unit/*/obj_dir; do \
	  [ -d "$$d" ] && incs="$$incs -I$$d"; \
	done; \
	for f in $(CPP_SRCS); do \
	  $(CXX) -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow $$incs "$$f" || exit 1; \
	done
	@echo "lint-cpp: $(words $(CPP_SRCS)) file(s) clean"

# --------------------------------------------------------------------- tests

unit: check-valid-profile manifest
	@case "$(CASE)" in \
	  "") $(PYTHON) tools/run_unit.py --profile $(PROFILE) --all ;; \
	  *)  $(PYTHON) tools/run_unit.py --profile $(PROFILE) --case $(CASE) ;; \
	esac

unit-all: unit

sim: check-valid-profile manifest
	$(PYTHON) tools/verify.py --profile $(PROFILE) --all

synth-generic: check-valid-profile manifest
	$(PYTHON) tools/synth_check.py --profile $(PROFILE)

test: check check-contracts lint lint-slang lint-cpp unit sim synth-generic
	@echo "profile $(PROFILE): all configured checks passed"

check: check-config check-contracts

# ---------------------------------------------------------------------- clean

clean:
	rm -rf $(BUILD_DIR) $(RESULTS_DIR)

distclean:
	rm -rf build results