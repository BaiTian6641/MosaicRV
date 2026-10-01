// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Model implementation (design independent parts)

#include "Vmosaic_alu_tb__pch.h"

//============================================================
// Constructors

Vmosaic_alu_tb::Vmosaic_alu_tb(VerilatedContext* _vcontextp__, const char* _vcname__)
    : VerilatedModel{*_vcontextp__}
    , vlSymsp{new Vmosaic_alu_tb__Syms(contextp(), _vcname__, this)}
    , m_evalLoop{*this, /*convergeLimit:*/ 10000}
    , op{vlSymsp->TOP.op}
    , zero{vlSymsp->TOP.zero}
    , a{vlSymsp->TOP.a}
    , b{vlSymsp->TOP.b}
    , result{vlSymsp->TOP.result}
    , rootp{&(vlSymsp->TOP)}
{
    // Register model with the context
    contextp()->addModel(this);
}

Vmosaic_alu_tb::Vmosaic_alu_tb(const char* _vcname__)
    : Vmosaic_alu_tb(Verilated::threadContextp(), _vcname__)
{
}

//============================================================
// Destructor

Vmosaic_alu_tb::~Vmosaic_alu_tb() {
    delete vlSymsp;
}

//============================================================
// Evaluation function

#ifdef VL_DEBUG
void Vmosaic_alu_tb___024root___eval_debug_assertions(Vmosaic_alu_tb___024root* vlSelf);
#endif  // VL_DEBUG
VL_ATTR_COLD void Vmosaic_alu_tb___024root___eval_static(Vmosaic_alu_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_alu_tb___024root___eval_initial(Vmosaic_alu_tb___024root* vlSelf);
VL_ATTR_COLD bool Vmosaic_alu_tb___024root___eval_stl(Vmosaic_alu_tb___024root* vlSelf, CData/*0:0*/ firstIteration);
void Vmosaic_alu_tb___024root___eval_sample(Vmosaic_alu_tb___024root* vlSelf);
bool Vmosaic_alu_tb___024root___eval_ico(Vmosaic_alu_tb___024root* vlSelf, CData/*0:0*/ firstIteration);
bool Vmosaic_alu_tb___024root___eval_act(Vmosaic_alu_tb___024root* vlSelf);
bool Vmosaic_alu_tb___024root___eval_inact(Vmosaic_alu_tb___024root* vlSelf);
bool Vmosaic_alu_tb___024root___eval_nba(Vmosaic_alu_tb___024root* vlSelf);
bool Vmosaic_alu_tb___024root___eval_obs(Vmosaic_alu_tb___024root* vlSelf);
bool Vmosaic_alu_tb___024root___eval_react(Vmosaic_alu_tb___024root* vlSelf);
void Vmosaic_alu_tb___024root___eval_postponed(Vmosaic_alu_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_alu_tb___024root___eval_final(Vmosaic_alu_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_alu_tb___024root___eval_dump_triggers__stl(Vmosaic_alu_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_alu_tb___024root___eval_dump_triggers__ico(Vmosaic_alu_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_alu_tb___024root___eval_dump_triggers__act(Vmosaic_alu_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_alu_tb___024root___eval_dump_triggers__nba(Vmosaic_alu_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_alu_tb___024root___eval_dump_triggers__obs(Vmosaic_alu_tb___024root* vlSelf);
VL_ATTR_COLD void Vmosaic_alu_tb___024root___eval_dump_triggers__react(Vmosaic_alu_tb___024root* vlSelf);

void Vmosaic_alu_tb::eval_step() {
    VL_DEBUG_IF(VL_DBG_MSGF("+++++TOP Evaluate Vmosaic_alu_tb::eval_step\n"); );
    m_evalLoop.eval();
}

void Vmosaic_alu_tb::evalBegin() {
#ifdef VL_DEBUG
    // Debug assertions
    Vmosaic_alu_tb___024root___eval_debug_assertions(&(vlSymsp->TOP));
#endif  // VL_DEBUG
    vlSymsp->__Vm_deleter.deleteAll();
}

void Vmosaic_alu_tb::evalEnd() {
    // Evaluate cleanup
    Verilated::endOfEval(vlSymsp->__Vm_evalMsgQp);
}

void Vmosaic_alu_tb::evalStatic() {
    Vmosaic_alu_tb___024root___eval_static(&(vlSymsp->TOP));
}

void Vmosaic_alu_tb::evalInitial() {
    Vmosaic_alu_tb___024root___eval_initial(&(vlSymsp->TOP));
}

bool Vmosaic_alu_tb::evalStl(bool firstIteration) {
    return Vmosaic_alu_tb___024root___eval_stl(&(vlSymsp->TOP), firstIteration);
}

void Vmosaic_alu_tb::evalSample() {
    Vmosaic_alu_tb___024root___eval_sample(&(vlSymsp->TOP));
}

bool Vmosaic_alu_tb::evalIco(bool firstIteration) {
    return Vmosaic_alu_tb___024root___eval_ico(&(vlSymsp->TOP), firstIteration);
}

bool Vmosaic_alu_tb::evalAct() {
    return Vmosaic_alu_tb___024root___eval_act(&(vlSymsp->TOP));
}

bool Vmosaic_alu_tb::evalInact() {
    return Vmosaic_alu_tb___024root___eval_inact(&(vlSymsp->TOP));
}

bool Vmosaic_alu_tb::evalNba() {
    return Vmosaic_alu_tb___024root___eval_nba(&(vlSymsp->TOP));
}

bool Vmosaic_alu_tb::evalObs() {
    return Vmosaic_alu_tb___024root___eval_obs(&(vlSymsp->TOP));
}

bool Vmosaic_alu_tb::evalReact() {
    return Vmosaic_alu_tb___024root___eval_react(&(vlSymsp->TOP));
}

void Vmosaic_alu_tb::evalPostponed() {
    Vmosaic_alu_tb___024root___eval_postponed(&(vlSymsp->TOP));
}

void Vmosaic_alu_tb::evalFinal() {
    Vmosaic_alu_tb___024root___eval_final(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vmosaic_alu_tb::dumpTriggersStl() {
    Vmosaic_alu_tb___024root___eval_dump_triggers__stl(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vmosaic_alu_tb::dumpTriggersIco() {
    Vmosaic_alu_tb___024root___eval_dump_triggers__ico(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vmosaic_alu_tb::dumpTriggersAct() {
    Vmosaic_alu_tb___024root___eval_dump_triggers__act(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vmosaic_alu_tb::dumpTriggersNba() {
    Vmosaic_alu_tb___024root___eval_dump_triggers__nba(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vmosaic_alu_tb::dumpTriggersObs() {
    Vmosaic_alu_tb___024root___eval_dump_triggers__obs(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Vmosaic_alu_tb::dumpTriggersReact() {
    Vmosaic_alu_tb___024root___eval_dump_triggers__react(&(vlSymsp->TOP));
}

//============================================================
// Events and timing
bool Vmosaic_alu_tb::eventsPending() { return false; }

uint64_t Vmosaic_alu_tb::nextTimeSlot() {
    VL_FATAL_MT(__FILE__, __LINE__, "", "No delays in the design");
    return 0;
}

//============================================================
// Utilities

const char* Vmosaic_alu_tb::name() const {
    return vlSymsp->name();
}

//============================================================
// Invoke final blocks

VL_ATTR_COLD void Vmosaic_alu_tb::final() {
    contextp()->executingFinal(true);
    evalFinal();
    contextp()->executingFinal(false);
}

//============================================================
// Implementations of abstract methods from VerilatedModel

const char* Vmosaic_alu_tb::hierName() const { return vlSymsp->name(); }
const char* Vmosaic_alu_tb::modelName() const { return "Vmosaic_alu_tb"; }
unsigned Vmosaic_alu_tb::threads() const { return 1; }
void Vmosaic_alu_tb::prepareClone() const { contextp()->prepareClone(); }
void Vmosaic_alu_tb::atClone() const {
    contextp()->threadPoolpOnClone();
}
