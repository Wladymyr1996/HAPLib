#include "HAPTest.hpp"

#include <HAPCodec/HAPCodec.hpp>

#include <cstring>

namespace {

void testLittleEndian() noexcept {
  uint8_t buffer[16];
  HAPWriter writer(buffer, sizeof(buffer));

  writer.u8(0x9C);
  writer.u16(0x9C4A);   // descriptorRev, as it appears in the specification
  writer.u32(0x11223344);
  writer.i32(-2);

  CHECK(writer.ok());
  CHECK_BYTES(buffer, writer.size(), "9C 4A9C 44332211 FEFFFFFF");
}

void testFloatsMatchTheSpecification() noexcept {
  // Docs/Protocol.md section 8.2 prints these two readings byte for byte.
  uint8_t buffer[8];
  HAPWriter writer(buffer, sizeof(buffer));

  writer.f32(21.5f);
  writer.f32(44.0f);

  CHECK(writer.ok());
  CHECK_BYTES(buffer, writer.size(), "0000AC41 00003042");

  HAPReader reader(buffer, writer.size());
  CHECK(reader.f32() == 21.5f);
  CHECK(reader.f32() == 44.0f);
  CHECK(reader.ok());
  CHECK(reader.remaining() == 0);
}

void testNames() noexcept {
  uint8_t buffer[64];
  HAPWriter writer(buffer, sizeof(buffer));

  writer.name("Bedroom");
  CHECK(writer.ok());
  CHECK_BYTES(buffer, writer.size(), "07 426564726F6F6D");

  HAPReader reader(buffer, writer.size());
  CHECK(std::strcmp(reader.name().c_str(), "Bedroom") == 0);
  CHECK(reader.ok());
}

void testUkrainianNameIsBytesNotCharacters() noexcept {
  uint8_t buffer[64];
  HAPWriter writer(buffer, sizeof(buffer));

  // "Ліжко" from Docs/Protocol.md section 8.4: five characters, ten bytes.
  writer.name("Ліжко");

  CHECK(writer.ok());
  CHECK_BYTES(buffer, writer.size(), "0A D09BD196D0B6D0BAD0BE");
}

void testOverlongNameIsCutAtACodePoint() noexcept {
  uint8_t buffer[64];
  HAPWriter writer(buffer, sizeof(buffer));

  // Seventeen Cyrillic characters is 34 bytes, three over the limit. A plain
  // cut at 31 would land in the middle of a two-byte sequence and put invalid
  // UTF-8 on the wire, so the whole character goes and 30 bytes survive.
  writer.name("ААААААААААААААААА");

  CHECK(writer.ok());
  CHECK(writer.size() == 1 + 30);
  CHECK(buffer[0] == 30);

  HAPReader reader(buffer, writer.size());
  const HAPName name = reader.name();
  CHECK(name.size() == 30);
  CHECK(reader.ok());
}

void testValues() noexcept {
  uint8_t buffer[128];
  HAPWriter writer(buffer, sizeof(buffer));

  writer.value(HValue());          // Null - "no reading", never zero
  writer.value(HValue(true));
  writer.value(HValue(-1));
  writer.value(HValue(21.5f));
  writer.value(HValue("hi"));

  CHECK(writer.ok());
  CHECK_BYTES(buffer, writer.size(),
              "00"
              "01 01"
              "02 FFFFFFFF"
              "03 0000AC41"
              "04 02 6869");

  HAPReader reader(buffer, writer.size());

  const HValue nullValue = reader.value();
  CHECK(nullValue.isNull());

  const HValue boolValue = reader.value();
  CHECK(boolValue.isBool() && boolValue.asBool());

  const HValue intValue = reader.value();
  CHECK(intValue.isInt() && intValue.asInt() == -1);

  const HValue floatValue = reader.value();
  CHECK(floatValue.isFloat() && floatValue.asFloat() == 21.5f);

  const HValue stringValue = reader.value();
  CHECK(stringValue.isString());
  CHECK(std::strcmp(stringValue.asString().c_str(), "hi") == 0);

  CHECK(reader.ok());
  CHECK(reader.remaining() == 0);
}

void testWriterStopsAtCapacity() noexcept {
  // Zeroed, and that is load-bearing: the check below proves the rejected u32
  // left byte 2 alone, which is only a statement about the writer if the byte
  // started as something known. Uninitialised, it read whatever the stack
  // happened to hold - 0 in a debug build, garbage in a release one.
  uint8_t buffer[4] = {0};
  HAPWriter writer(buffer, sizeof(buffer));

  writer.u16(0x1234);
  CHECK(writer.ok());
  CHECK(writer.remaining() == 2);

  writer.u32(0);  // One byte too many: rejected whole, not half written.
  CHECK(!writer.ok());
  CHECK(writer.remaining() == 0);
  CHECK(buffer[2] == 0);

  // Sticky: everything after a failure is a no-op, so a caller checking once at
  // the end cannot be fooled by a later write appearing to succeed.
  writer.u8(0xFF);
  CHECK(!writer.ok());
}

void testReaderStopsAtTheEnd() noexcept {
  const uint8_t truncated[] = {0x03, 0x00, 0x00};  // A Float missing a byte.

  HAPReader reader(truncated, sizeof(truncated));
  const HValue value = reader.value();

  CHECK(!reader.ok());
  CHECK(value.isNull());

  // Reading on after the end keeps handing back zeroes rather than whatever is
  // in memory past the buffer.
  CHECK(reader.u32() == 0);
  CHECK(reader.bytes(1) == nullptr);
  CHECK(reader.remaining() == 0);
}

void testTruncatedNameIsRefused() noexcept {
  const uint8_t truncated[] = {0x07, 'B', 'e', 'd'};  // Claims 7 bytes, carries 3.

  HAPReader reader(truncated, sizeof(truncated));
  const HAPName name = reader.name();

  CHECK(!reader.ok());
  CHECK(name.empty());
}

void testUnknownValueTypeStopsParsing() noexcept {
  // An unknown type byte cannot be skipped - its body has no known length, so
  // everything after it in the frame is unparseable too.
  const uint8_t unknown[] = {0x7F, 0x01, 0x02, 0x03};

  HAPReader reader(unknown, sizeof(unknown));
  const HValue value = reader.value();

  CHECK(!reader.ok());
  CHECK(value.isNull());
}

void testNullBuffers() noexcept {
  HAPWriter writer(nullptr, 16);
  writer.u8(1);
  CHECK(!writer.ok());
  CHECK(writer.size() == 0);

  HAPReader reader(nullptr, 16);
  CHECK(reader.u8() == 0);
  CHECK(!reader.ok());
}

}  // namespace

void runCodecTests() noexcept {
  HAPTest::begin("HAPCodec");

  testLittleEndian();
  testFloatsMatchTheSpecification();
  testNames();
  testUkrainianNameIsBytesNotCharacters();
  testOverlongNameIsCutAtACodePoint();
  testValues();
  testWriterStopsAtCapacity();
  testReaderStopsAtTheEnd();
  testTruncatedNameIsRefused();
  testUnknownValueTypeStopsParsing();
  testNullBuffers();
}
