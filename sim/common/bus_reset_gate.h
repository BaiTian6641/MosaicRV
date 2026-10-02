// ============================================================================
// bus_reset_gate.h -- the reset-traffic rule, in one place (V-010).
//
// ------------------------------------------------------------------ the rule
//
// While reset is asserted, a driver's bus model MUST NOT accept, queue or
// deliver a request.  A request the DUT presents while reset is asserted is
// *ignored*: the model accepts nothing and answers nothing.  That is the
// defined behaviour; it does not depend on what the memory model happens to do.
//
// ------------------------------------------------------------------ why
//
// The OoO core's fetch unit matches a response to a fetch slot by {id, epoch},
// and only a redirect advances the epoch.  During reset the fetch unit's PC
// register is held at the reset vector and its slot has not been recorded busy,
// so it *presents* the reset-vector request on every reset cycle.  A bus model
// that accepts those requests queues responses that the core discards at reset;
// they then sit in front of the post-reset responses with the *same* id and
// epoch, so fetch pairs a stale response with a fresh request and the
// instruction stream shifts by one.  From outside that looks exactly like a
// retired payload (`ev_rd`/`ev_value`) lagging its program counter.
//
// The hazard was found and fixed in one cache-integration driver by an
// order-swap experiment.  It is latent in every other core driver, and they
// only escape it because their programs happen to start with a redirecting
// instruction (`JAL`) that bumps the epoch and drops the stale responses.  That
// is a property of the stimulus, not of the harness.
//
// ------------------------------------------------------- how it is enforced
//
// Every core driver holds one `mosaic::BusResetGate` and routes each bus
// accept/response decision through it, so the rule has exactly one
// implementation and a new driver inherits it by holding a gate rather than by
// remembering the rule.  A conforming driver calls:
//
//     if (gate.MayAccept(rst, (req_valid && req_ready))) { bus.Accept(...); }
//     if (gate.MayDeliver(rst) && (rsp_valid && rsp_ready))    { bus.Pop(); }
//
// and nothing else touches the bus.
//
// ------------------------------------------------------- the two named knobs
//
//   * `SetObserveResetTraffic(true)` is the explicit, named option for a driver
//     that genuinely needs to *see* reset-time traffic (for example a case that
//     asserts the DUT presents nothing during reset).  Observation records what
//     was presented and never changes what the bus does.  It is OFF by default:
//     the default behaviour is to ignore the traffic silently.
//
//   * `SetAcceptDuringResetControl(true)` is the negative-control hook.  It
//     restores the pre-rule behaviour (accept during reset) so a case can prove
//     that it is the case which catches the class: with the control engaged the
//     accepted-during-reset count goes non-zero and the run breaks.  It is OFF
//     by default and must never be engaged in a shipping run.
//
// The counters below are only advanced when the matching knob is engaged, so a
// conforming driver carries no bookkeeping and no behavioural difference.
// ============================================================================

#ifndef MOSAIC_BUS_RESET_GATE_H_
#define MOSAIC_BUS_RESET_GATE_H_

#include <cstdint>

namespace mosaic {

class BusResetGate {
 public:
  // May the bus model accept the request the DUT presents this cycle?
  // `request_presented` is `req_valid && req_ready`.  True => accept it.
  // False => do not: either nothing was presented, or reset is asserted and the
  // request is ignored (the rule), unless the negative control is engaged.
  bool MayAccept(bool reset_asserted, bool request_presented) {
    if (!request_presented) return false;
    if (!reset_asserted) return true;
    if (accept_during_reset_control_) {
      ++accepted_during_reset_;
      return true;
    }
    if (observe_reset_traffic_) ++presented_during_reset_;
    return false;
  }

  // May the bus model pop/deliver a response this cycle?  A response must not
  // cross a reset, so delivery is suppressed while reset is asserted too.
  bool MayDeliver(bool reset_asserted) const {
    return !reset_asserted || accept_during_reset_control_;
  }

  // The explicit, named observation option.  Off by default.
  void SetObserveResetTraffic(bool on) { observe_reset_traffic_ = on; }
  bool observe_reset_traffic() const { return observe_reset_traffic_; }

  // The negative-control hook.  Off by default; never engaged when shipping.
  void SetAcceptDuringResetControl(bool on) { accept_during_reset_control_ = on; }
  bool accept_during_reset_control() const { return accept_during_reset_control_; }

  // Requests the DUT presented while reset was asserted.  Only advanced while
  // observation is enabled; a conforming DUT presents none.
  uint64_t presented_during_reset() const { return presented_during_reset_; }

  // Requests actually accepted while reset was asserted.  Zero under the rule;
  // non-zero only while the control is engaged.
  uint64_t accepted_during_reset() const { return accepted_during_reset_; }

 private:
  bool observe_reset_traffic_ = false;
  bool accept_during_reset_control_ = false;
  uint64_t presented_during_reset_ = 0;
  uint64_t accepted_during_reset_ = 0;
};

}  // namespace mosaic

#endif  // MOSAIC_BUS_RESET_GATE_H_
