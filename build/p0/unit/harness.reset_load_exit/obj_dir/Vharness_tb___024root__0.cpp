// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vharness_tb.h for the primary calling header

#include "Vharness_tb__pch.h"

void Vharness_tb___024root___eval_sample(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_sample\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vharness_tb___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag);
#endif  // VL_DEBUG
bool Vharness_tb___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in);
void Vharness_tb___024root___eval_body__ico(Vharness_tb___024root* vlSelf);

bool Vharness_tb___024root___eval_ico(Vharness_tb___024root* vlSelf, CData/*0:0*/ firstIteration) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_ico\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VicoExecute;
    // Body
    vlSelfRef.__VicoTriggered[1U] = ((0xfffffffffffffffeULL 
                                      & vlSelfRef.__VicoTriggered[1U]) 
                                     | (IData)((IData)(firstIteration)));
    {
        // Inlined CFunc: _eval_triggers_vec__ico
        vlSelfRef.__VicoTriggered[0U] = (QData)((IData)(
                                                        (((vlSelfRef.insn_in 
                                                           != vlSelfRef.__Vtrigprevexpr___TOP__insn_in__0) 
                                                          << 4U) 
                                                         | ((((vlSelfRef.pc_in 
                                                               != vlSelfRef.__Vtrigprevexpr___TOP__pc_in__0) 
                                                              << 3U) 
                                                             | (((IData)(vlSelfRef.step_valid) 
                                                                 != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__step_valid__0)) 
                                                                << 2U)) 
                                                            | ((((IData)(vlSelfRef.rst) 
                                                                 != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__rst__0)) 
                                                                << 1U) 
                                                               | ((IData)(vlSelfRef.clk) 
                                                                  != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__clk__0)))))));
        vlSelfRef.__Vtrigprevexpr___TOP__clk__0 = vlSelfRef.clk;
        vlSelfRef.__Vtrigprevexpr___TOP__rst__0 = vlSelfRef.rst;
        vlSelfRef.__Vtrigprevexpr___TOP__step_valid__0 
            = vlSelfRef.step_valid;
        vlSelfRef.__Vtrigprevexpr___TOP__pc_in__0 = vlSelfRef.pc_in;
        vlSelfRef.__Vtrigprevexpr___TOP__insn_in__0 
            = vlSelfRef.insn_in;
        if (VL_UNLIKELY(((1U & (~ (IData)(vlSelfRef.__VicoDidInit)))))) {
            vlSelfRef.__VicoDidInit = 1U;
            vlSelfRef.__VicoTriggered[0U] = (1ULL | vlSelfRef.__VicoTriggered[0U]);
            vlSelfRef.__VicoTriggered[0U] = (2ULL | vlSelfRef.__VicoTriggered[0U]);
            vlSelfRef.__VicoTriggered[0U] = (4ULL | vlSelfRef.__VicoTriggered[0U]);
            vlSelfRef.__VicoTriggered[0U] = (8ULL | vlSelfRef.__VicoTriggered[0U]);
            vlSelfRef.__VicoTriggered[0U] = (0x0000000000000010ULL 
                                             | vlSelfRef.__VicoTriggered[0U]);
        }
    }
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Vharness_tb___024root___dump_triggers__ico(vlSelfRef.__VicoTriggered, "ico"s);
    }
#endif
    __VicoExecute = Vharness_tb___024root___trigger_anySet__ico(vlSelfRef.__VicoTriggered);
    if (__VicoExecute) {
        Vharness_tb___024root___eval_body__ico(vlSelf);
    }
    return (__VicoExecute);
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vharness_tb___024root___dump_triggers__act(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag);
#endif  // VL_DEBUG
void Vharness_tb___024root___trigger_orInto__act_vec_vec(VlUnpacked<QData/*63:0*/, 1> &out, const VlUnpacked<QData/*63:0*/, 1> &in);

