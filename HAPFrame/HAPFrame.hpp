#pragma once

#include <HAP.h>
#include <HAPNodeId/HAPNodeId.hpp>

#include <cstddef>
#include <cstdint>

/** @brief Why a received buffer was not a frame. */
enum class HAPFrameError : uint8_t {
  None = 0,
  TooShort,      ///< Fewer bytes than a header.
  BadMagic,      ///< Did not begin 'H' 'A' - not ours, and common on a shared channel.
  BadVersion,    ///< A protocol this build does not speak.
  NoSource,      ///< An all-zero source id: nobody sent it, so nobody can answer.
  TooLong        ///< More payload than a frame may carry.
};

/** @brief Human-readable form of a decode failure, for logs. Never nullptr. */
const char* HAPFrameErrorToString(HAPFrameError error) noexcept;

/**
 * @brief One HAP frame: a 23-byte header and a view of its payload.
 *
 * The header is public data because that is what it is - a wire structure, laid
 * out in Docs/ThreadDesign.md section 5, with no invariant to protect beyond
 * what decode() has already checked.
 *
 * ## The payload is a VIEW
 * `payload` points into whatever buffer decode() was given and copies nothing.
 * A 232-byte memcpy per hop, on a device forwarding for its children, is a cost
 * with nothing to show for it - but it does mean a decoded frame is only valid
 * while the buffer it came from is. Forwarding therefore reads from the receive
 * buffer and encodes into a separate transmit buffer, never into its own source.
 *
 * ## Addresses are node ids
 * `src` is the ORIGINATOR and is never rewritten by forwarding, so a frame
 * decoded anywhere on its way says who sent it. `dst` is the target, or all
 * zeroes for "the master, whoever it is". On the wire both are their eight bytes
 * in stored order (see HAPCodec.hpp), not integers.
 */
struct HAPFrame {
  /** @brief An empty Ping with both ids zero - a frame to fill in. */
  HAPFrame() noexcept;

  /** Message type; a HAPMessage, kept as a byte because the wire may carry one this build does not know. */
  uint8_t type;

  /** HAPFlags bits. */
  uint8_t flags;

  /** Unique per originator for a few seconds; echoed by Ack and Nack. */
  uint16_t seq;

  /** Where it is going. All zeroes means the master, whoever it is. */
  HAPNodeId dst;

  /** Who sent it. Never zero, and not rewritten by a node that forwards it. */
  HAPNodeId src;

  /** Not owned. See the note above about lifetime. */
  const uint8_t* payload;

  /** How many bytes `payload` points at; at most HAP_MAX_PAYLOAD_SIZE. */
  uint8_t payloadSize;

  /**
   * @brief Parses a received buffer.
   *
   * Rejects anything it cannot answer: a foreign magic, a version it does not
   * speak (a version 1 frame is one), a zero source.
   *
   * @param out Untouched unless the return value is None.
   */
  static HAPFrameError decode(const uint8_t* data, size_t size,
                              HAPFrame& out) noexcept;

  /**
   * @brief Writes the header and payload into `out`.
   * @return Bytes written, or 0 if they would not fit, the payload is oversized
   *         or the source is zero. A zero `dst` is fine: it is the master.
   */
  size_t encode(uint8_t* out, size_t capacity) const noexcept;

  /** @brief The whole frame's size on the wire. */
  size_t frameSize() const noexcept;

  /** @brief True when every bit of `mask` is set. */
  bool has(uint8_t mask) const noexcept;

  void set(uint8_t mask) noexcept;
  void unset(uint8_t mask) noexcept;

  /** @brief True when `dst` is zero: the frame is for the master, whoever it is. */
  bool isForMaster() const noexcept;

  /** @brief The message type as an enum; meaningless if this build does not know it. */
  HAPMessage message() const noexcept;
};
