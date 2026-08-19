#pragma once

#include <HAP.h>
#include <HAPPath/HAPPath.hpp>

#include <cstddef>
#include <cstdint>

/** @brief Why a received buffer was not a frame. */
enum class HAPFrameError : uint8_t {
  None = 0,
  TooShort,      ///< Fewer bytes than a header.
  BadMagic,      ///< Did not begin 'H' 'A' - not ours, and common on a shared channel.
  BadVersion,    ///< A protocol this build does not speak.
  BadPathLength, ///< A path claiming more hops than the tree may be deep.
  BadHop,        ///< A zero or reserved hop index inside a path.
  TooLong        ///< More payload than a frame may carry.
};

/** @brief Human-readable form of a decode failure, for logs. Never nullptr. */
const char* HAPFrameErrorToString(HAPFrameError error) noexcept;

/**
 * @brief One HAP frame: an 18-byte header and a view of its payload.
 *
 * The header is public data because that is what it is - a wire structure, laid
 * out in Docs/Protocol.md section 2, with no invariant to protect beyond what
 * decode() has already checked.
 *
 * ## The payload is a VIEW
 * `payload` points into whatever buffer decode() was given and copies nothing.
 * A 232-byte memcpy per hop, on a device forwarding for its children, is a cost
 * with nothing to show for it - but it does mean a decoded frame is only valid
 * while the buffer it came from is. Forwarding therefore reads from the receive
 * buffer and encodes into a separate transmit buffer, never into its own source.
 *
 * ## Forwarding is three lines
 * @code
 *   HAPFrame frame;
 *   HAPFrame::decode(rx, rxSize, frame);
 *   frame.src.prepend(childIndex);          // upstream
 *   frame.encode(tx, sizeof(tx));
 * @endcode
 */
struct HAPFrame {
  /** @brief An empty Ping with both paths empty - a frame to fill in. */
  HAPFrame() noexcept;

  /** Message type; a HAPMessage, kept as a byte because the wire may carry one this build does not know. */
  uint8_t type;

  /** HAPFlags bits. */
  uint8_t flags;

  /** Unique per originator for a few seconds; echoed by Ack, Nack and RouteError. */
  uint16_t seq;

  /** Where it is going, relative to whoever holds it. Unused when Upstream is set. */
  HAPPath dest;

  /** Where it came from - written by each node it passes, on the way up. */
  HAPPath src;

  /** Not owned. See the note above about lifetime. */
  const uint8_t* payload;

  /** How many bytes `payload` points at; at most HAP_MAX_PAYLOAD_SIZE. */
  uint8_t payloadSize;

  /**
   * @brief Parses a received buffer.
   *
   * Rejects anything it cannot route: a foreign magic, a version it does not
   * speak, an impossible path length, an illegal hop. It is deliberately LENIENT
   * about one thing - bytes past a path's length are ignored rather than
   * required to be zero - because a sloppy sender is not a threat, while
   * re-encoding always zeroes them so that this node's own frames compare equal.
   *
   * @param out Untouched unless the return value is None.
   */
  static HAPFrameError decode(const uint8_t* data, size_t size,
                              HAPFrame& out) noexcept;

  /**
   * @brief Writes the header and payload into `out`.
   * @return Bytes written, or 0 if they would not fit or the payload is
   *         oversized.
   */
  size_t encode(uint8_t* out, size_t capacity) const noexcept;

  /** @brief The whole frame's size on the wire. */
  size_t frameSize() const noexcept;

  /** @brief True when every bit of `mask` is set. */
  bool has(uint8_t mask) const noexcept;

  void set(uint8_t mask) noexcept;
  void unset(uint8_t mask) noexcept;

  /** @brief True when this frame routes toward the root rather than by dest. */
  bool isUpstream() const noexcept;

  /** @brief The message type as an enum; meaningless if this build does not know it. */
  HAPMessage message() const noexcept;
};