bool Vharness_tb___024root___eval_act(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_act\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    {
        // Inlined CFunc: _eval_triggers_vec__act
        vlSelfRef.__VactTriggered[0U] = (QData)((IData)(
                                                        ((IData)(vlSelfRef.clk) 
                                                         & (~ (IData)(vlSelfRef.__Vtrigprevexpr___TOP__clk__1)))));
        vlSelfRef.__Vtrigprevexpr___TOP__clk__1 = vlSelfRef.clk;
    }
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Vharness_tb___024root___dump_triggers__act(vlSelfRef.__VactTriggered, "act"s);
    }
#endif
    Vharness_tb___024root___trigger_orInto__act_vec_vec(vlSelfRef.__VnbaTriggered, vlSelfRef.__VactTriggered);
    return (0U);
}

bool Vharness_tb___024root___eval_inact(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_inact\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    return (0U);
}

bool Vharness_tb___024root___trigger_anySet__act(const VlUnpacked<QData/*63:0*/, 1> &in);
void Vharness_tb___024root___nba_sequent__TOP__0(Vharness_tb___024root* vlSelf);
void Vharness_tb___024root___trigger_clear__act(VlUnpacked<QData/*63:0*/, 1> &out);

bool Vharness_tb___024root___eval_nba(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_nba\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VnbaExecute;
    // Body
    __VnbaExecute = Vharness_tb___024root___trigger_anySet__act(vlSelfRef.__VnbaTriggered);
    if (__VnbaExecute) {
        {
            // Inlined CFunc: _eval_body__nba
            if ((1ULL & vlSelfRef.__VnbaTriggered[0U])) {
                Vharness_tb___024root___nba_sequent__TOP__0(vlSelf);
            }
        }
        Vharness_tb___024root___trigger_clear__act(vlSelfRef.__VnbaTriggered);
    }
    return (__VnbaExecute);
}

bool Vharness_tb___024root___eval_obs(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_obs\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    return (0U);
}

bool Vharness_tb___024root___eval_react(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_react\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    return (0U);
}

void Vharness_tb___024root___eval_postponed(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_postponed\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

bool Vharness_tb___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___trigger_anySet__ico\n"); );
    // Locals
    IData/*31:0*/ n;
    // Body
    n = 0U;
    do {
        if (in[n]) {
            return (1U);
        }
        n = ((IData)(1U) + n);
    } while ((2U > n));
    return (0U);
}

