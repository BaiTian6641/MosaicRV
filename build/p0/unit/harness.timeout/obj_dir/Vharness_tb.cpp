// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Model implementation (design independent parts)

#include "Vharness_tb__pch.h"

//============================================================
// Constructors

Vharness_tb::Vharness_tb(VerilatedContext* _vcontextp__, const char* _vcname__)
    : VerilatedModel{*_vcontextp__}
    , vlSymsp{new Vharness_tb__Syms(contextp(), _vcname__, this)}
    , m_evalLoop{*this, /*convergeLimit:*/ 10000}
    , clk{vlSymsp->TOP.clk}
    , rst{vlSymsp->TOP.rst}
    , step_valid{vlSymsp->TOP.step_valid}
    , step_ready{vlSymsp->TOP.step_ready}
    , mem_en{vlSymsp->TOP.mem_en}
    , mem_byte_en{vlSymsp->TOP.mem_byte_en}
    , retire{vlSymsp->TOP.retire}
    , illegal{vlSymsp->TOP.illegal}
    , rd_we{vlSymsp->TOP.rd_we}
    , rd_index{vlSymsp->TOP.rd_index}
    , insn_in{vlSymsp->TOP.insn_in}
    , pc_in{vlSymsp->TOP.pc_in}
    , mem_addr{vlSymsp->TOP.mem_addr}
    , mem_wdata{vlSymsp->TOP.mem_wdata}
    , next_pc{vlSymsp->TOP.next_pc}
    , rd_value_out{vlSymsp->TOP.rd_value_out}
    , rootp{&(vlSymsp->TOP)}
{
    // Register model with the context
    contextp()->addModel(this);
}

Vharness_tb::Vharness_tb(const char* _vcname__)
    : Vharness_tb(Verilated::threadContextp(), _vcname__)
{
}

//============================================================
// Destructor

Vharness_tb::~Vharness_tb() {
    delete vlSymsp;
}

//============================================================
// Evaluation function

#ifdef VL_DEBUG
void Vharness_tb___024root___eval_debug_assertions(Vharness_tb___024root* vlSelf);
#endif  // VL_DEBUG
VL_ATTR_COLD void Vharness_tb___024root___eval_static(Vharness_tb___024root* vlSelf);
VL_ATTR_COLD void Vharness_tb___024root___eval_initial(Vharness_tb___024root* vlSelf);
VL_ATTR_COLD bool Vharness_tb___024root___eval_stl(Vharness_tb___024root* vlSelf, CData/*0:0*/ firstIteration);
void Vharness_tb___024root___eval_sample(Vharness_tb___024root* vlSelf);
bool Vharness_tb___024root___eval_ico(Vharness_tb___024root* vlSelf, CData/*0:0*/ firstIteration);
bool Vharness_tb___024root___eval_act(Vharness_tb___024root* vlSelf);
bool Vharness_tb___024root___eval_inact(Vharness_tb___024root* vlSelf);
bool Vharness_tb___024root___eval_nba(Vharness_tb___024root* vlSelf);
bool Vharness_tb___024root___eval_obs(Vharness_tb___024root* vlSelf);
bool Vharness_tb___024root___eval_react(Vharness_tb___024root* vlSelf);
void Vharness_tb___024root___eval_postponed(Vharness_tb___024root* vlSelf);
VL_ATTR_COLD void Vharness_tb___024root___eval_final(Vharness_tb___024root* vlSelf);
VL_ATTR_COLD void Vharness_tb___024root___eval_dump_triggers__stl(Vharness_tb___024root* vlSelf);
VL_ATTR_COLD void Vharness_tb___024root___eval_dump_triggers__ico(Vharness_tb___024root* vlSelf);
VL_ATTR_COLD void Vharness_tb___024root___eval_dump_triggers__act(Vharness_tb___024root* vlSelf);
VL_ATTR_COLD void Vharness_tb___024root___eval_dump_triggers__nba(Vharness_tb___024root* vlSelf);
VL_ATTR_COLD void Vharness_tb___024root___eval_dump_triggers__obs(Vharness_tb___024root* vlSelf);
VL_ATTR_COLD void Vharness_tb___024root___eval_dump_triggers__react(Vharness_tb___024root* vlSelf);

