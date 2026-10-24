#pragma once

#include <HAP.h>
#include <HAPCodec/HAPCodec.hpp>
#include <HAPNodeId/HAPNodeId.hpp>
#include <HValue/HValue.hpp>

/**
 * @file HAPMessageParts.hpp
 * @brief The three structures that turn up inside more than one message.
 *
 * Every message payload in Docs/Protocol.md is built from primitives plus these:
 * a description of an instance, a value with its address, and a reference to one
 * port anywhere below a node.
 *
 * ## A warning about anything holding an HValue
 * HValue's `operator=` COERCES into the destination's existing type rather than
 * replacing it - assigning a Float into a Null HValue leaves it Null (see
 * HValue's own documentation, where this is deliberate). Any struct with an
 * HValue member inherits that from its compiler-generated assignment operator.
 *
 * So these types are CONSTRUCTED, never assigned. Their decoders build a fresh
 * object and hand it back by value, and containers of them are filled with
 * push_back, which copy-constructs. setValue() exists for the one place that
 * genuinely has to replace a value in place, and is the only correct way to do
 * it.
 */

/**
 * @brief One class instance, as it appears in BindAnnounce and DescribeResponse.
 *
 * Four bytes and a name. Ports are NOT here: they belong to the class and are
 * documented in Docs/Classes/, so a descriptor names what a thing is and the
 * class says what it can do.
 */
struct HAPInstanceDescriptor {
  /** A HAPClassId, kept as a byte - the wire may carry one this build lacks. */
  uint8_t classId = 0;

  /** Unique within the NODE, not within the class. 0..HAP_MAX_INSTANCES-1. */
  uint8_t instanceId = 0;

  /** HAPInstanceFlags bits. */
  uint8_t flags = HAPInstanceFlags::None;

  /** The HAPValueType this instance's primary port reports. */
  uint8_t valueType = 0;

  /** The user's label. Editable; part of the descriptor revision. */
  HAPName name;

  void encode(HAPWriter& writer) const noexcept;

  /** @return false when the reader ran out; the object is then meaningless. */
  bool decode(HAPReader& reader) noexcept;

  /** Bytes this descriptor occupies: four plus a length-prefixed name. */
  size_t encodedSize() const noexcept;
};

/**
 * @brief One value and the port it came from or is going to.
 *
 * The unit a Report is made of, and what a link carries. See the file note above
 * about assignment.
 */
struct HAPValueEntry {
  HAPValueEntry() noexcept = default;

  HAPValueEntry(uint8_t classId, uint8_t instanceId, uint8_t portId,
                const HValue& value) noexcept;

  uint8_t classId = 0;
  uint8_t instanceId = 0;

  /** 0 is the instance's primary value; see Docs/Links.md. */
  uint8_t portId = 0;

  /** Null means NO READING - never zero. */
  HValue value;

  /**
   * @brief Replaces the value, type and all.
   *
   * Plain assignment cannot do this: it would coerce the new value into the old
   * one's type, so a fresh Float landing on a Null entry would read back Null.
   * This destroys and reconstructs instead.
   */
  void setValue(const HValue& newValue) noexcept;

  void encode(HAPWriter& writer) const noexcept;

  /** @brief Reads one entry. Check reader.ok() afterwards. */
  static HAPValueEntry decode(HAPReader& reader) noexcept;
};

/**
 * @brief One port on a node, named by the node's id.
 *
 * Used only by links. A link lives at the lowest common ancestor of its two
 * endpoints, so a zero node means "the node holding the link" - what an empty
 * path used to mean. Eleven bytes.
 */
struct HAPPortRef {
  /** The node the port is on; zero means the node holding this reference. */
  HAPNodeId node;

  uint8_t classId = 0;
  uint8_t instanceId = 0;
  uint8_t portId = 0;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;

  bool operator==(const HAPPortRef& other) const noexcept;
  bool operator!=(const HAPPortRef& other) const noexcept;

  /** Fixed: a node id and three identifiers. */
  static constexpr size_t kEncodedSize = HAP_NODE_ID_LEN + 3;
};
