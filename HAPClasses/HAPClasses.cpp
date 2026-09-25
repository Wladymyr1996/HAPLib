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

// A door is the first class that is genuinely a MACHINE rather than a reading:
// it takes time to move, it can be interrupted, and where it is between the two
// ends is a thing somebody wants to see.
//
// Out 0 is unchanged and stays a plain Bool, because that is what every master
// already written against this class reads and because "is it open" is the
// question most things want answered. Out 1 carries what a bool cannot: the six
// states in Docs/Classes/DoorClass.md, as Text, so `opening` and `stopped` are
// visible rather than collapsed into `false`.
//
// The three in ports are separate rather than one Text command, and the reason
// is links. A link matches on KIND, so three OnOff inputs can be driven by any
// button, contact or logic output in the ecosystem; a single Text "Command" port
// could only ever be written by a master that knew the vocabulary, which would
// put a door outside the reach of the very wiring this class exists for.
constexpr HAPPortSpec kDoorPorts[] = {
    {0, HAPPortDirection::Out, HAPKind::OnOff, HAPValueType::Bool, "Open"},
    {1, HAPPortDirection::Out, HAPKind::Text, HAPValueType::String, "State"},
    {0, HAPPortDirection::In, HAPKind::OnOff, HAPValueType::Bool, "Open"},
    {1, HAPPortDirection::In, HAPKind::OnOff, HAPValueType::Bool, "Close"},
    {2, HAPPortDirection::In, HAPKind::OnOff, HAPValueType::Bool, "Stop"}};

// The same shape as a lamp, on purpose, and still a class of its own. A gateway
// shows "relay" rather than "lamp" for a contact that may be switching a pump,
// and the class can grow - a port saying who drives it, say - without changing
// what a lamp is. See Docs/Classes/RelayClass.md.
constexpr HAPPortSpec kRelayPorts[] = {
    {0, HAPPortDirection::Out, HAPKind::OnOff, HAPValueType::Bool, "State"},
    {0, HAPPortDirection::In, HAPKind::OnOff, HAPValueType::Bool, "State"}};

// Two outputs, and the first of them is the one that matters: SoC is what a
// user acts on, so it is port 0 and it is what an instance's descriptor
// declares. Voltage rides along on port 1 because a gauge knows it anyway, and
// a terminal voltage sagging under load says things a percentage cannot - a
// cell at 0.5 SoC that sags to 3.1 V under transmit has days rather than weeks.
constexpr HAPPortSpec kBatteryStatePorts[] = {
    {0, HAPPortDirection::Out, HAPKind::Ratio, HAPValueType::Float, "SoC"},
    {1, HAPPortDirection::Out, HAPKind::Voltage, HAPValueType::Float,
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
    makeClass(HAPClassId::Relay, "Relay", kRelayPorts),
    makeClass(HAPClassId::BatteryState, "BatteryState", kBatteryStatePorts),
    makeClass(HAPClassId::Regulator, "Regulator", kRegulatorPorts)};

constexpr size_t kClassCount = sizeof(kClasses) / sizeof(kClasses[0]);

/**
 * @brief No class may declare more ports than an instance can hold.
 *
 * The coupling between this table and HAPInstance's storage, made a build error
 * rather than a runtime one. Without it, adding a sixth port to a class compiles
 * perfectly and then fails inside HAPInstance::configure() on every device that
 * tries to be one - which reads as "this node refuses to have a door" and sends
 * somebody hunting through the wrong layer entirely.
 *
 * Raise HAP_MAX_PORTS_PER_INSTANCE in HAP.h if a class genuinely needs more,
 * remembering that every instance on every node pays for it.
 */
constexpr uint8_t widestClass() noexcept {
  uint8_t widest = 0;

  for (const HAPClassSpec& candidate : kClasses) {
    if (candidate.portCount > widest) {
      widest = candidate.portCount;
    }
  }

  return widest;
}

static_assert(widestClass() <= HAP_MAX_PORTS_PER_INSTANCE,
              "a class in this table declares more ports than HAPInstance can "
              "hold - raise HAP_MAX_PORTS_PER_INSTANCE in HAP.h");

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
