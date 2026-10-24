#pragma once

#include <HAP.h>
#include <HAPNodeId/HAPNodeId.hpp>

#include <cstdint>

#include <etl/string.h>

/**
 * @brief Where a transport sends bytes: opaque to everything above it.
 *
 * An IPv6 address on Thread, a MAC on ESP-NOW and on the loopback bus - HAP
 * never looks inside, it only stores, compares and hands one back to the
 * transport that produced it. It may change under a node that keeps its
 * HAPNodeId, which is why the two are different types.
 *
 * Three values are not the address of anybody and mean something to every
 * transport: zero() is "no address", master() is the anycast destination - the
 * current master, whoever that is and wherever it lives - and broadcast() is
 * "everyone in range", which only binding uses and the star removes.
 *
 * ## The tree-era bridge
 * Today's frames carry no node id, so the stack recovers who sent a frame from
 * where it came from, and sends to a child at the address it was heard on. That
 * only works because the transports in the tree use the MAC as the address.
 * fromNodeId() and nodeId() are that assumption, written down once; frames that
 * carry both ids make them go away.
 */
struct HAPAddress {
  uint8_t bytes[HAP_ADDRESS_LEN] = {};

  /** @brief All zeroes - "no address", and never a real peer. */
  static HAPAddress zero() noexcept;

  /** @brief The anycast destination: the master, wherever it is. */
  static HAPAddress master() noexcept;

  /** @brief Everyone in range, which is how an unbound node is heard at all. */
  static HAPAddress broadcast() noexcept;

  /** @brief Copies HAP_ADDRESS_LEN bytes; a null pointer yields zero(). */
  static HAPAddress fromBytes(const uint8_t* data) noexcept;

  /** @brief An address that is a HAP_MAC_LEN-byte MAC; null yields zero(). */
  static HAPAddress fromMac(const uint8_t* mac) noexcept;

  /** @brief The address of the node with this id, on a MAC-addressed transport. */
  static HAPAddress fromNodeId(const HAPNodeId& id) noexcept;

  /** @brief The first HAP_MAC_LEN bytes, for a MAC-addressed transport. */
  void toMac(uint8_t* mac) const noexcept;

  /** @brief The id of the node at this address, on a MAC-addressed transport. */
  HAPNodeId nodeId() const noexcept;

  bool isZero() const noexcept;
  bool isMaster() const noexcept;
  bool isBroadcast() const noexcept;

  bool operator==(const HAPAddress& other) const noexcept;
  bool operator!=(const HAPAddress& other) const noexcept;

  /** @brief Hex in groups of two bytes - for logs, not for parsing. */
  etl::string<39> toString() const noexcept;
};