void Vharness_tb___024root___eval_body__ico(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_body__ico\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    if ((4ULL & vlSelfRef.__VicoTriggered[0U])) {
        {
            // Inlined CFunc: _ico_sequent__TOP__0
            vlSelfRef.retire = vlSelfRef.step_valid;
        }
    }
    if ((0x0000000000000010ULL & vlSelfRef.__VicoTriggered[0U])) {
        {
            // Inlined CFunc: _ico_sequent__TOP__1
            CData/*0:0*/ __Vinline_0__ico_sequent__TOP__1___VdfgRegularize_hebeb780c_0_5;
            __Vinline_0__ico_sequent__TOP__1___VdfgRegularize_hebeb780c_0_5 = 0;
            vlSelfRef.illegal = (1U & ((0x00000040U 
                                        & vlSelfRef.insn_in)
                                        ? ((~ (IData)(
                                                      (0x0000002fU 
                                                       == 
                                                       (0x0000002fU 
                                                        & vlSelfRef.insn_in)))) 
                                           | (vlSelfRef.insn_in 
                                              >> 4U))
                                        : ((0x00000020U 
                                            & vlSelfRef.insn_in)
                                            ? ((0x00000010U 
                                                & vlSelfRef.insn_in)
                                                ? (
                                                   (~ 
                                                    (7U 
                                                     == 
                                                     (7U 
                                                      & vlSelfRef.insn_in))) 
                                                   | (vlSelfRef.insn_in 
                                                      >> 3U))
                                                : (IData)(
                                                          (0x00003003U 
                                                           != 
                                                           (0x0000700fU 
                                                            & vlSelfRef.insn_in))))
                                            : (IData)(
                                                      (0x00000013U 
                                                       != 
                                                       (0x0000701fU 
                                                        & vlSelfRef.insn_in))))));
            __Vinline_0__ico_sequent__TOP__1___VdfgRegularize_hebeb780c_0_5 
                = (IData)((7U == (7U & vlSelfRef.insn_in)));
            vlSelfRef.harness_tb__DOT__u_probe__DOT__does_store 
                = (IData)((0x00003023U == (0x0000707fU 
                                           & vlSelfRef.insn_in)));
            vlSelfRef.rd_index = (0x0000001fU & (vlSelfRef.insn_in 
                                                 >> 7U));
            vlSelfRef.harness_tb__DOT__u_probe__DOT__write_rd 
                = (((0x00000040U & vlSelfRef.insn_in)
                     ? (IData)(((0x00000028U == (0x00000038U 
                                                 & vlSelfRef.insn_in)) 
                                & __Vinline_0__ico_sequent__TOP__1___VdfgRegularize_hebeb780c_0_5))
                     : ((0x00000020U & vlSelfRef.insn_in)
                         ? (IData)(((0x00000010U == 
                                     (0x00000018U & vlSelfRef.insn_in)) 
                                    & __Vinline_0__ico_sequent__TOP__1___VdfgRegularize_hebeb780c_0_5))
                         : (IData)((0x00000013U == 
                                    (0x0000701fU & vlSelfRef.insn_in))))) 
                   & (0U != (IData)(vlSelfRef.rd_index)));
            vlSelfRef.mem_addr = (vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs
                                  [(0x0000001fU & (vlSelfRef.insn_in 
                                                   >> 0x0000000fU))] 
                                  + (((- (QData)((IData)(
                                                         (vlSelfRef.insn_in 
                                                          >> 0x0000001fU)))) 
                                      << 0x0000000cU) 
                                     | (QData)((IData)(
                                                       ((0x00000fe0U 
                                                         & (vlSelfRef.insn_in 
                                                            >> 0x00000014U)) 
                                                        | (0x0000001fU 
                                                           & ((0x23U 
                                                               == 
                                                               (0x0000007fU 
                                                                & vlSelfRef.insn_in))
                                                               ? (IData)(vlSelfRef.rd_index)
                                                               : 
                                                              (vlSelfRef.insn_in 
                                                               >> 0x00000014U))))))));
        }
    }
    if ((8ULL & vlSelfRef.__VicoTriggered[0U])) {
        {
            // Inlined CFunc: _ico_sequent__TOP__2
            vlSelfRef.next_pc = (4ULL + vlSelfRef.pc_in);
        }
    }
    if ((0x0000000000000014ULL & vlSelfRef.__VicoTriggered[0U])) {
        {
            // Inlined CFunc: _ico_comb__TOP__0
            vlSelfRef.mem_en = ((IData)(vlSelfRef.step_valid) 
                                & (IData)(vlSelfRef.harness_tb__DOT__u_probe__DOT__does_store));
            vlSelfRef.rd_we = ((IData)(vlSelfRef.step_valid) 
                               & ((~ (IData)(vlSelfRef.illegal)) 
                                  & (IData)(vlSelfRef.harness_tb__DOT__u_probe__DOT__write_rd)));
        }
    }
    if ((0x0000000000000018ULL & vlSelfRef.__VicoTriggered[0U])) {
        {
            // Inlined CFunc: _ico_comb__TOP__1
            vlSelfRef.rd_value_out = ((0x00000040U 
                                       & vlSelfRef.insn_in)
                                       ? (vlSelfRef.next_pc 
                                          & (- (QData)((IData)(
                                                               (0x0000002fU 
                                                                == 
                                                                (0x0000003fU 
                                                                 & vlSelfRef.insn_in))))))
                                       : ((0x00000020U 
                                           & vlSelfRef.insn_in)
                                           ? ((- (QData)((IData)(
                                                                 (0x00000017U 
                                                                  == 
                                                                  (0x0000001fU 
                                                                   & vlSelfRef.insn_in))))) 
                                              & (QData)((IData)(
                                                                (0xfffff000U 
                                                                 & vlSelfRef.insn_in))))
                                           : (vlSelfRef.mem_addr 
                                              & (- (QData)((IData)(
                                                                   (0x00000013U 
                                                                    == 
                                                                    (0x0000701fU 
                                                                     & vlSelfRef.insn_in))))))));
            vlSelfRef.mem_wdata = ((IData)(vlSelfRef.harness_tb__DOT__u_probe__DOT__does_store)
                                    ? vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs
                                   [(0x0000001fU & 
                                     (vlSelfRef.insn_in 
                                      >> 0x00000014U))]
                                    : (vlSelfRef.rd_value_out 
                                       & (- (QData)((IData)(
                                                            (0U 
                                                             != (IData)(vlSelfRef.rd_index)))))));
        }
    }
}

