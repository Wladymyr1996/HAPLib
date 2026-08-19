#include <HAPInstance/HAPInstance.hpp>

#include <HSystemUtils/HSystemUtils.hpp>

namespace {

/** Returned for every port that does not exist, so nothing hands out a pointer. */
const HValue kNoReading;

}  // namespace

bool HAPInstance::configure(uint8_t classId, uint8_t instanceId,
                            const HAPName& name) noexcept {
  const HAPClassSpec* classSpec = HAPClasses::find(classId);
  if (classSpec == nullptr) {
    return false;
  }

  if (classSpec->portCount > ports_.max_size()) {
    return false;
  }

  ports_.clear();

  // The ports come from the class, not from the caller. An instance therefore
  // cannot have a port its class does not - which is what lets a master wire
  // two nodes together knowing nothing but their class identifiers.
  for (uint8_t i = 0; i < classSpec->portCount; ++i) {
    const HAPPortSpec& portSpec = classSpec->ports[i];

    Port port;
    port.portId = portSpec.portId;
    port.direction = portSpec.direction;
    port.valueType = portSpec.valueType;
    // Its value stays Null until something measures or feeds it. Null is "no
    // reading", and a report must be able to say so.
    ports_.push_back(port);
  }

  classId_ = classId;
  instanceId_ = instanceId;
  name_ = name;
  configured_ = true;
  return true;
}

bool HAPInstance::isConfigured() const noexcept {
  return configured_;
}

uint8_t HAPInstance::classId() const noexcept {
  return classId_;
}

uint8_t HAPInstance::instanceId() const noexcept {
  return instanceId_;
}

const HAPName& HAPInstance::name() const noexcept {
  return name_;
}

void HAPInstance::setName(const HAPName& newName) noexcept {
  name_ = newName;
}

const HAPClassSpec* HAPInstance::spec() const noexcept {
  return configured_ ? HAPClasses::find(classId_) : nullptr;
}

HAPInstance::Port* HAPInstance::find(HAPPortDirection direction,
                                     uint8_t portId) noexcept {
  for (Port& port : ports_) {
    if (port.direction == direction && port.portId == portId) {
      return &port;
    }
  }

  return nullptr;
}

const HAPInstance::Port* HAPInstance::find(HAPPortDirection direction,
                                           uint8_t portId) const noexcept {
  for (const Port& port : ports_) {
    if (port.direction == direction && port.portId == portId) {
      return &port;
    }
  }

  return nullptr;
}

bool HAPInstance::typeMatches(HAPValueType expected,
                              const HValue& value) noexcept {
  // Null is always allowed: it is how a port says it has no reading, whatever
  // type it carries when it does have one.
  if (value.isNull()) {
    return true;
  }

  switch (expected) {
    case HAPValueType::Bool:
      return value.isBool();
    case HAPValueType::Int:
      return value.isInt();
    case HAPValueType::Float:
      return value.isFloat();
    case HAPValueType::String:
      return value.isString();
    case HAPValueType::Null:
      return false;
  }

  return false;
}

bool HAPInstance::publish(uint8_t portId, const HValue& value) noexcept {
  Port* port = find(HAPPortDirection::Out, portId);
  if (port == nullptr) {
    return false;
  }

  if (!typeMatches(port->valueType, value)) {
    return false;
  }

  HAPAssign(port->value, value);
  port->updatedAtMs = HSystemUtils::millis();
  return true;
}

const HValue& HAPInstance::read(uint8_t portId) const noexcept {
  const Port* port = find(HAPPortDirection::Out, portId);
  return port == nullptr ? kNoReading : port->value;
}

HAPResult HAPInstance::write(uint8_t portId, const HValue& value) noexcept {
  Port* port = find(HAPPortDirection::In, portId);

  if (port == nullptr) {
    // Telling the two apart is worth the extra lookup: "this instance has no
    // inputs at all" and "it has inputs, but not that one" are different
    // mistakes at the far end.
    return find(HAPPortDirection::Out, portId) != nullptr ? HAPResult::NotWritable
                                                          : HAPResult::NoSuchPort;
  }

  if (!typeMatches(port->valueType, value)) {
    return HAPResult::BadValue;
  }

  HAPAssign(port->value, value);
  port->updatedAtMs = HSystemUtils::millis();
  return HAPResult::Ok;
}

const HValue& HAPInstance::input(uint8_t portId) const noexcept {
  const Port* port = find(HAPPortDirection::In, portId);
  return port == nullptr ? kNoReading : port->value;
}

uint32_t HAPInstance::inputUpdatedAtMs(uint8_t portId) const noexcept {
  const Port* port = find(HAPPortDirection::In, portId);
  return port == nullptr ? 0 : port->updatedAtMs;
}

uint32_t HAPInstance::inputAgeMs(uint8_t portId) const noexcept {
  const Port* port = find(HAPPortDirection::In, portId);

  if (port == nullptr || port->value.isNull()) {
    return UINT32_MAX;
  }

  return HSystemUtils::millis() - port->updatedAtMs;
}

bool HAPInstance::isInputStale(uint8_t portId, uint32_t timeoutMs) const noexcept {
  if (find(HAPPortDirection::In, portId) == nullptr) {
    return false;
  }

  // Never fed is stale from the start. A regulator that has never had a
  // temperature must run its failsafe rather than wait indefinitely for a first
  // reading that may never come.
  return inputAgeMs(portId) >= timeoutMs;
}

HAPInstanceDescriptor HAPInstance::describe() const noexcept {
  HAPInstanceDescriptor descriptor;
  descriptor.classId = classId_;
  descriptor.instanceId = instanceId_;
  descriptor.name = name_;

  const HAPClassSpec* classSpec = spec();
  if (classSpec == nullptr) {
    return descriptor;
  }

  if (classSpec->countPorts(HAPPortDirection::In) > 0) {
    descriptor.flags |= HAPInstanceFlags::Writable;
  }

  // The type a master should expect in a report. An instance that only takes
  // input - nothing standard does yet - describes its input's type instead, so
  // the field is never meaninglessly Null.
  const Port* primary = find(HAPPortDirection::Out, 0);
  if (primary == nullptr) {
    primary = find(HAPPortDirection::In, 0);
  }

  if (primary != nullptr) {
    descriptor.valueType = static_cast<uint8_t>(primary->valueType);
  }

  return descriptor;
}
