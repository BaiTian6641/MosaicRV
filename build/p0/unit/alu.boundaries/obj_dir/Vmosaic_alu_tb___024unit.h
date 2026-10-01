// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design internal header
// See Vmosaic_alu_tb.h for the primary calling header

#ifndef VERILATED_VMOSAIC_ALU_TB___024UNIT_H_
#define VERILATED_VMOSAIC_ALU_TB___024UNIT_H_  // guard

#include "verilated.h"


class Vmosaic_alu_tb__Syms;

class alignas(VL_CACHE_LINE_BYTES) Vmosaic_alu_tb___024unit final {
  public:

    // INTERNAL VARIABLES
    Vmosaic_alu_tb__Syms* vlSymsp;
    const char* vlNamep;

    // CONSTRUCTORS
    Vmosaic_alu_tb___024unit();
    ~Vmosaic_alu_tb___024unit();
    void ctor(Vmosaic_alu_tb__Syms* symsp, const char* namep);
    void dtor();
    VL_UNCOPYABLE(Vmosaic_alu_tb___024unit);

    // INTERNAL METHODS
    void __Vconfigure(bool first);
};


#endif  // guard
