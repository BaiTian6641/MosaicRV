// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design internal header
// See Vmosaic_alu_tb.h for the primary calling header

#ifndef VERILATED_VMOSAIC_ALU_TB___024ROOT_H_
#define VERILATED_VMOSAIC_ALU_TB___024ROOT_H_  // guard

#include "verilated.h"


class Vmosaic_alu_tb__Syms;

class alignas(VL_CACHE_LINE_BYTES) Vmosaic_alu_tb___024root final {
  public:

    // DESIGN SPECIFIC STATE
    VL_IN8(op,3,0);
    VL_OUT8(zero,0,0);
    CData/*3:0*/ __Vtrigprevexpr___TOP__op__0;
    CData/*0:0*/ __VicoDidInit;
    IData/*31:0*/ __VdfgRegularize_hebeb780c_0_0;
    IData/*31:0*/ __VdfgRegularize_hebeb780c_0_1;
    IData/*31:0*/ __VdfgRegularize_hebeb780c_0_2;
    IData/*31:0*/ __VdfgRegularize_hebeb780c_0_3;
    IData/*31:0*/ __VdfgRegularize_hebeb780c_0_4;
    VL_IN64(a,63,0);
    VL_IN64(b,63,0);
    VL_OUT64(result,63,0);
    QData/*63:0*/ __Vtrigprevexpr___TOP__a__0;
    QData/*63:0*/ __Vtrigprevexpr___TOP__b__0;
    VlUnpacked<QData/*63:0*/, 1> __VstlTriggered;
    VlUnpacked<QData/*63:0*/, 2> __VicoTriggered;

    // INTERNAL VARIABLES
    Vmosaic_alu_tb__Syms* vlSymsp;
    const char* vlNamep;

    // CONSTRUCTORS
    Vmosaic_alu_tb___024root(Vmosaic_alu_tb__Syms* symsp, const char* namep);
    ~Vmosaic_alu_tb___024root();
    VL_UNCOPYABLE(Vmosaic_alu_tb___024root);

    // INTERNAL METHODS
    void __Vconfigure(bool first);
};


#endif  // guard
