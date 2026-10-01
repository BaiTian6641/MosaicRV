// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vharness_tb.h for the primary calling header

#include "Vharness_tb__pch.h"

VL_ATTR_COLD void Vharness_tb___024root___eval_static(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_static\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    vlSelfRef.__Vtrigprevexpr___TOP__clk__0 = vlSelfRef.clk;
    vlSelfRef.__Vtrigprevexpr___TOP__rst__0 = vlSelfRef.rst;
    vlSelfRef.__Vtrigprevexpr___TOP__step_valid__0 
        = vlSelfRef.step_valid;
    vlSelfRef.__Vtrigprevexpr___TOP__pc_in__0 = vlSelfRef.pc_in;
    vlSelfRef.__Vtrigprevexpr___TOP__insn_in__0 = vlSelfRef.insn_in;
    vlSelfRef.__Vtrigprevexpr___TOP__clk__1 = vlSelfRef.clk;
}

VL_ATTR_COLD void Vharness_tb___024root___eval_initial(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_initial\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    {
        // Inlined CFunc: _eval_initial__TOP
        vlSelfRef.step_ready = 1U;
        vlSelfRef.mem_byte_en = 0xffU;
    }
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vharness_tb___024root___dump_triggers__stl(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag);
#endif  // VL_DEBUG
VL_ATTR_COLD bool Vharness_tb___024root___trigger_anySet__stl(const VlUnpacked<QData/*63:0*/, 1> &in);
VL_ATTR_COLD void Vharness_tb___024root___stl_sequent__TOP__0(Vharness_tb___024root* vlSelf);

VL_ATTR_COLD bool Vharness_tb___024root___eval_stl(Vharness_tb___024root* vlSelf, CData/*0:0*/ firstIteration) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_stl\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VstlExecute;
    // Body
    vlSelfRef.__VstlTriggered[0U] = ((0xfffffffffffffffeULL 
                                      & vlSelfRef.__VstlTriggered[0U]) 
                                     | (IData)((IData)(firstIteration)));
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Vharness_tb___024root___dump_triggers__stl(vlSelfRef.__VstlTriggered, "stl"s);
    }
#endif
    __VstlExecute = Vharness_tb___024root___trigger_anySet__stl(vlSelfRef.__VstlTriggered);
    if (__VstlExecute) {
        {
            // Inlined CFunc: _eval_body__stl
            if ((1ULL & vlSelfRef.__VstlTriggered[0U])) {
                Vharness_tb___024root___stl_sequent__TOP__0(vlSelf);
            }
        }
    }
    return (__VstlExecute);
}

VL_ATTR_COLD void Vharness_tb___024root___eval_dump_triggers__stl(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_dump_triggers__stl\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
#ifdef VL_DEBUG
    Vharness_tb___024root___dump_triggers__stl(vlSelfRef.__VstlTriggered, "stl"s);
#endif
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vharness_tb___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag);
#endif  // VL_DEBUG

