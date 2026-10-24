#include "HAPTest.hpp"

#include <HAPFrame/HAPFrame.hpp>

#include <cstring>

namespace {

HAPNodeId idOf(uint8_t last) noexcept {
  HAPNodeId id;
  id.bytes[0] = 0x24;
  id.bytes[1] = 0x6F;
  id.bytes[2] = 0x28;
  id.bytes[3] = 0xFF;
  id.bytes[4] = 0xFE;
  id.bytes[7] = last;
  return id;
}

/** A minimal well-formed frame: a Ping for the master, from node ...01. */
size_t buildPing(uint8_t* out, size_t capacity) noexcept {
  HAPFrame frame;
  frame.type = static_cast<uint8_t>(HAPMessage::Ping);
  frame.seq = 0x0102;
  frame.src = idOf(1);
  return frame.encode(out, capacity);
}

void testHeaderOnlyRoundTrip() noexcept {
  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = buildPing(buffer, sizeof(buffer));

  CHECK(size == HAP_HEADER_SIZE);
  CHECK(HAP_HEADER_SIZE == 23);
  CHECK_BYTES(buffer, size,
              "4841"                // magic
              "02"                  // version
              "20"                  // Ping
              "00"                  // flags
              "0201"                // seq 0x0102, little-endian
              "0000000000000000"    // dst at 7: zero, the master
              "246F28FFFE000001");  // src at 15, in stored order

  HAPFrame decoded;
  CHECK(HAPFrame::decode(buffer, size, decoded) == HAPFrameError::None);
  CHECK(decoded.message() == HAPMessage::Ping);
  CHECK(decoded.seq == 0x0102);
  CHECK(decoded.dst.isZero());
  CHECK(decoded.src == idOf(1));
  CHECK(decoded.isForMaster());
  CHECK(decoded.payloadSize == 0);
  CHECK(decoded.payload == nullptr);
  CHECK(decoded.frameSize() == HAP_HEADER_SIZE);
}

void testPayloadIsAViewNotACopy() noexcept {
  const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};

  HAPFrame frame;
  frame.type = static_cast<uint8_t>(HAPMessage::Report);
  frame.src = idOf(1);
  frame.payload = payload;
  frame.payloadSize = sizeof(payload);

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = frame.encode(buffer, sizeof(buffer));
  CHECK(size == HAP_HEADER_SIZE + sizeof(payload));

  HAPFrame decoded;
  CHECK(HAPFrame::decode(buffer, size, decoded) == HAPFrameError::None);
  CHECK(decoded.payloadSize == sizeof(payload));

  // Points INTO the buffer it was decoded from - no copy, and no lifetime of
  // its own. The payload begins at offset 23.
  CHECK(decoded.payload == buffer + 23);
  CHECK(std::memcmp(decoded.payload, payload, sizeof(payload)) == 0);
}

void testIdsKeepTheirByteOrder() noexcept {
  HAPFrame frame;
  frame.dst = idOf(2);
  frame.src = idOf(3);

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = frame.encode(buffer, sizeof(buffer));
  CHECK(size == HAP_HEADER_SIZE);

  // Eight raw bytes each, not a little-endian integer.
  CHECK_BYTES(buffer + 7, 16, "246F28FFFE000002 246F28FFFE000003");

  HAPFrame decoded;
  CHECK(HAPFrame::decode(buffer, size, decoded) == HAPFrameError::None);
  CHECK(decoded.dst == idOf(2));
  CHECK(decoded.src == idOf(3));
  CHECK(!decoded.isForMaster());
}

void testFlags() noexcept {
  HAPFrame frame;
  CHECK(!frame.has(HAPFlags::AckRequested));

  frame.set(HAPFlags::More | HAPFlags::AckRequested);
  CHECK(frame.has(HAPFlags::AckRequested));
  CHECK(frame.has(HAPFlags::More | HAPFlags::AckRequested));
  CHECK(!frame.has(HAPFlags::Queued));

  frame.unset(HAPFlags::AckRequested);
  CHECK(!frame.has(HAPFlags::AckRequested));
  CHECK(frame.has(HAPFlags::More));
}

void testRetiredFlagBitSurvivesARoundTrip() noexcept {
  // 0x02 was Upstream. It is reserved now: carried untouched, never acted on.
  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  HAPFrame frame;
  frame.src = idOf(1);
  frame.flags = 0x02;
  const size_t size = frame.encode(buffer, sizeof(buffer));
  CHECK(buffer[4] == 0x02);

  HAPFrame decoded;
  CHECK(HAPFrame::decode(buffer, size, decoded) == HAPFrameError::None);
  CHECK(decoded.flags == 0x02);
  CHECK(decoded.isForMaster());
  CHECK((HAPFlags::Reserved & 0x02) != 0);
}

