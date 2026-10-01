// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vmosaic_ram_tb.h for the primary calling header

#include "Vmosaic_ram_tb__pch.h"

void Vmosaic_ram_tb___024root___ctor_var_reset(Vmosaic_ram_tb___024root* vlSelf);

Vmosaic_ram_tb___024root::Vmosaic_ram_tb___024root(Vmosaic_ram_tb__Syms* symsp, const char* namep)
 {
    vlSymsp = symsp;
    vlNamep = strdup(namep);
    // Reset structure values
    Vmosaic_ram_tb___024root___ctor_var_reset(this);
}

void Vmosaic_ram_tb___024root::__Vconfigure(bool first) {
    (void)first;  // Prevent unused variable warning
}

Vmosaic_ram_tb___024root::~Vmosaic_ram_tb___024root() {
    VL_DO_DANGLING(std::free(const_cast<char*>(vlNamep)), vlNamep);
}