void Vharness_tb::eval_step() {
    VL_DEBUG_IF(VL_DBG_MSGF("+++++TOP Evaluate Vharness_tb::eval_step\n"); );
    m_evalLoop.eval();
}

void Vharness_tb::evalBegin() {
#ifdef VL_DEBUG
    // Debug assertions
    Vharness_tb___024root___eval_debug_assertions(&(vlSymsp->TOP));
#endif  // VL_DEBUG
    vlSymsp->__Vm_deleter.deleteAll();
}

void Vharness_tb::evalEnd() {
    // Evaluate cleanup
    Verilated::endOfEval(vlSymsp->__Vm_evalMsgQp);
}

void Vharness_tb::evalStatic() {
    Vharness_tb___024root___eval_static(&(vlSymsp->TOP));
}

void Vharness_tb::evalInitial() {
    Vharness_tb___024root___eval_initial(&(vlSymsp->TOP));
}

bool Vharness_tb::evalStl(bool firstIteration) {
    return Vharness_tb___024root___eval_stl(&(vlSymsp->TOP), firstIteration);
}

void Vharness_tb::evalSample() {
    Vharness_tb___024root___eval_sample(&(vlSymsp->TOP));
}

bool Vharness_tb::evalIco(bool firstIteration) {
    return Vharness_tb___024root___eval_ico(&(vlSymsp->TOP), firstIteration);
}

bool Vharness_tb::evalAct() {
    return Vharness_tb___024root___eval_act(&(vlSymsp->TOP));
}

bool Vharness_tb::evalInact() {
    return Vharness_tb___024root___eval_inact(&(vlSymsp->TOP));
}

bool Vharness_tb::evalNba() {
    return Vharness_tb___024root___eval_nba(&(vlSymsp->TOP));
}

bool Vharness_tb::evalObs() {
    return Vharness_tb___024root___eval_obs(&(vlSymsp->TOP));
}

bool Vharness_tb::evalReact() {
    return Vharness_tb___024root___eval_react(&(vlSymsp->TOP));
}

void Vharness_tb::evalPostponed() {
    Vharness_tb___024root___eval_postponed(&(vlSymsp->TOP));
}

void Vharness_tb::evalFinal() {
    Vharness_tb___024root___eval_final(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vharness_tb::dumpTriggersStl() {
    Vharness_tb___024root___eval_dump_triggers__stl(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vharness_tb::dumpTriggersIco() {
    Vharness_tb___024root___eval_dump_triggers__ico(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vharness_tb::dumpTriggersAct() {
    Vharness_tb___024root___eval_dump_triggers__act(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vharness_tb::dumpTriggersNba() {
    Vharness_tb___024root___eval_dump_triggers__nba(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vharness_tb::dumpTriggersObs() {
    Vharness_tb___024root___eval_dump_triggers__obs(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vharness_tb::dumpTriggersReact() {
    Vharness_tb___024root___eval_dump_triggers__react(&(vlSymsp->TOP));
}

//============================================================
// Events and timing
bool Vharness_tb::eventsPending() { return false; }

uint64_t Vharness_tb::nextTimeSlot() {
    VL_FATAL_MT(__FILE__, __LINE__, "", "No delays in the design");
    return 0;
}

//============================================================
// Utilities

const char* Vharness_tb::name() const {
    return vlSymsp->name();
}

//============================================================
// Invoke final blocks

VL_ATTR_COLD void Vharness_tb::final() {
    contextp()->executingFinal(true);
    evalFinal();
    contextp()->executingFinal(false);
}

//============================================================
// Implementations of abstract methods from VerilatedModel

const char* Vharness_tb::hierName() const { return vlSymsp->name(); }
const char* Vharness_tb::modelName() const { return "Vharness_tb"; }
unsigned Vharness_tb::threads() const { return 1; }
void Vharness_tb::prepareClone() const { contextp()->prepareClone(); }
void Vharness_tb::atClone() const {
    contextp()->threadPoolpOnClone();
}
