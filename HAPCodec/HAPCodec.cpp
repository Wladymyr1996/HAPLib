#include <HAPCodec/HAPCodec.hpp>

#include <cstring>

// The f32 conversions below reinterpret a float's storage as an integer, which
// is only the IEEE-754 bit pattern the specification names if the compiler
// agrees that a float IS one.
static_assert(sizeof(float) == 4, "HAP encodes a float as four bytes.");

namespace {

/** True when this byte continues a UTF-8 sequence rather than starting one. */
bool isUtf8Continuation(uint8_t byte) noexcept {
  return (byte & 0xC0) == 0x80;
}

/**
 * The largest length <= `limit` that does not split a UTF-8 sequence.
 *
 * Walks back over continuation bytes to the start byte that owns them. A name
 * truncated mid-sequence is invalid UTF-8, and this protocol carries Ukrainian
 * names as a matter of course - two bytes per character, so the boundary is hit
 * by any odd-length cut.
 */
size_t utf8SafeLength(const char* text, size_t length, size_t limit) noexcept {
  if (length <= limit) {
    return length;
  }

  size_t cut = limit;
  while (cut > 0 && isUtf8Continuation(static_cast<uint8_t>(text[cut]))) {
    --cut;
  }

  return cut;
}

}  // namespace

// ---------------------------------------------------------------------------
// HAPWriter
// ---------------------------------------------------------------------------

HAPWriter::HAPWriter(uint8_t* buffer, size_t capacity) noexcept
    : buffer_(buffer),
      capacity_(buffer == nullptr ? 0 : capacity),
      size_(0),
      ok_(buffer != nullptr) {}

bool HAPWriter::take(size_t size) noexcept {
  if (!ok_) {
    return false;
  }

  if (size > capacity_ - size_) {
    ok_ = false;
    return false;
  }

  return true;
}

void HAPWriter::u8(uint8_t value) noexcept {
  if (!take(1)) {
    return;
  }

  buffer_[size_++] = value;
}

