// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Symbol table internal header
//
// Internal details; most calling programs do not need this header,
// unless using verilator public meta comments.

#ifndef VERILATED_VMOSAIC_FIFO_TB__SYMS_H_
#define VERILATED_VMOSAIC_FIFO_TB__SYMS_H_  // guard

#include "verilated.h"

// INCLUDE MODEL CLASS

#include "Vmosaic_fifo_tb.h"

// INCLUDE MODULE CLASSES
#include "Vmosaic_fifo_tb___024root.h"

// SYMS CLASS (contains all model state)
class alignas(VL_CACHE_LINE_BYTES) Vmosaic_fifo_tb__Syms final : public VerilatedSyms {
  public:
    // INTERNAL STATE
    Vmosaic_fifo_tb* const __Vm_modelp;
    VlDeleter __Vm_deleter;
    bool& __Vm_didInit;

    // MODULE INSTANCE STATE
    Vmosaic_fifo_tb___024root      TOP;

    // CONSTRUCTORS
    Vmosaic_fifo_tb__Syms(VerilatedContext* contextp, const char* namep, Vmosaic_fifo_tb* modelp);
    ~Vmosaic_fifo_tb__Syms();

    // METHODS
    const char* name() const { return TOP.vlNamep; }
};

#endif  // guard