void testRejections() noexcept {
  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = buildPing(buffer, sizeof(buffer));
  HAPFrame decoded;

  CHECK(HAPFrame::decode(buffer, HAP_HEADER_SIZE - 1, decoded) ==
        HAPFrameError::TooShort);
  CHECK(HAPFrame::decode(nullptr, size, decoded) == HAPFrameError::TooShort);

  uint8_t corrupt[HAP_MAX_FRAME_SIZE];

  // Another protocol sharing the channel: rejected before anything is believed.
  std::memcpy(corrupt, buffer, size);
  corrupt[0] = 'X';
  CHECK(HAPFrame::decode(corrupt, size, decoded) == HAPFrameError::BadMagic);

  std::memcpy(corrupt, buffer, size);
  corrupt[2] = HAP_VERSION + 1;
  CHECK(HAPFrame::decode(corrupt, size, decoded) == HAPFrameError::BadVersion);

  // A version 1 frame is not understood: there is no compatibility mode.
  std::memcpy(corrupt, buffer, size);
  corrupt[2] = 1;
  CHECK(HAPFrame::decode(corrupt, size, decoded) == HAPFrameError::BadVersion);

  // A source of all zeroes is a sender that forgot to say who it is.
  std::memcpy(corrupt, buffer, size);
  std::memset(corrupt + 15, 0, HAP_NODE_ID_LEN);
  CHECK(HAPFrame::decode(corrupt, size, decoded) == HAPFrameError::NoSource);
}

void testEncodeRefusesWhatItCannotFit() noexcept {
  uint8_t small[HAP_HEADER_SIZE - 1];
  HAPFrame frame;
  frame.src = idOf(1);
  CHECK(frame.encode(small, sizeof(small)) == 0);

  // A payload pointer that is not there, with a size that says it is.
  frame.payload = nullptr;
  frame.payloadSize = 4;
  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  CHECK(frame.encode(buffer, sizeof(buffer)) == 0);

  // More payload than a frame may carry.
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE + 1] = {};
  frame.payload = payload;
  frame.payloadSize = HAP_MAX_PAYLOAD_SIZE + 1;
  CHECK(frame.encode(buffer, sizeof(buffer)) == 0);
}

void testEncodeRefusesAZeroSource() noexcept {
  // A zero destination is the master and is fine; a zero source is a bug.
  HAPFrame frame;
  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  CHECK(frame.encode(buffer, sizeof(buffer)) == 0);

  frame.dst = idOf(2);
  CHECK(frame.encode(buffer, sizeof(buffer)) == 0);

  frame.src = idOf(1);
  CHECK(frame.encode(buffer, sizeof(buffer)) == HAP_HEADER_SIZE);
}

void testLargestLegalFrame() noexcept {
  CHECK(HAP_MAX_FRAME_SIZE == 255);
  CHECK(HAP_MAX_PAYLOAD_SIZE == 232);

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  std::memset(payload, 0xA5, sizeof(payload));

  HAPFrame frame;
  frame.src = idOf(1);
  frame.payload = payload;
  frame.payloadSize = HAP_MAX_PAYLOAD_SIZE;

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  CHECK(frame.encode(buffer, sizeof(buffer)) == HAP_MAX_FRAME_SIZE);

  HAPFrame decoded;
  CHECK(HAPFrame::decode(buffer, HAP_MAX_FRAME_SIZE, decoded) ==
        HAPFrameError::None);
  CHECK(decoded.payloadSize == HAP_MAX_PAYLOAD_SIZE);

  // One byte more than the largest frame is not a frame.
  uint8_t oversized[HAP_MAX_FRAME_SIZE + 1];
  std::memcpy(oversized, buffer, HAP_MAX_FRAME_SIZE);
  oversized[HAP_MAX_FRAME_SIZE] = 0;
  CHECK(HAPFrame::decode(oversized, sizeof(oversized), decoded) ==
        HAPFrameError::TooLong);
}

}  // namespace

void runFrameTests() noexcept {
  HAPTest::begin("HAPFrame");

  testHeaderOnlyRoundTrip();
  testPayloadIsAViewNotACopy();
  testIdsKeepTheirByteOrder();
  testFlags();
  testRetiredFlagBitSurvivesARoundTrip();
  testRejections();
  testEncodeRefusesWhatItCannotFit();
  testEncodeRefusesAZeroSource();
  testLargestLegalFrame();
}
