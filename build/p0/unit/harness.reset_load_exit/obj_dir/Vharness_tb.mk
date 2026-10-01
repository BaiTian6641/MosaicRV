# Verilated -*- Makefile -*-
# DESCRIPTION: Verilator output: Makefile for building Verilated archive or executable
#
# Execute this makefile from the object directory:
#    make -f Vharness_tb.mk

default: /Users/flare/MosaicRV/build/p0/unit/harness.reset_load_exit/harness.reset_load_exit

### Constants...
# Perl executable (from $PERL, defaults to 'perl' if not set)
PERL = perl
# Python3 executable (from $PYTHON3, defaults to 'python3' if not set)
PYTHON3 = python3
# Path to Verilator kit (from $VERILATOR_ROOT)
VERILATOR_ROOT = /opt/homebrew/Cellar/verilator/5.052/share/verilator
# SystemC include directory with systemc.h (from $SYSTEMC_INCLUDE)
SYSTEMC_INCLUDE ?=
# SystemC library directory with libsystemc.a (from $SYSTEMC_LIBDIR)
SYSTEMC_LIBDIR ?=

### Switches...
# C++ code coverage  0/1 (from --prof-c)
VM_PROFC = 0
# SystemC output mode?  0/1 (from --sc)
VM_SC = 0
# Legacy or SystemC output mode?  0/1 (from --sc)
VM_SP_OR_SC = $(VM_SC)
# Deprecated
VM_PCLI = 1
# Deprecated: SystemC architecture to find link library path (from $SYSTEMC_ARCH)
VM_SC_TARGET_ARCH = linux

### Vars...
# Design prefix (from --prefix)
VM_PREFIX = Vharness_tb
# Module prefix (from --prefix)
VM_MODPREFIX = Vharness_tb
# User CFLAGS (from -CFLAGS on Verilator command line)
VM_USER_CFLAGS = \
  -O2 -std=c++17 -Wall \
  -I/Users/flare/MosaicRV/sim/common \
  -I/Users/flare/MosaicRV/build/p0/sim \

# User LDLIBS (from -LDFLAGS on Verilator command line)
VM_USER_LDLIBS = \

# User .cpp files (from .cpp's on Verilator command line)
VM_USER_CLASSES = \
  elf_loader \
  event_tap \
  memory_model \
  sim_common \
  tb_harness \

# User .cpp directories (from .cpp's on Verilator command line)
VM_USER_DIR = \
  ../../../../.. \
  ../../../../../sim/common \
  ../../../../../sim/harness \

### Default rules...
# Include list of all generated classes
include Vharness_tb_classes.mk
# Include global rules
include $(VERILATOR_ROOT)/include/verilated.mk

### Executable rules... (from --exe)
VPATH += $(VM_USER_DIR)

elf_loader.o: /Users/flare/MosaicRV/sim/common/elf_loader.cpp 
	$(OBJCACHE) $(CXX) $(CXXFLAGS) $(CPPFLAGS) $(OPT_FAST)  -c -o $@ $<
event_tap.o: /Users/flare/MosaicRV/sim/common/event_tap.cpp 
	$(OBJCACHE) $(CXX) $(CXXFLAGS) $(CPPFLAGS) $(OPT_FAST)  -c -o $@ $<
memory_model.o: /Users/flare/MosaicRV/sim/common/memory_model.cpp 
	$(OBJCACHE) $(CXX) $(CXXFLAGS) $(CPPFLAGS) $(OPT_FAST)  -c -o $@ $<
sim_common.o: /Users/flare/MosaicRV/sim/common/sim_common.cpp 
	$(OBJCACHE) $(CXX) $(CXXFLAGS) $(CPPFLAGS) $(OPT_FAST)  -c -o $@ $<
tb_harness.o: /Users/flare/MosaicRV/sim/harness/tb_harness.cpp 
	$(OBJCACHE) $(CXX) $(CXXFLAGS) $(CPPFLAGS) $(OPT_FAST)  -c -o $@ $<

### Link rules... (from --exe)
/Users/flare/MosaicRV/build/p0/unit/harness.reset_load_exit/harness.reset_load_exit: $(VK_USER_OBJS) $(VK_GLOBAL_OBJS) $(VM_PREFIX)__ALL.a
	$(LINK) $(LDFLAGS) $^ $(LOADLIBES) $(LDLIBS) $(LIBS) $(SC_LIBS) -o $@

# Verilated -*- Makefile -*-
