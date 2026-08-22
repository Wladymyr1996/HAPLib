#pragma once

#include <HAPMessages/HAPMessageParts.hpp>

/**
 * @file HAPServiceMessages.hpp
 * @brief Node service: messages about the DEVICE, not about what it measures.
 *
 * Docs/Protocol.md section 4.19 onward, message codes 0x60-0x6F. Everything
 * else in the catalogue is about a node's readings, its name, or the shape of
 * the network. This is the range for asking a node to do something to itself.
 *
 * There is one pair so far, and it is the firmware update.
 */

/** @brief Bits in HAPOtaRequest::flags. */
namespace HAPOtaFlags {
constexpr uint8_t None = 0x00;

/**
 * Let the node reinstall the version it is already running.
 *
 * For repairing a node that came back from a partial write, where the version
 * is right and the image is not.
 */
constexpr uint8_t AllowSameVersion = 0x01;

/**
 * Answer, and do NOTHING else.
 *
 * What makes this pair a version QUERY as well as a command: a master that
 * wants to know what a node is running sends this and reads the version out of
 * the response, without the node joining a network or touching its flash. It is
 * also the only way to confirm an update afterwards, since the node that would
 * have reported the outcome has been replaced by the one it installed.
 *
 * A node MUST NOT act on a request carrying this bit.
 */
constexpr uint8_t QueryOnly = 0x02;

/** Written as zero, ignored on receipt - this is how version 1 grows. */
constexpr uint8_t Reserved = 0xFC;
}  // namespace HAPOtaFlags

/**
 * @brief "Join this network and install the firmware at this URL."
 *
 * Sent by a master to one node. The node answers immediately with what it is
 * running and whether it accepted, then - if it did - leaves the mesh, joins the
 * router named here, downloads over plain HTTP and reboots. It is not heard from
 * again until it comes back and reports as usual.
 *
 * ## Why one message rather than three writes into a private class
 * A parent queues ONE frame for a sleeping child (Protocol.md §6), so three
 * separate writes carrying an SSID, a passphrase and a URL would take three
 * report cycles to arrive - three minutes on a node reporting once a minute -
 * and could be acted on half-delivered. Everything needed to start an update
 * fits one 232-byte payload, so it travels as one.
 *
 * ## What it does not carry
 * No checksum and no signature. The node validates the IMAGE - chip, product,
 * version - out of the image's own header, and the transport this URL names is
 * plain HTTP: this message authenticates the INSTRUCTION, because it arrives
 * over an encrypted link from a bound parent, and it says nothing about who
 * serves the bytes.
 */
struct HAPOtaRequest {
  /** HAPOtaFlags bits. */
  uint8_t flags = HAPOtaFlags::None;

  /**
   * The Wi-Fi channel the router is on, or 0 to scan for it.
   *
   * Worth carrying because a scan across every channel costs a battery node
   * seconds of radio it does not have to spend when the master already knows.
   */
  uint8_t channel = 0;

  HAPSsid ssid;
  HAPPassphrase passphrase;

  /** Where the image is. `http://host[:port]/path`. */
  HAPUrl url;

  void encode(HAPWriter& writer) const noexcept;

  /** @return false when the reader ran out; the object is then meaningless. */
  bool decode(HAPReader& reader) noexcept;

  /** @brief Bytes this message occupies on the wire. */
  size_t encodedSize() const noexcept;

  /** @brief True when the master only wants the version back. */
  bool isQueryOnly() const noexcept;

  /** @brief True when there is a network and a URL worth trying. */
  bool isActionable() const noexcept;
};

/**
 * @brief "Here is what I am running, and whether I will do it."
 *
 * Sent the moment the request is understood, NOT when the update finishes. A
 * node cannot report the outcome of an update that replaces the firmware doing
 * the reporting, and a master that waited for one would wait forever.
 *
 * So `result` means "accepted, and I am about to try" rather than "installed".
 * What a master does with that is send a QueryOnly request later and compare
 * the version - which is why the version is here at all.
 */
struct HAPOtaResponse {
  /**
   * Ok when the node accepted, or why not:
   * - Unsupported - this node cannot update itself over the air;
   * - BadRequest - no SSID, or a URL it will not try;
   * - Busy - an update is already in progress.
   */
  HAPResult result = HAPResult::Ok;

  /** The firmware version running RIGHT NOW, before any update. */
  HAPName version;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;
  size_t encodedSize() const noexcept;
};
