#pragma once

#include <HAP.h>

#include <cstdint>

/**
 * @brief A MAC address: a node's permanent identity.
 *
 * The child index a node is given can be reassigned, and its path changes the
 * moment anything above it is rebound - but this does not. It is what a parent
 * stores, what ChildAttached carries, and what lets a master recognise a device
 * that has moved and rewrite the links that referenced it.
 *
 * A struct rather than a bare `uint8_t[6]` because an address that can be
 * compared, copied and printed by value is the difference between routing code
 * that reads like the specification and routing code full of memcmp.
 */
struct HAPMac {
  uint8_t bytes[HAP_MAC_LEN] = {};

  /** @brief All zeroes - "nobody", and never a real peer. */
  static HAPMac zero() noexcept;

  /** @brief FF:FF:FF:FF:FF:FF, which is how an unbound node is heard at all. */
  static HAPMac broadcast() noexcept;

  /** @brief Copies six bytes; a null pointer yields zero(). */
  static HAPMac fromBytes(const uint8_t* data) noexcept;

  bool isZero() const noexcept;
  bool isBroadcast() const noexcept;

  bool operator==(const HAPMac& other) const noexcept;
  bool operator!=(const HAPMac& other) const noexcept;

  /** @brief Lower-case and colon-separated, as every ESP tool prints it. */
  etl::string<17> toString() const noexcept;
};