VL_ATTR_COLD void Vharness_tb___024root___eval_dump_triggers__ico(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_dump_triggers__ico\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
#ifdef VL_DEBUG
    Vharness_tb___024root___dump_triggers__ico(vlSelfRef.__VicoTriggered, "ico"s);
#endif
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vharness_tb___024root___dump_triggers__act(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag);
#endif  // VL_DEBUG

VL_ATTR_COLD void Vharness_tb___024root___eval_dump_triggers__act(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_dump_triggers__act\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
#ifdef VL_DEBUG
    Vharness_tb___024root___dump_triggers__act(vlSelfRef.__VactTriggered, "act"s);
#endif
}

VL_ATTR_COLD void Vharness_tb___024root___eval_dump_triggers__nba(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_dump_triggers__nba\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
#ifdef VL_DEBUG
    Vharness_tb___024root___dump_triggers__act(vlSelfRef.__VnbaTriggered, "nba"s);
#endif
}

VL_ATTR_COLD void Vharness_tb___024root___eval_dump_triggers__obs(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_dump_triggers__obs\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

VL_ATTR_COLD void Vharness_tb___024root___eval_dump_triggers__react(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_dump_triggers__react\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

VL_ATTR_COLD void Vharness_tb___024root___eval_final(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___eval_final\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vharness_tb___024root___dump_triggers__stl(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___dump_triggers__stl\n"); );
    // Body
    if ((1U & (~ (IData)(Vharness_tb___024root___trigger_anySet__stl(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: Internal 'stl' trigger - first iteration\n");
    }
}
#endif  // VL_DEBUG

VL_ATTR_COLD bool Vharness_tb___024root___trigger_anySet__stl(const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___trigger_anySet__stl\n"); );
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

VL_ATTR_COLD void Vharness_tb___024root___stl_sequent__TOP__0(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___stl_sequent__TOP__0\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VdfgRegularize_hebeb780c_0_5;
    __VdfgRegularize_hebeb780c_0_5 = 0;
    // Body
    vlSelfRef.retire = vlSelfRef.step_valid;
    __VdfgRegularize_hebeb780c_0_5 = (IData)((7U == 
                                              (7U & vlSelfRef.insn_in)));
    vlSelfRef.harness_tb__DOT__u_probe__DOT__does_store 
        = (IData)((0x00003023U == (0x0000707fU & vlSelfRef.insn_in)));
    vlSelfRef.next_pc = (4ULL + vlSelfRef.pc_in);
    vlSelfRef.rd_index = (0x0000001fU & (vlSelfRef.insn_in 
                                         >> 7U));
    vlSelfRef.mem_en = ((IData)(vlSelfRef.step_valid) 
                        & (IData)(vlSelfRef.harness_tb__DOT__u_probe__DOT__does_store));
    vlSelfRef.harness_tb__DOT__u_probe__DOT__write_rd 
        = (((0x00000040U & vlSelfRef.insn_in) ? (IData)(
                                                        ((0x00000028U 
                                                          == 
                                                          (0x00000038U 
                                                           & vlSelfRef.insn_in)) 
                                                         & (IData)(__VdfgRegularize_hebeb780c_0_5)))
             : ((0x00000020U & vlSelfRef.insn_in) ? (IData)(
                                                            ((0x00000010U 
                                                              == 
                                                              (0x00000018U 
                                                               & vlSelfRef.insn_in)) 
                                                             & (IData)(__VdfgRegularize_hebeb780c_0_5)))
                 : (IData)((0x00000013U == (0x0000701fU 
                                            & vlSelfRef.insn_in))))) 
           & (0U != (IData)(vlSelfRef.rd_index)));
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
    if ((0x00000040U & vlSelfRef.insn_in)) {
        vlSelfRef.illegal = (1U & ((~ (IData)((0x0000002fU 
                                               == (0x0000002fU 
                                                   & vlSelfRef.insn_in)))) 
                                   | (vlSelfRef.insn_in 
                                      >> 4U)));
        vlSelfRef.rd_value_out = (vlSelfRef.next_pc 
                                  & (- (QData)((IData)(
                                                       (0x0000002fU 
                                                        == 
                                                        (0x0000003fU 
                                                         & vlSelfRef.insn_in))))));
    } else if ((0x00000020U & vlSelfRef.insn_in)) {
        vlSelfRef.illegal = (1U & ((0x00000010U & vlSelfRef.insn_in)
                                    ? ((~ (7U == (7U 
                                                  & vlSelfRef.insn_in))) 
                                       | (vlSelfRef.insn_in 
                                          >> 3U)) : (IData)(
                                                            (0x00003003U 
                                                             != 
                                                             (0x0000700fU 
                                                              & vlSelfRef.insn_in)))));
        vlSelfRef.rd_value_out = ((- (QData)((IData)(
                                                     (0x00000017U 
                                                      == 
                                                      (0x0000001fU 
                                                       & vlSelfRef.insn_in))))) 
                                  & (QData)((IData)(
                                                    (0xfffff000U 
                                                     & vlSelfRef.insn_in))));
    } else {
        vlSelfRef.illegal = (1U & (IData)((0x00000013U 
                                           != (0x0000701fU 
                                               & vlSelfRef.insn_in))));
        vlSelfRef.rd_value_out = (vlSelfRef.mem_addr 
                                  & (- (QData)((IData)(
                                                       (0x00000013U 
                                                        == 
                                                        (0x0000701fU 
                                                         & vlSelfRef.insn_in))))));
    }
    vlSelfRef.rd_we = ((IData)(vlSelfRef.step_valid) 
                       & ((~ (IData)(vlSelfRef.illegal)) 
                          & (IData)(vlSelfRef.harness_tb__DOT__u_probe__DOT__write_rd)));
    vlSelfRef.mem_wdata = ((IData)(vlSelfRef.harness_tb__DOT__u_probe__DOT__does_store)
                            ? vlSelfRef.harness_tb__DOT__u_probe__DOT__xregs
                           [(0x0000001fU & (vlSelfRef.insn_in 
                                            >> 0x00000014U))]
                            : (vlSelfRef.rd_value_out 
                               & (- (QData)((IData)(
                                                    (0U 
                                                     != (IData)(vlSelfRef.rd_index)))))));
}

bool Vharness_tb___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in);

#ifdef VL_DEBUG
VL_ATTR_COLD void Vharness_tb___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___dump_triggers__ico\n"); );
    // Body
    if ((1U & (~ (IData)(Vharness_tb___024root___trigger_anySet__ico(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: @( clk)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 1U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 1 is active: @( rst)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 2U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 2 is active: @( step_valid)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 3U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 3 is active: @( pc_in)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 4U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 4 is active: @( insn_in)\n");
    }
    if ((1U & (IData)(triggers[1U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 64 is active: Internal 'ico' trigger - first iteration\n");
    }
}
#endif  // VL_DEBUG

bool Vharness_tb___024root___trigger_anySet__act(const VlUnpacked<QData/*63:0*/, 1> &in);

#ifdef VL_DEBUG
VL_ATTR_COLD void Vharness_tb___024root___dump_triggers__act(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___dump_triggers__act\n"); );
    // Body
    if ((1U & (~ (IData)(Vharness_tb___024root___trigger_anySet__act(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: @(posedge clk)\n");
    }
}
#endif  // VL_DEBUG

VL_ATTR_COLD void Vharness_tb___024root___ctor_var_reset(Vharness_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vharness_tb___024root___ctor_var_reset\n"); );
    Vharness_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    const uint64_t __VscopeHash = VL_MURMUR64_HASH(vlSelf->vlNamep);
    vlSelf->clk = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 16707436170211756652ull);
    vlSelf->rst = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 18209466448985614591ull);
    vlSelf->step_valid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 13205812531432268174ull);
    vlSelf->step_ready = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 18240843361901587921ull);
    vlSelf->pc_in = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 4451962802771921047ull);
    vlSelf->insn_in = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 13636782990827057824ull);
    vlSelf->mem_en = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 9775133303814137848ull);
    vlSelf->mem_addr = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 326597072690670135ull);
    vlSelf->mem_wdata = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 5431754401481461448ull);
    vlSelf->mem_byte_en = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 3641948651044933622ull);
    vlSelf->retire = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 10454370077121718844ull);
    vlSelf->next_pc = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 13592750897586193959ull);
    vlSelf->illegal = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 14027783744025561445ull);
    vlSelf->rd_we = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 591954311741417218ull);
    vlSelf->rd_index = VL_SCOPED_RAND_RESET_I(5, __VscopeHash, 7372217062034325348ull);
    vlSelf->rd_value_out = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 2864909112941848831ull);
    for (int __Vi0 = 0; __Vi0 < 32; ++__Vi0) {
        vlSelf->harness_tb__DOT__u_probe__DOT__xregs[__Vi0] = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 5680544343963798077ull);
    }
    vlSelf->harness_tb__DOT__u_probe__DOT__does_store = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 9333979477452932104ull);
    vlSelf->harness_tb__DOT__u_probe__DOT__write_rd = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 2517720902364472770ull);
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VstlTriggered[__Vi0] = 0;
    }
    for (int __Vi0 = 0; __Vi0 < 2; ++__Vi0) {
        vlSelf->__VicoTriggered[__Vi0] = 0;
    }
    vlSelf->__Vtrigprevexpr___TOP__clk__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__rst__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__step_valid__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__pc_in__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__insn_in__0 = 0;
    vlSelf->__VicoDidInit = 0;
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VactTriggered[__Vi0] = 0;
    }
    vlSelf->__Vtrigprevexpr___TOP__clk__1 = 0;
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VnbaTriggered[__Vi0] = 0;
    }
}
