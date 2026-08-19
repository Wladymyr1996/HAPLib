#include <HAPReporter/HAPReporter.hpp>

#include <HAPClasses/HAPClasses.hpp>
#include <HSystemUtils/HSystemUtils.hpp>

#include <cmath>

HAPReporter::HAPReporter(const HAPNode& node) noexcept : node_(node) {}

void HAPReporter::begin(uint16_t defaultIntervalSec,
                        const HValue& defaultDeadband) noexcept {
  policies_.clear();

  for (size_t i = 0; i < node_.instanceCount(); ++i) {
    const HAPInstance* instance = node_.instanceAt(i);
    if (instance == nullptr) {
      continue;
    }

    const HAPClassSpec* spec = instance->spec();
    if (spec == nullptr) {
      continue;
    }

    for (uint8_t p = 0; p < spec->portCount; ++p) {
      const HAPPortSpec& port = spec->ports[p];

      if (port.direction != HAPPortDirection::Out || policies_.full()) {
        continue;
      }

      Policy policy;
      policy.classId = instance->classId();
      policy.instanceId = instance->instanceId();
      policy.portId = port.portId;
      policy.intervalSec = defaultIntervalSec;

      // A deadband only means anything for a value you can subtract. Handing a
      // lamp one would be a policy that never fires and never says why.
      if (port.valueType == HAPValueType::Float) {
        HAPAssign(policy.deadband, defaultDeadband);
      }

      policies_.push_back(policy);
    }
  }
}

void HAPReporter::setMinimumIntervalSec(uint16_t seconds) noexcept {
  minimumIntervalSec_ = seconds;
}

HAPReporter::Policy* HAPReporter::find(uint8_t classId, uint8_t instanceId,
                                       uint8_t portId) noexcept {
  for (Policy& policy : policies_) {
    if (policy.classId == classId && policy.instanceId == instanceId &&
        policy.portId == portId) {
      return &policy;
    }
  }

  return nullptr;
}

HAPSetPolicyResponse HAPReporter::setPolicy(
    const HAPSetPolicyRequest& request) noexcept {
  Policy* policy = find(request.classId, request.instanceId, request.portId);

  if (policy == nullptr) {
    return HAPSetPolicyResponse(HAPResult::NoSuchPort, request.classId,
                                request.instanceId, request.portId, 0, HValue());
  }

  // Clamped rather than refused. A master asking a battery sensor for a reading
  // every second gets an answer saying what it will actually get.
  uint16_t interval = request.intervalSec;
  if (interval != 0 && interval < minimumIntervalSec_) {
    interval = minimumIntervalSec_;
  }

  policy->intervalSec = interval;
  HAPAssign(policy->deadband, request.deadband);

  return HAPSetPolicyResponse(HAPResult::Ok, policy->classId, policy->instanceId,
                              policy->portId, policy->intervalSec,
                              policy->deadband);
}

bool HAPReporter::moved(const HValue& then, const HValue& now,
                        const HValue& deadband) noexcept {
  // A reading appearing or disappearing is always news. A sensor that stopped
  // answering must not be hidden by a deadband, because Null is not a small
  // change from 21.5 - it is the absence of a reading.
  if (then.isNull() != now.isNull()) {
    return true;
  }

  if (now.isNull()) {
    return false;
  }

  if (deadband.isNull() || !now.isFloat() || !then.isFloat()) {
    // Nothing to subtract: any change at all counts.
    return then != now;
  }

  return std::fabs(now.asFloat() - then.asFloat()) > deadband.asFloat();
}

bool HAPReporter::isDue(const Policy& policy, uint32_t now) const noexcept {
  // Never reported: say so at once rather than an interval after binding.
  if (!policy.everSent) {
    return true;
  }

  if (policy.intervalSec != 0) {
    const uint32_t elapsed = now - policy.lastSentMs;
    if (elapsed >= static_cast<uint32_t>(policy.intervalSec) * 1000u) {
      return true;
    }
  }

  const HAPInstance* instance = node_.instance(policy.classId, policy.instanceId);
  if (instance == nullptr) {
    return false;
  }

  return moved(policy.lastValue, instance->read(policy.portId), policy.deadband);
}

bool HAPReporter::isDue() const noexcept {
  const uint32_t now = HSystemUtils::millis();

  for (const Policy& policy : policies_) {
    if (isDue(policy, now)) {
      return true;
    }
  }

  return false;
}

uint32_t HAPReporter::nextDueInMs() const noexcept {
  const uint32_t now = HSystemUtils::millis();
  uint32_t soonest = UINT32_MAX;

  for (const Policy& policy : policies_) {
    if (isDue(policy, now)) {
      return 0;
    }

    if (policy.intervalSec == 0) {
      continue;  // On change only: there is no deadline to count down to.
    }

    const uint32_t period = static_cast<uint32_t>(policy.intervalSec) * 1000u;
    const uint32_t elapsed = now - policy.lastSentMs;
    const uint32_t remaining = elapsed >= period ? 0 : period - elapsed;

    if (remaining < soonest) {
      soonest = remaining;
    }
  }

  return soonest;
}

void HAPReporter::fillReport(HAPReport& report) const noexcept {
  // Every out port, not only the ones that are due: the deadband decided
  // whether to transmit, and having decided, a consistent snapshot is worth
  // more to a master than a scattering of single values.
  node_.fillReport(report);
}

void HAPReporter::markReported() noexcept {
  const uint32_t now = HSystemUtils::millis();

  for (Policy& policy : policies_) {
    const HAPInstance* instance =
        node_.instance(policy.classId, policy.instanceId);

    if (instance != nullptr) {
      HAPAssign(policy.lastValue, instance->read(policy.portId));
    }

    policy.lastSentMs = now;
    policy.everSent = true;
  }
}

size_t HAPReporter::policyCount() const noexcept {
  return policies_.size();
}
