#pragma once

#include <HAPMessages/HAPDataMessages.hpp>
#include <HAPNode/HAPNode.hpp>

#include <etl/vector.h>

/**
 * @brief Decides when a node has something worth saying.
 *
 * Every out port carries a policy - an interval, and for an analog value a
 * deadband - and this class turns the node's current readings plus those
 * policies into one question: is a report due?
 *
 * ## No tick, no state machine
 * isDue() is a pure function of the values, the policies and the clock. There
 * is nothing to update() and nothing to get out of step: a node asks when it is
 * about to decide whether to transmit, which on a battery is once per wake and
 * on a mains node is once per loop.
 *
 * ## The deadband decides whether to SPEAK, not what to say
 * When anything is due, the report carries **every** out port. A master gets a
 * consistent snapshot rather than a scattering of single values, and the few
 * extra bytes cost nothing next to the transmission itself - which the deadband
 * has already decided is happening.
 *
 * ## A node may refuse what its battery cannot afford
 * setPolicy() clamps an interval below the minimum this node is willing to run
 * at, and the response reports what was actually taken. A master asking a
 * battery sensor to report every second is not refused, it is answered with the
 * truth.
 */
class HAPReporter {
 public:
  explicit HAPReporter(const HAPNode& node) noexcept;

  /**
   * @brief Builds a policy for every out port the node has.
   *
   * Call after the application has added its instances. Ports added later are
   * not covered - the class set is fixed at start-up, so there are none.
   *
   * @param defaultIntervalSec 0 means report only on change.
   * @param defaultDeadband Null for none; a Float for an analog port.
   */
  void begin(uint16_t defaultIntervalSec, const HValue& defaultDeadband) noexcept;

  /**
   * @brief The shortest interval this node will accept.
   *
   * A battery node's wake period is a floor on its reporting whatever a master
   * asks for, and saying so is more use than silently ignoring the request.
   */
  void setMinimumIntervalSec(uint16_t seconds) noexcept;

  /** @brief Applies a master's policy change, clamped, and reports the result. */
  HAPSetPolicyResponse setPolicy(const HAPSetPolicyRequest& request) noexcept;

  /**
   * @brief True when the interval has elapsed, or a value has moved far enough.
   *
   * Also true for a port that has never been reported, so the first report
   * after a bind goes out at once rather than an interval later.
   */
  bool isDue() const noexcept;

  /** @brief Every out port, whatever made the report due. */
  void fillReport(HAPReport& report) const noexcept;

  /**
   * @brief Records what was just sent, and when.
   *
   * Call only when the frame actually went out. Marking a report that failed to
   * transmit would start the next interval from a reading nobody received.
   */
  void markReported() noexcept;

  /** @brief Milliseconds until the soonest interval expires; 0 when due now. */
  uint32_t nextDueInMs() const noexcept;

  size_t policyCount() const noexcept;

 private:
  struct Policy {
    uint8_t classId = 0;
    uint8_t instanceId = 0;
    uint8_t portId = 0;

    uint16_t intervalSec = 0;
    HValue deadband;

    /** What was in the last report, to measure a deadband against. */
    HValue lastValue;
    uint32_t lastSentMs = 0;
    bool everSent = false;
  };

  Policy* find(uint8_t classId, uint8_t instanceId, uint8_t portId) noexcept;

  /** True when `now` differs from `then` by more than the deadband allows. */
  static bool moved(const HValue& then, const HValue& now,
                    const HValue& deadband) noexcept;

  bool isDue(const Policy& policy, uint32_t now) const noexcept;

  const HAPNode& node_;
  uint16_t minimumIntervalSec_ = 0;

  etl::vector<Policy, HAP_MAX_REPORT_ENTRIES> policies_;
};