bool Vharness_tb___024root___trigger_anySet__act(const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___trigger_anySet__act\n"); );
    // Locals
    IData/*31:0*/ n;
    // Body
    n = 0U;
    do {
        if (in[n]) {
            return (1U);
        }
        n = ((IData)(1U) + n);
    } while ((1U > n));
    return (0U);
}

void Vharness_tb___024root___nba_sequent__TOP__0(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___nba_sequent__TOP__0\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VdlySet__harness_tb__DOT__u_probe__DOT__xregs__v0;
    __VdlySet__harness_tb__DOT__u_probe__DOT__xregs__v0 = 0;
    QData/*63:0*/ __VdlyVal__harness_tb__DOT__u_probe__DOT__xregs__v32;
    __VdlyVal__harness_tb__DOT__u_probe__DOT__xregs__v32 = 0;
    CData/*4:0*/ __VdlyDim0__harness_tb__DOT__u_probe__DOT__xregs__v32;
    __VdlyDim0__harness_tb__DOT__u_probe__DOT__xregs__v32 = 0;
    CData/*0:0*/ __VdlySet__harness_tb__DOT__u_probe__DOT__xregs__v32;
    __VdlySet__harness_tb__DOT__u_probe__DOT__xregs__v32 = 0;
    // Body
    __VdlySet__harness_tb__DOT__u_probe__DOT__xregs__v0 = 0U;
    __VdlySet__harness_tb__DOT__u_probe__DOT__xregs__v32 = 0U;
    if (vlSelfRef.rst) {
        __VdlySet__harness_tb__DOT__u_probe__DOT__xregs__v0 = 1U;
    } else if (((IData)(vlSelfRef.step_valid) & (IData)(vlSelfRef.harness_tb__DOT__u_probe__DOT__write_rd))) {
        __VdlyVal__harness_tb__DOT__u_probe__DOT__xregs__v32 
            = vlSelfRef.rd_value_out;
        __VdlyDim0__harness_tb__DOT__u_probe__DOT__xregs__v32 
            = vlSelfRef.rd_index;
        __VdlySet__harness_tb__DOT__u_probe__DOT__xregs__v32 = 1U;
    }
    if (__VdlySet__harness_tb__DOT__u_probe__DOT__xregs__v0) {
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[0U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[1U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[2U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[3U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[4U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[5U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[6U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[7U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[8U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[9U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[10U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[11U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[12U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[13U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[14U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[15U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[16U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[17U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[18U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[19U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[20U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[21U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[22U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[23U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[24U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[25U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[26U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[27U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[28U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[29U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[30U] = 0ULL;
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[31U] = 0ULL;
    }
    if (__VdlySet__harness_tb__DOT__u_probe__DOT__xregs__v32) {
        vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs[__VdlyDim0__harness_tb__DOT__u_probe__DOT__xregs__v32] 
            = __VdlyVal__harness_tb__DOT__u_probe__DOT__xregs__v32;
    }
    vlSelfRef.mem_addr = (vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs
                          [(0x0000001fU & (vlSelfRef.insn_in 
                                           >> 0x0000000fU))] 
                          + (((- (QData)((IData)((vlSelfRef.insn_in 
                                                  >> 0x0000001fU)))) 
                              << 0x0000000cU) | (QData)((IData)(
                                                                ((0x00000fe0U 
                                                                  & (vlSelfRef.insn_in 
                                                                     >> 0x00000014U)) 
                                                                 | (0x0000001fU 
                                                                    & ((0x23U 
                                                                        == 
                                                                        (0x0000007fU 
                                                                         & vlSelfRef.insn_in))
                                                                        ? (IData)(vlSelfRef.rd_index)
                                                                        : 
                                                                       (vlSelfRef.insn_in 
                                                                        >> 0x00000014U))))))));
    vlSelfRef.rd_value_out = ((0x00000040U & vlSelfRef.insn_in)
                               ? (vlSelfRef.next_pc 
                                  & (- (QData)((IData)(
                                                       (0x0000002fU 
                                                        == 
                                                        (0x0000003fU 
                                                         & vlSelfRef.insn_in))))))
                               : ((0x00000020U & vlSelfRef.insn_in)
                                   ? ((- (QData)((IData)(
                                                         (0x00000017U 
                                                          == 
                                                          (0x0000001fU 
                                                           & vlSelfRef.insn_in))))) 
                                      & (QData)((IData)(
                                                        (0xfffff000U 
                                                         & vlSelfRef.insn_in))))
                                   : (vlSelfRef.mem_addr 
                                      & (- (QData)((IData)(
                                                           (0x00000013U 
                                                            == 
                                                            (0x0000701fU 
                                                             & vlSelfRef.insn_in))))))));
    vlSelfRef.mem_wdata = ((IData)(vlSelfRef.harness_tb__DOT__u_probe__DOT__does_store)
                            ? vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs
                           [(0x0000001fU & (vlSelfRef.insn_in 
                                            >> 0x00000014U))]
                            : (vlSelfRef.rd_value_out 
                               & (- (QData)((IData)(
                                                    (0U 
                                                     != (IData)(vlSelfRef.rd_index)))))));
}

void Vharness_tb___024root___trigger_orInto__act_vec_vec(VlUnpacked<QData/*63:0*/, 1> &out, const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___trigger_orInto__act_vec_vec\n"); );
    // Locals
    IData/*31:0*/ n;
    // Body
    n = 0U;
    do {
        out[n] = (out[n] | in[n]);
        n = ((IData)(1U) + n);
    } while ((0U >= n));
}

void Vharness_tb___024root___trigger_clear__act(VlUnpacked<QData/*63:0*/, 1> &out) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___trigger_clear__act\n"); );
    // Locals
    IData/*31:0*/ n;
    // Body
    n = 0U;
    do {
        out[n] = 0ULL;
        n = ((IData)(1U) + n);
    } while ((1U > n));
}

#ifdef VL_DEBUG
void Vharness_tb___024root___eval_debug_assertions(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_debug_assertions\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    if (VL_UNLIKELY(((vlSelfRef.clk & 0xfeU)))) {
        Verilated::overWidthError("clk");
    }
    if (VL_UNLIKELY(((vlSelfRef.rst & 0xfeU)))) {
        Verilated::overWidthError("rst");
    }
    if (VL_UNLIKELY(((vlSelfRef.step_valid & 0xfeU)))) {
        Verilated::overWidthError("step_valid");
    }
}
#endif  // VL_DEBUG
