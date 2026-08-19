#pragma once

#include <HAP.h>
#include <HValue/HValue.hpp>

#include <cstddef>
#include <cstdint>

#include <etl/string_view.h>

/**
 * @file HAPCodec.hpp
 * @brief Bounds-checked reading and writing of the primitives HAP frames are
 *        made of.
 *
 * ## Sticky failure, and why nothing returns a bool
 * Both classes latch a failure. The first write that would overflow, or the
 * first read that would run past the end, sets ok() false and every call after
 * it does nothing - a writer stops writing, a reader keeps handing back zeroes.
 * So a message codec reads like the table in the specification:
 *
 * @code
 *   uint8_t classId = reader.u8();
 *   uint8_t instance = reader.u8();
 *   uint8_t port = reader.u8();
 *   HValue value = reader.value();
 *   if (!reader.ok()) return false;   // one check, at the end
 * @endcode
 *
 * rather than as a ladder of error handling with a place to forget a check. The
 * bytes being parsed came off a radio and are attacker-shaped by definition, so
 * "cannot read past the buffer" has to be a property of the class, not of the
 * discipline of whoever writes the next message type.
 *
 * ## Little-endian, explicitly
 * Every multi-byte field is assembled a byte at a time rather than memcpy'd out
 * of native storage. Both ends of this protocol happen to be little-endian, but
 * the WIRE is little-endian by specification, and a codec that only works
 * because of what it is compiled on is a trap for the first host tool built on
 * something else.
 */

/** @brief Writes HAP primitives into a caller-owned buffer. See file docs. */
class HAPWriter {
 public:
  /**
   * @brief Wraps a buffer.
   * @param buffer Storage to fill; not owned, and must outlive the writer.
   * @param capacity How many bytes may be written.
   */
  HAPWriter(uint8_t* buffer, size_t capacity) noexcept;

  void u8(uint8_t value) noexcept;
  void u16(uint16_t value) noexcept;
  void u32(uint32_t value) noexcept;
  void i32(int32_t value) noexcept;

  /** @brief IEEE-754 single precision, little-endian. */
  void f32(float value) noexcept;

  void bytes(const uint8_t* data, size_t size) noexcept;

  /**
   * @brief A length-prefixed UTF-8 name.
   *
   * Text longer than HAP_MAX_NAME_LEN is truncated at a CODE POINT boundary, not
   * at a byte: chopping "Ліжко" mid-sequence would put an invalid UTF-8 fragment
   * on the wire, and every receiver would have to defend against it.
   */
  void name(etl::string_view text) noexcept;

  /** @brief A type byte followed by the body that type implies. */
  void value(const HValue& value) noexcept;

  /** @brief True while every write so far has fitted. */
  bool ok() const noexcept;

  /** @brief Bytes written. Meaningless unless ok(). */
  size_t size() const noexcept;

  /** @brief Bytes still available. */
  size_t remaining() const noexcept;

 private:
  bool take(size_t size) noexcept;

  uint8_t* buffer_;
  size_t capacity_;
  size_t size_;
  bool ok_;
};

/** @brief Reads HAP primitives out of a received buffer. See file docs. */
class HAPReader {
 public:
  /**
   * @brief Wraps received bytes.
   * @param buffer Not owned, and must outlive the reader - as must anything
   *        bytes() hands back, which points into it.
   */
  HAPReader(const uint8_t* buffer, size_t size) noexcept;

  uint8_t u8() noexcept;
  uint16_t u16() noexcept;
  uint32_t u32() noexcept;
  int32_t i32() noexcept;
  float f32() noexcept;

  /**
   * @brief A view of `size` raw bytes.
   * @return A pointer INTO the wrapped buffer, or nullptr if that many are not
   *         there. Nothing is copied.
   */
  const uint8_t* bytes(size_t size) noexcept;

  /** @brief A length-prefixed name; empty if the buffer ends first. */
  HAPName name() noexcept;

  /**
   * @brief A type byte and its body.
   *
   * Returns Null both for a genuine Null and for anything malformed - which is
   * safe rather than sloppy, because Null already means "no reading" everywhere
   * in this protocol. ok() tells the two apart when a caller cares.
   *
   * A String longer than HVALUE_MAX_STRING_LEN is truncated, exactly as HValue
   * truncates one everywhere else.
   */
  HValue value() noexcept;

  /** @brief True while every read so far has been satisfied. */
  bool ok() const noexcept;

  /** @brief Bytes not yet consumed. */
  size_t remaining() const noexcept;

  /** @brief Bytes consumed so far. */
  size_t offset() const noexcept;

 private:
  bool take(size_t size) noexcept;

  const uint8_t* buffer_;
  size_t size_;
  size_t offset_;
  bool ok_;
};
