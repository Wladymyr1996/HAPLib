#pragma once

#include <HAPClasses/HAPClasses.hpp>
#include <HAPMessages/HAPMessageParts.hpp>

#include <etl/vector.h>

/** Ports one instance may have. The widest standard class, Regulator, has three. */
#ifndef HAP_MAX_PORTS_PER_INSTANCE
#define HAP_MAX_PORTS_PER_INSTANCE 4
#endif

/**
 * @brief One live class instance on this node: its name, and its ports' values.
 *
 * A `HAPInstanceDescriptor` says what an instance IS, on the wire. This is what
 * it currently reads, in memory - the thermometer that has measured 21.5, the
 * regulator whose Measured input was fed by a link four seconds ago.
 *
 * Its ports come from the class table rather than from the caller: configure()
 * looks the class up and creates exactly the ports Docs/Classes/ says it has.
 * An instance cannot therefore have a port its class does not, which is what
 * lets a master wire two nodes together knowing only their class identifiers.
 *
 * ## Everything starts Null, and Null is not zero
 * A port that has never been given a value reads Null - "no reading" - and a
 * report carries that faithfully. A thermometer whose sensor has not answered
 * yet must not appear to be reading 0 °C, which is how a heating system runs all
 * night.
 */
class HAPInstance {
 public:
  /**
   * @brief Makes this a live instance of a class.
   * @param classId Must be a class this build knows; private classes cannot be
   *        configured this way, since nothing here knows their ports.
   * @param instanceId Unique within the NODE, not within the class.
   * @return false when the class is unknown or it has more ports than
   *         HAP_MAX_PORTS_PER_INSTANCE.
   */
  bool configure(uint8_t classId, uint8_t instanceId, const HAPName& name) noexcept;

  bool isConfigured() const noexcept;

  uint8_t classId() const noexcept;
  uint8_t instanceId() const noexcept;

  const HAPName& name() const noexcept;

  /** @brief Renames it. The name is part of the descriptor, so the revision moves. */
  void setName(const HAPName& newName) noexcept;

  /** @return nullptr before configure(). */
  const HAPClassSpec* spec() const noexcept;

  /**
   * @brief Sets what an OUT port reads - the application's own measurement.
   *
   * This is how a driver hands a reading to the protocol. It is not a write in
   * the protocol's sense: nothing arrives from the network here, and nothing
   * is refused for being read-only.
   *
   * @return false when there is no such out port, or the value's type is not
   *         what the class declares. A hygrometer publishing a Bool is a bug in
   *         the application, and a silent one if this returned true.
   */
  bool publish(uint8_t portId, const HValue& value) noexcept;

  /** @brief What an out port currently reads; Null when there is no such port. */
  const HValue& read(uint8_t portId) const noexcept;

  /**
   * @brief Applies a value to an IN port - from a master, or from a link.
   * @return Ok, NoSuchPort, NotWritable (the port exists but faces outward), or
   *         BadValue (the wrong type for what the class declares).
   */
  HAPResult write(uint8_t portId, const HValue& value) noexcept;

  /** @brief What an in port was last given; Null when never fed. */
  const HValue& input(uint8_t portId) const noexcept;

  /**
   * @brief When an in port was last fed, in millis().
   *
   * What staleness is measured from: an input silent for three intervals must
   * fall back to its class's failsafe rather than hold the last value forever.
   * 0 when it has never been fed.
   */
  uint32_t inputUpdatedAtMs(uint8_t portId) const noexcept;

  /** @brief How long since an in port was fed. UINT32_MAX when never. */
  uint32_t inputAgeMs(uint8_t portId) const noexcept;

  /**
   * @brief True when an in port has gone quiet for longer than it should.
   *
   * The other half of what a link promises. A link cannot guarantee delivery,
   * only attempt it - so everything that matters hangs off the destination
   * noticing that it has stopped being fed. One timer catches all of it: a dead
   * source, a broken hop, a link whose lowest common ancestor lost power, and a
   * node that was simply never wired up.
   *
   * A port that has never been fed is stale from the start, which is correct: a
   * regulator that has never had a temperature must run its failsafe, not wait
   * indefinitely for a first reading.
   *
   * @return false for a port that is not an input at all - a question about a
   *         thermometer's output cannot have a useful answer here.
   */
  bool isInputStale(uint8_t portId, uint32_t timeoutMs) const noexcept;

  /** @brief This instance as it appears in a BindAnnounce or a Describe. */
  HAPInstanceDescriptor describe() const noexcept;

 private:
  struct Port {
    uint8_t portId = 0;
    HAPPortDirection direction = HAPPortDirection::Out;
    HAPValueType valueType = HAPValueType::Null;
    HValue value;
    uint32_t updatedAtMs = 0;
  };

  Port* find(HAPPortDirection direction, uint8_t portId) noexcept;
  const Port* find(HAPPortDirection direction, uint8_t portId) const noexcept;

  static bool typeMatches(HAPValueType expected, const HValue& value) noexcept;

  uint8_t classId_ = 0;
  uint8_t instanceId_ = 0;
  bool configured_ = false;
  HAPName name_;

  etl::vector<Port, HAP_MAX_PORTS_PER_INSTANCE> ports_;
};
