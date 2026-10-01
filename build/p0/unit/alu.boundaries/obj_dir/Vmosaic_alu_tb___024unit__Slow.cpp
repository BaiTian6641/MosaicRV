// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vmosaic_alu_tb.h for the primary calling header

#include "Vmosaic_alu_tb__pch.h"


Vmosaic_alu_tb___024unit::Vmosaic_alu_tb___024unit() = default;
Vmosaic_alu_tb___024unit::~Vmosaic_alu_tb___024unit() = default;

void Vmosaic_alu_tb___024unit::ctor(Vmosaic_alu_tb__Syms* symsp, const char* namep) {
    vlSymsp = symsp;
    vlNamep = strdup(Verilated::catName(vlSymsp->name(), namep));
    // Reset structure values
}

void Vmosaic_alu_tb___024unit::__Vconfigure(bool first) {
    (void)first;  // Prevent unused variable warning
}

void Vmosaic_alu_tb___024unit::dtor() {
    VL_DO_DANGLING(std::free(const_cast<char*>(vlNamep)), vlNamep);
}
