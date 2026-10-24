#pragma once

#include <HAP.h>

#include <cstdint>

#include <etl/string.h>

/**
 * @brief A node's permanent identity: its factory IEEE 802.15.4 EUI-64.
 *
 * What a master stores about a device, what a link names, what the successor
 * tie-break compares. It is NOT where to send bytes - that is a HAPAddress, which
 * may change under a node that keeps its id.
 *
 * A struct rather than a bare `uint8_t[8]` because an id that can be compared,
 * ordered, copied and printed by value is the difference between code that reads
 * like the specification and code full of memcmp.
 *
 * ## The MAC form
 * The v1 frames and the stored state carry six bytes, not eight. fromMac()
 * and toMac() convert in the standard way - an EUI-48 becomes an EUI-64 by
 * inserting FF FE in the middle - and are the only place that narrower width is
 * known. The zero id converts to the zero MAC and back, so "nobody" stays
 * "nobody" through a round trip.
 */
struct HAPNodeId {
  uint8_t bytes[HAP_NODE_ID_LEN] = {};

  /** @brief All zeroes - "nobody", and never a real node. */
  static HAPNodeId zero() noexcept;

  /** @brief Copies eight bytes; a null pointer yields zero(). */
  static HAPNodeId fromBytes(const uint8_t* data) noexcept;

  /** @brief Widens HAP_MAC_LEN bytes of MAC; a null pointer yields zero(). */
  static HAPNodeId fromMac(const uint8_t* mac) noexcept;

  /** @brief Narrows to HAP_MAC_LEN bytes of MAC - exact for ids made by fromMac(). */
  void toMac(uint8_t* mac) const noexcept;

  bool isZero() const noexcept;

  bool operator==(const HAPNodeId& other) const noexcept;
  bool operator!=(const HAPNodeId& other) const noexcept;

  /** @brief Compares as an 8-byte big-endian number - the successor rule. */
  bool operator<(const HAPNodeId& other) const noexcept;

  /** @brief Lower-case and colon-separated, as every tool prints an EUI-64. */
  etl::string<23> toString() const noexcept;
};
