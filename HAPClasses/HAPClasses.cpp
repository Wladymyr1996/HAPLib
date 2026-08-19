#include <HAPClasses/HAPClasses.hpp>

namespace {

// Every table below is the Ports section of the matching document in
// Docs/Classes/. When one changes the other must, and the host tests check the
// pair that Docs/Links.md works through.

constexpr HAPPortSpec kThermometerPorts[] = {
    {0, HAPPortDirection::Out, HAPKind::Temperature, HAPValueType::Float,
     "Temperature"}};

constexpr HAPPortSpec kHygrometerPorts[] = {
    {0, HAPPortDirection::Out, HAPKind::Humidity, HAPValueType::Float,
     "Humidity"}};

constexpr HAPPortSpec kBarometerPorts[] = {
    {0, HAPPortDirection::Out, HAPKind::Pressure, HAPValueType::Float,
     "Pressure"}};

constexpr HAPPortSpec kSwitchPorts[] = {
    {0, HAPPortDirection::Out, HAPKind::OnOff, HAPValueType::Bool, "State"}};

// Out 0 and in 0 are the same lamp seen from two sides: what it reports, and
// what it is told. See the header on why they share a number.
constexpr HAPPortSpec kLampPorts[] = {
    {0, HAPPortDirection::Out, HAPKind::OnOff, HAPValueType::Bool, "State"},
    {0, HAPPortDirection::In, HAPKind::OnOff, HAPValueType::Bool, "State"}};

constexpr HAPPortSpec kDoorPorts[] = {
    {0, HAPPortDirection::Out, HAPKind::OnOff, HAPValueType::Bool, "Open"}};

constexpr HAPPortSpec kBatteryPorts[] = {
    {0, HAPPortDirection::Out, HAPKind::Voltage, HAPValueType::Float,
     "Voltage"}};

// The class Docs/Links.md wires up: two inputs and an output, and the reason
// the control-function range exists.
constexpr HAPPortSpec kRegulatorPorts[] = {
    {0, HAPPortDirection::In, HAPKind::Temperature, HAPValueType::Float,
     "Measured"},
    {1, HAPPortDirection::In, HAPKind::Temperature, HAPValueType::Float,
     "Setpoint"},
    {0, HAPPortDirection::Out, HAPKind::Ratio, HAPValueType::Float, "Demand"}};

template <size_t N>
constexpr HAPClassSpec makeClass(HAPClassId classId, const char* name,
                                 const HAPPortSpec (&ports)[N]) noexcept {
  return HAPClassSpec{static_cast<uint8_t>(classId), name, ports,
                      static_cast<uint8_t>(N)};
}

constexpr HAPClassSpec kClasses[] = {
    makeClass(HAPClassId::Thermometer, "Thermometer", kThermometerPorts),
    makeClass(HAPClassId::Hygrometer, "Hygrometer", kHygrometerPorts),
    makeClass(HAPClassId::Barometer, "Barometer", kBarometerPorts),
    makeClass(HAPClassId::Switch, "Switch", kSwitchPorts),
    makeClass(HAPClassId::Lamp, "Lamp", kLampPorts),
    makeClass(HAPClassId::Door, "Door", kDoorPorts),
    makeClass(HAPClassId::Battery, "Battery", kBatteryPorts),
    makeClass(HAPClassId::Regulator, "Regulator", kRegulatorPorts)};

constexpr size_t kClassCount = sizeof(kClasses) / sizeof(kClasses[0]);

}  // namespace

const HAPPortSpec* HAPClassSpec::find(HAPPortDirection direction,
                                      uint8_t portId) const noexcept {
  for (uint8_t i = 0; i < portCount; ++i) {
    if (ports[i].direction == direction && ports[i].portId == portId) {
      return &ports[i];
    }
  }

  return nullptr;
}

uint8_t HAPClassSpec::countPorts(HAPPortDirection direction) const noexcept {
  uint8_t total = 0;

  for (uint8_t i = 0; i < portCount; ++i) {
    if (ports[i].direction == direction) {
      ++total;
    }
  }

  return total;
}

namespace HAPClasses {

const HAPClassSpec* find(uint8_t classId) noexcept {
  for (const HAPClassSpec& candidate : kClasses) {
    if (candidate.classId == classId) {
      return &candidate;
    }
  }

  return nullptr;
}

const HAPPortSpec* port(uint8_t classId, HAPPortDirection direction,
                        uint8_t portId) noexcept {
  const HAPClassSpec* spec = find(classId);
  return spec == nullptr ? nullptr : spec->find(direction, portId);
}

HAPValueType valueType(uint8_t classId, HAPPortDirection direction,
                       uint8_t portId) noexcept {
  const HAPPortSpec* spec = port(classId, direction, portId);
  return spec == nullptr ? HAPValueType::Null : spec->valueType;
}

bool isWritable(uint8_t classId) noexcept {
  const HAPClassSpec* spec = find(classId);
  return spec != nullptr && spec->countPorts(HAPPortDirection::In) > 0;
}

HAPResult validateLink(uint8_t sourceClassId, uint8_t sourcePortId,
                       uint8_t destinationClassId,
                       uint8_t destinationPortId) noexcept {
  const HAPClassSpec* source = find(sourceClassId);
  const HAPClassSpec* destination = find(destinationClassId);

  if (source == nullptr || destination == nullptr) {
    return HAPResult::NoSuchClass;
  }

  const HAPPortSpec* out = source->find(HAPPortDirection::Out, sourcePortId);
  if (out == nullptr) {
    return HAPResult::NoSuchPort;
  }

  const HAPPortSpec* in =
      destination->find(HAPPortDirection::In, destinationPortId);
  if (in == nullptr) {
    // The destination class may exist and have no inputs at all - wiring
    // something into a thermometer is the commonest way to reach here.
    return destination->countPorts(HAPPortDirection::In) == 0
               ? HAPResult::NotWritable
               : HAPResult::NoSuchPort;
  }

  // The check the whole quantity-kind idea exists for: both ends of this are
  // floats in a plausible range, and only the kind says one is degrees and the
  // other is percent.
  if (out->kind != in->kind) {
    return HAPResult::TypeMismatch;
  }

  return HAPResult::Ok;
}

size_t count() noexcept {
  return kClassCount;
}

const HAPClassSpec* at(size_t position) noexcept {
  return position < kClassCount ? &kClasses[position] : nullptr;
}

}  // namespace HAPClasses
