// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vharness_tb.h for the primary calling header

#include "Vharness_tb__pch.h"

void Vharness_tb___024root___ctor_var_reset(Vharness_tb___024root* vlSelf);

Vharness_tb___024root::Vharness_tb___024root(Vharness_tb__Syms* symsp, const char* namep)
 {
    vlSymsp = symsp;
    vlNamep = strdup(namep);
    // Reset structure values
    Vharness_tb___024root___ctor_var_reset(this);
}

void Vharness_tb___024root::__Vconfigure(bool first) {
    (void)first;  // Prevent unused variable warning
}

Vharness_tb___024root::~Vharness_tb___024root() {
    VL_DO_DANGLING(std::free(const_cast<char*>(vlNamep)), vlNamep);
}
