// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vmosaic_alu_tb.h for the primary calling header

#include "Vmosaic_alu_tb__pch.h"

VL_ATTR_COLD void Vmosaic_alu_tb___024root___eval_static(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_static\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    vlSelfRef.__Vtrigprevexpr___TOP__a__0 = vlSelfRef.a;
    vlSelfRef.__Vtrigprevexpr___TOP__b__0 = vlSelfRef.b;
    vlSelfRef.__Vtrigprevexpr___TOP__op__0 = vlSelfRef.op;
}

VL_ATTR_COLD void Vmosaic_alu_tb___024root___eval_initial(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_initial\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_alu_tb___024root___dump_triggers__stl(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag);
#endif  // VL_DEBUG
VL_ATTR_COLD bool Vmosaic_alu_tb___024root___trigger_anySet__stl(const VlUnpacked<QData/*63:0*/, 1> &in);
VL_ATTR_COLD void Vmosaic_alu_tb___024root___stl_sequent__TOP__0(Vmosaic_alu_tb___024root* vlSelf);

VL_ATTR_COLD bool Vmosaic_alu_tb___024root___eval_stl(Vmosaic_alu_tb___024root* vlSelf, CData/*0:0*/ firstIteration) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_stl\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VstlExecute;
    // Body
    vlSelfRef.__VstlTriggered[0U] = ((0xfffffffffffffffeULL 
                                      & vlSelfRef.__VstlTriggered[0U]) 
                                     | (IData)((IData)(firstIteration)));
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Vmosaic_alu_tb___024root___dump_triggers__stl(vlSelfRef.__VstlTriggered, "stl"s);
    }
#endif
    __VstlExecute = Vmosaic_alu_tb___024root___trigger_anySet__stl(vlSelfRef.__VstlTriggered);
    if (__VstlExecute) {
        {
            // Inlined CFunc: _eval_body__stl
            if ((1ULL & vlSelfRef.__VstlTriggered[0U])) {
                Vmosaic_alu_tb___024root___stl_sequent__TOP__0(vlSelf);
            }
        }
    }
    return (__VstlExecute);
}

VL_ATTR_COLD void Vmosaic_alu_tb___024root___eval_dump_triggers__stl(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_dump_triggers__stl\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
#ifdef VL_DEBUG
    Vmosaic_alu_tb___024root___dump_triggers__stl(vlSelfRef.__VstlTriggered, "stl"s);
#endif
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_alu_tb___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag);
#endif  // VL_DEBUG

VL_ATTR_COLD void Vmosaic_alu_tb___024root___eval_dump_triggers__ico(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_dump_triggers__ico\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
#ifdef VL_DEBUG
    Vmosaic_alu_tb___024root___dump_triggers__ico(vlSelfRef.__VicoTriggered, "ico"s);
#endif
}

