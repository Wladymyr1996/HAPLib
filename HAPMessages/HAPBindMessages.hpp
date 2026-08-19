#pragma once

#include <HAPMessages/HAPMessageParts.hpp>

#include <etl/vector.h>

/**
 * @file HAPBindMessages.hpp
 * @brief The four messages of the bind handshake. Docs/Protocol.md section 6.
 *
 * A press on each device, and nothing happens without both. These are the only
 * messages ever sent unencrypted between two nodes that do not yet share a key -
 * which is also the protocol's one honest weak point, documented as such.
 */

/**
 * @brief "I am unbound, and this is what I am." Broadcast by a slave.
 *
 * ## Paging is the caller's job
 * encode() writes every descriptor this object holds and fails if they do not
 * fit - it does not decide what to leave out. Which instances belong on which
 * page is a question about a NODE, not about a message, so it is answered by
 * HAPNode (phase 3), which fills pageIndex and pageCount accordingly.
 *
 * The node name repeats on every page so that a page can be parsed alone.
 */
struct HAPBindAnnounce {
  HAPDeviceType deviceType = HAPDeviceType::Sensor;

  /** HAPCaps bits. BatteryPowered is what tells a master to queue rather than send. */
  uint8_t capabilities = HAPCaps::None;

  /** How often this node intends to report. 0 means only on change. */
  uint16_t reportIntervalSec = 0;

  /** CRC-16 of the whole descriptor, names included. */
  uint16_t descriptorRev = 0;

  /** Instances across ALL pages, which may exceed the count carried here. */
  uint8_t instanceCount = 0;

  uint8_t pageIndex = 0;
  uint8_t pageCount = 1;

  HAPName nodeName;

  etl::vector<HAPInstanceDescriptor, HAP_MAX_INSTANCES> instances;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;

  /** Bytes before the descriptors: seven fixed plus the node's name. */
  static constexpr size_t kFixedSize = 9;
};

/**
 * @brief The master's answer: an index, a key, and where to find the network.
 *
 * Unicast to the announcing MAC, and unencrypted because it carries the key that
 * makes everything after it encrypted.
 */
struct HAPBindAccept {
  /** 1..HAP_MAX_CHILDREN, chosen by the master. */
  uint8_t childIndex = 0;

  /** The Wi-Fi channel the whole network runs on. */
  uint8_t channel = 0;

  /** The ESP-NOW local master key for this one parent-child link. */
  uint8_t linkKey[HAP_KEY_LEN] = {};

  /** For the slave's screen, so a user can see what adopted it. */
  HAPName masterName;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;
};

/**
 * @brief "Stored." Sent encrypted with the key just received.
 *
 * That it decrypts at all is the proof the key arrived intact, which is why this
 * message needs nothing in it but the revision the master is about to cache.
 */
struct HAPBindConfirm {
  uint16_t descriptorRev = 0;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;
};

/**
 * @brief A parent telling the root about a child that just bound or came back.
 *
 * Without it, a new battery sensor goes unnoticed until its first report - up to
 * a minute later. The MAC is what lets a root recognise a device that moved
 * elsewhere in the tree and rewrite the links that referenced it.
 */
struct HAPChildAttached {
  /** Relative to the sender; the root appends it to the frame's source path. */
  uint8_t childIndex = 0;

  HAPDeviceType deviceType = HAPDeviceType::Sensor;
  uint8_t capabilities = HAPCaps::None;

  /**
   * How often the child intends to report, from its announcement.
   *
   * The root cannot learn this any other way - a BindAnnounce is only ever
   * heard by the node that adopts it - and without it there is nothing to
   * measure a silence against. It is what makes "offline after three missed
   * reports" mean anything more than one hop down.
   */
  uint16_t reportIntervalSec = 0;

  uint16_t descriptorRev = 0;

  /** The child's permanent identity. */
  uint8_t mac[HAP_MAC_LEN] = {};

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;
};