void HAPWriter::u16(uint16_t value) noexcept {
  if (!take(2)) {
    return;
  }

  buffer_[size_++] = static_cast<uint8_t>(value & 0xFF);
  buffer_[size_++] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void HAPWriter::u32(uint32_t value) noexcept {
  if (!take(4)) {
    return;
  }

  buffer_[size_++] = static_cast<uint8_t>(value & 0xFF);
  buffer_[size_++] = static_cast<uint8_t>((value >> 8) & 0xFF);
  buffer_[size_++] = static_cast<uint8_t>((value >> 16) & 0xFF);
  buffer_[size_++] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void HAPWriter::i32(int32_t value) noexcept {
  // Through the unsigned type on purpose: shifting a negative signed value is
  // implementation-defined, and two's complement is what the wire specifies.
  u32(static_cast<uint32_t>(value));
}

void HAPWriter::f32(float value) noexcept {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  u32(bits);
}

void HAPWriter::bytes(const uint8_t* data, size_t size) noexcept {
  if (size == 0) {
    return;
  }

  if (data == nullptr) {
    ok_ = false;
    return;
  }

  if (!take(size)) {
    return;
  }

  std::memcpy(buffer_ + size_, data, size);
  size_ += size;
}

void HAPWriter::name(etl::string_view text) noexcept {
  const size_t length =
      utf8SafeLength(text.data(), text.size(), HAP_MAX_NAME_LEN);

  if (!take(1 + length)) {
    return;
  }

  u8(static_cast<uint8_t>(length));
  bytes(reinterpret_cast<const uint8_t*>(text.data()), length);
}

void HAPWriter::value(const HValue& value) noexcept {
  switch (value.type()) {
    case HValue::Type::Null:
      u8(static_cast<uint8_t>(HAPValueType::Null));
      break;

    case HValue::Type::Bool:
      u8(static_cast<uint8_t>(HAPValueType::Bool));
      u8(value.asBool() ? 1 : 0);
      break;

    case HValue::Type::Int:
      u8(static_cast<uint8_t>(HAPValueType::Int));
      i32(static_cast<int32_t>(value.asInt()));
      break;

    case HValue::Type::Float:
      u8(static_cast<uint8_t>(HAPValueType::Float));
      f32(value.asFloat());
      break;

    case HValue::Type::String: {
      const etl::string<HVALUE_MAX_STRING_LEN> text = value.asString();
      u8(static_cast<uint8_t>(HAPValueType::String));
      u8(static_cast<uint8_t>(text.size()));
      bytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
      break;
    }
  }
}

bool HAPWriter::ok() const noexcept {
  return ok_;
}

size_t HAPWriter::size() const noexcept {
  return size_;
}

size_t HAPWriter::remaining() const noexcept {
  return ok_ ? capacity_ - size_ : 0;
}

// ---------------------------------------------------------------------------
// HAPReader
// ---------------------------------------------------------------------------

HAPReader::HAPReader(const uint8_t* buffer, size_t size) noexcept
    : buffer_(buffer),
      size_(buffer == nullptr ? 0 : size),
      offset_(0),
      ok_(buffer != nullptr) {}

bool HAPReader::take(size_t size) noexcept {
  if (!ok_) {
    return false;
  }

  if (size > size_ - offset_) {
    ok_ = false;
    return false;
  }

  return true;
}

uint8_t HAPReader::u8() noexcept {
  if (!take(1)) {
    return 0;
  }

  return buffer_[offset_++];
}

uint16_t HAPReader::u16() noexcept {
  if (!take(2)) {
    return 0;
  }

  const uint16_t low = buffer_[offset_++];
  const uint16_t high = buffer_[offset_++];
  return static_cast<uint16_t>(low | (high << 8));
}

uint32_t HAPReader::u32() noexcept {
  if (!take(4)) {
    return 0;
  }

  uint32_t value = 0;
  value |= static_cast<uint32_t>(buffer_[offset_++]);
  value |= static_cast<uint32_t>(buffer_[offset_++]) << 8;
  value |= static_cast<uint32_t>(buffer_[offset_++]) << 16;
  value |= static_cast<uint32_t>(buffer_[offset_++]) << 24;
  return value;
}

int32_t HAPReader::i32() noexcept {
  return static_cast<int32_t>(u32());
}

float HAPReader::f32() noexcept {
  const uint32_t bits = u32();

  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

const uint8_t* HAPReader::bytes(size_t size) noexcept {
  if (!take(size)) {
    return nullptr;
  }

  const uint8_t* start = buffer_ + offset_;
  offset_ += size;
  return start;
}

HAPName HAPReader::name() noexcept {
  HAPName text;

  const uint8_t length = u8();
  const uint8_t* data = bytes(length);
  if (data == nullptr) {
    return text;
  }

  // A name longer than the buffer is a peer built with a different limit, not
  // an attack. Truncating at a code point boundary keeps it displayable.
  const size_t kept =
      utf8SafeLength(reinterpret_cast<const char*>(data), length, HAP_MAX_NAME_LEN);
  text.assign(reinterpret_cast<const char*>(data), kept);
  return text;
}

HValue HAPReader::value() noexcept {
  const uint8_t type = u8();

  switch (static_cast<HAPValueType>(type)) {
    case HAPValueType::Null:
      return HValue();

    // Each of these checks ok() before believing what it parsed. A truncated
    // body reads back as zeroes, and handing those over as a value would turn
    // a damaged frame into a confident 0.0 - which is the one thing this
    // protocol's Null exists to prevent.
    case HAPValueType::Bool: {
      const bool parsed = u8() != 0;
      return ok_ ? HValue(parsed) : HValue();
    }

    case HAPValueType::Int: {
      const int32_t parsed = i32();
      return ok_ ? HValue(static_cast<int>(parsed)) : HValue();
    }

    case HAPValueType::Float: {
      const float parsed = f32();
      return ok_ ? HValue(parsed) : HValue();
    }

    case HAPValueType::String: {
      const uint8_t length = u8();
      const uint8_t* data = bytes(length);
      if (data == nullptr) {
        return HValue();
      }

      const size_t kept = utf8SafeLength(reinterpret_cast<const char*>(data),
                                         length, HVALUE_MAX_STRING_LEN);
      return HValue(etl::string_view(reinterpret_cast<const char*>(data), kept));
    }

    default:
      // An unknown type byte cannot be skipped: its body has no known length, so
      // everything after it in this frame is unparseable too.
      ok_ = false;
      return HValue();
  }
}

bool HAPReader::ok() const noexcept {
  return ok_;
}

size_t HAPReader::remaining() const noexcept {
  return ok_ ? size_ - offset_ : 0;
}

size_t HAPReader::offset() const noexcept {
  return offset_;
}
