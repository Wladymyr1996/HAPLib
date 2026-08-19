#pragma once

#include <HAP.h>

#include <cstddef>

/**
 * @file HAPClasses.hpp
 * @brief Docs/Classes/ as code: what every standard class can do.
 *
 * A class identifier implies its ports - how many, which way they face, what
 * quantity they carry, what they are called. None of that is ever transmitted:
 * a descriptor names what a thing IS, and this table says what that means. Which
 * is why a node's descriptor is small, and why port names are not user data.
 *
 * ## Direction is implied by position, so the two id spaces are separate
 * A lamp's state is both readable and writable, and is **out port 0 and in port
 * 0** - two ports, same number, different directions. That is unambiguous
 * everywhere it matters, because nothing on the wire ever names a port without
 * saying which way it faces:
 *
 * | Message | Names |
 * | ------- | ----- |
 * | Report, ReadRequest, ReadResponse | **out** ports |
 * | WriteRequest, WriteResponse | **in** ports |
 * | SetLinkRequest | source is **out**, destination is **in** |
 *
 * The alternative - one shared numbering, where a lamp reports on port 0 and is
 * commanded on port 1 - costs a number nobody can remember to save an ambiguity
 * that does not exist.
 *
 * ## Private classes
 * 0x80 and above are nobody else's business, and are not in this table. A node
 * carrying one must enumerate its ports in DescribeResponse or no other vendor's
 * software can wire it up.
 */

/** @brief Which way a port faces. */
enum class HAPPortDirection : uint8_t {
  In,  ///< Consumed: what a WriteRequest sets, and what a link feeds.
  Out  ///< Produced: what a Report carries, and what a link takes from.
};

/** @brief One port of one class. */
struct HAPPortSpec {
  uint8_t portId;
  HAPPortDirection direction;

  /** What it MEANS - the thing a link matches on. */
  HAPKind kind;

  /** How it is encoded, which follows from the kind. */
  HAPValueType valueType;

  /** From the class documentation, never editable. Never nullptr. */
  const char* name;
};

/** @brief One class, and the ports it implies. */
struct HAPClassSpec {
  uint8_t classId;

  /** In English, for logs; a user interface uses its own dictionary. */
  const char* name;

  const HAPPortSpec* ports;
  uint8_t portCount;

  const HAPPortSpec* find(HAPPortDirection direction, uint8_t portId) const noexcept;
  uint8_t countPorts(HAPPortDirection direction) const noexcept;
};

namespace HAPClasses {

/** @return nullptr for a class this build does not know, private ones included. */
const HAPClassSpec* find(uint8_t classId) noexcept;

/** @return nullptr when the class or the port is unknown. */
const HAPPortSpec* port(uint8_t classId, HAPPortDirection direction,
                        uint8_t portId) noexcept;

/** @brief The type a class's port carries, for building a descriptor. */
HAPValueType valueType(uint8_t classId, HAPPortDirection direction,
                       uint8_t portId) noexcept;

/** @brief True when this class has a port that can be written. */
bool isWritable(uint8_t classId) noexcept;

/**
 * @brief Whether one port may be wired to another.
 *
 * @return Ok, or why not:
 *         - NoSuchClass / NoSuchPort - one end does not exist;
 *         - NotWritable - the destination is not an in port;
 *         - TypeMismatch - the kinds differ. This is the check that stops a
 *           hygrometer's 44.0 being wired into a heating setpoint, which a
 *           comparison of encodings would happily allow since both are floats.
 */
HAPResult validateLink(uint8_t sourceClassId, uint8_t sourcePortId,
                       uint8_t destinationClassId,
                       uint8_t destinationPortId) noexcept;

/** @brief How many classes this build knows, for iteration. */
size_t count() noexcept;

/** @brief For iteration; nullptr past the end. */
const HAPClassSpec* at(size_t position) noexcept;

}  // namespace HAPClasses