VL_ATTR_COLD void Vmosaic_alu_tb___024root___eval_dump_triggers__act(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_dump_triggers__act\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

VL_ATTR_COLD void Vmosaic_alu_tb___024root___eval_dump_triggers__nba(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_dump_triggers__nba\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

VL_ATTR_COLD void Vmosaic_alu_tb___024root___eval_dump_triggers__obs(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_dump_triggers__obs\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

VL_ATTR_COLD void Vmosaic_alu_tb___024root___eval_dump_triggers__react(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_dump_triggers__react\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

VL_ATTR_COLD void Vmosaic_alu_tb___024root___eval_final(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___eval_final\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_alu_tb___024root___dump_triggers__stl(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___dump_triggers__stl\n"); );
    // Body
    if ((1U & (~ (IData)(Vmosaic_alu_tb___024root___trigger_anySet__stl(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: Internal 'stl' trigger - first iteration\n");
    }
}
#endif  // VL_DEBUG

VL_ATTR_COLD bool Vmosaic_alu_tb___024root___trigger_anySet__stl(const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___trigger_anySet__stl\n"); );
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

VL_ATTR_COLD void Vmosaic_alu_tb___024root___stl_sequent__TOP__0(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___stl_sequent__TOP__0\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    vlSelfRef.__VdfgRegularize_hebeb780c_0_0 = ((IData)(vlSelfRef.a) 
                                                + (IData)(vlSelfRef.b));
    vlSelfRef.__VdfgRegularize_hebeb780c_0_1 = ((IData)(vlSelfRef.a) 
                                                - (IData)(vlSelfRef.b));
    vlSelfRef.__VdfgRegularize_hebeb780c_0_2 = ((IData)(vlSelfRef.a) 
                                                << 
                                                (0x0000001fU 
                                                 & (IData)(vlSelfRef.b)));
    vlSelfRef.__VdfgRegularize_hebeb780c_0_3 = ((IData)(vlSelfRef.a) 
                                                >> 
                                                (0x0000001fU 
                                                 & (IData)(vlSelfRef.b)));
    vlSelfRef.__VdfgRegularize_hebeb780c_0_4 = VL_SHIFTRS_III(32,32,5, (IData)(vlSelfRef.a), 
                                                              (0x0000001fU 
                                                               & (IData)(vlSelfRef.b)));
    vlSelfRef.result = ((8U & (IData)(vlSelfRef.op))
                         ? ((4U & (IData)(vlSelfRef.op))
                             ? ((2U & (IData)(vlSelfRef.op))
                                 ? ((1U & (IData)(vlSelfRef.op))
                                     ? vlSelfRef.b : 
                                    (((QData)((IData)(
                                                      (- (IData)(
                                                                 (vlSelfRef.__VdfgRegularize_hebeb780c_0_4 
                                                                  >> 0x0000001fU))))) 
                                      << 0x00000020U) 
                                     | (QData)((IData)(vlSelfRef.__VdfgRegularize_hebeb780c_0_4))))
                                 : ((1U & (IData)(vlSelfRef.op))
                                     ? (((QData)((IData)(
                                                         (- (IData)(
                                                                    (vlSelfRef.__VdfgRegularize_hebeb780c_0_3 
                                                                     >> 0x0000001fU))))) 
                                         << 0x00000020U) 
                                        | (QData)((IData)(vlSelfRef.__VdfgRegularize_hebeb780c_0_3)))
                                     : (((QData)((IData)(
                                                         (- (IData)(
                                                                    (vlSelfRef.__VdfgRegularize_hebeb780c_0_2 
                                                                     >> 0x0000001fU))))) 
                                         << 0x00000020U) 
                                        | (QData)((IData)(vlSelfRef.__VdfgRegularize_hebeb780c_0_2)))))
                             : ((2U & (IData)(vlSelfRef.op))
                                 ? ((1U & (IData)(vlSelfRef.op))
                                     ? (((QData)((IData)(
                                                         (- (IData)(
                                                                    (vlSelfRef.__VdfgRegularize_hebeb780c_0_1 
                                                                     >> 0x0000001fU))))) 
                                         << 0x00000020U) 
                                        | (QData)((IData)(vlSelfRef.__VdfgRegularize_hebeb780c_0_1)))
                                     : (((QData)((IData)(
                                                         (- (IData)(
                                                                    (vlSelfRef.__VdfgRegularize_hebeb780c_0_0 
                                                                     >> 0x0000001fU))))) 
                                         << 0x00000020U) 
                                        | (QData)((IData)(vlSelfRef.__VdfgRegularize_hebeb780c_0_0))))
                                 : ((1U & (IData)(vlSelfRef.op))
                                     ? (vlSelfRef.a 
                                        & vlSelfRef.b)
                                     : (vlSelfRef.a 
                                        | vlSelfRef.b))))
                         : ((4U & (IData)(vlSelfRef.op))
                             ? ((2U & (IData)(vlSelfRef.op))
                                 ? ((1U & (IData)(vlSelfRef.op))
                                     ? VL_SHIFTRS_QQI(64,64,6, vlSelfRef.a, 
                                                      (0x0000003fU 
                                                       & (IData)(vlSelfRef.b)))
                                     : (vlSelfRef.a 
                                        >> (0x0000003fU 
                                            & (IData)(vlSelfRef.b))))
                                 : ((1U & (IData)(vlSelfRef.op))
                                     ? (vlSelfRef.a 
                                        ^ vlSelfRef.b)
                                     : (1ULL & (- (QData)((IData)(
                                                                  (vlSelfRef.a 
                                                                   < vlSelfRef.b)))))))
                             : ((2U & (IData)(vlSelfRef.op))
                                 ? ((1U & (IData)(vlSelfRef.op))
                                     ? (1ULL & (- (QData)((IData)(
                                                                  VL_LTS_IQQ(64, vlSelfRef.a, vlSelfRef.b)))))
                                     : (vlSelfRef.a 
                                        << (0x0000003fU 
                                            & (IData)(vlSelfRef.b))))
                                 : ((1U & (IData)(vlSelfRef.op))
                                     ? (vlSelfRef.a 
                                        - vlSelfRef.b)
                                     : (vlSelfRef.a 
                                        + vlSelfRef.b)))));
    vlSelfRef.zero = (0ULL == vlSelfRef.result);
}

bool Vmosaic_alu_tb___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in);

#ifdef VL_DEBUG
VL_ATTR_COLD void Vmosaic_alu_tb___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___dump_triggers__ico\n"); );
    // Body
    if ((1U & (~ (IData)(Vmosaic_alu_tb___024root___trigger_anySet__ico(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: @( a)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 1U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 1 is active: @( b)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 2U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 2 is active: @( op)\n");
    }
    if ((1U & (IData)(triggers[1U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 64 is active: Internal 'ico' trigger - first iteration\n");
    }
}
#endif  // VL_DEBUG

VL_ATTR_COLD void Vmosaic_alu_tb___024root___ctor_var_reset(Vmosaic_alu_tb___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vmosaic_alu_tb___024root___ctor_var_reset\n"); );
    Vmosaic_alu_tb__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    const uint64_t __VscopeHash = VL_MURMUR64_HASH(vlSelf->vlNamep);
    vlSelf->a = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 510903276987443985ull);
    vlSelf->b = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 16900879642891266615ull);
    vlSelf->op = VL_SCOPED_RAND_RESET_I(4, __VscopeHash, 3630531923276091163ull);
    vlSelf->result = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 16664408842984530663ull);
    vlSelf->zero = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 10977623970759875275ull);
    vlSelf->__VdfgRegularize_hebeb780c_0_0 = 0;
    vlSelf->__VdfgRegularize_hebeb780c_0_1 = 0;
    vlSelf->__VdfgRegularize_hebeb780c_0_2 = 0;
    vlSelf->__VdfgRegularize_hebeb780c_0_3 = 0;
    vlSelf->__VdfgRegularize_hebeb780c_0_4 = 0;
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VstlTriggered[__Vi0] = 0;
    }
    for (int __Vi0 = 0; __Vi0 < 2; ++__Vi0) {
        vlSelf->__VicoTriggered[__Vi0] = 0;
    }
    vlSelf->__Vtrigprevexpr___TOP__a__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__b__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__op__0 = 0;
    vlSelf->__VicoDidInit = 0;
}
