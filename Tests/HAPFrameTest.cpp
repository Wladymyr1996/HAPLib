#include "HAPTest.hpp"

#include <HAPFrame/HAPFrame.hpp>

#include <cstring>

namespace {

/** A minimal well-formed frame: a Ping addressed to the receiver. */
size_t buildPing(uint8_t* out, size_t capacity) noexcept {
  HAPFrame frame;
  frame.type = static_cast<uint8_t>(HAPMessage::Ping);
  frame.seq = 0x0102;
  return frame.encode(out, capacity);
}

void testHeaderOnlyRoundTrip() noexcept {
  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = buildPing(buffer, sizeof(buffer));

  CHECK(size == HAP_HEADER_SIZE);
  CHECK_BYTES(buffer, size,
              "4841"          // magic
              "01"            // version
              "20"            // Ping
              "00"            // flags
              "0201"          // seq 0x0102, little-endian
              "00"            // path lengths: both empty
              "0000000000"    // destPath
              "0000000000");  // srcPath

  HAPFrame decoded;
  CHECK(HAPFrame::decode(buffer, size, decoded) == HAPFrameError::None);
  CHECK(decoded.message() == HAPMessage::Ping);
  CHECK(decoded.seq == 0x0102);
  CHECK(decoded.dest.isEmpty());
  CHECK(decoded.src.isEmpty());
  CHECK(decoded.payloadSize == 0);
  CHECK(decoded.payload == nullptr);
  CHECK(decoded.frameSize() == HAP_HEADER_SIZE);
}

void testPayloadIsAViewNotACopy() noexcept {
  const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};

  HAPFrame frame;
  frame.type = static_cast<uint8_t>(HAPMessage::Report);
  frame.payload = payload;
  frame.payloadSize = sizeof(payload);

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = frame.encode(buffer, sizeof(buffer));
  CHECK(size == HAP_HEADER_SIZE + sizeof(payload));

  HAPFrame decoded;
  CHECK(HAPFrame::decode(buffer, size, decoded) == HAPFrameError::None);
  CHECK(decoded.payloadSize == sizeof(payload));

  // Points INTO the buffer it was decoded from - no copy, and no lifetime of
  // its own.
  CHECK(decoded.payload == buffer + HAP_HEADER_SIZE);
  CHECK(std::memcmp(decoded.payload, payload, sizeof(payload)) == 0);
}

void testPathLengthsShareAByte() noexcept {
  const uint8_t destHops[] = {1, 4};
  const uint8_t srcHops[] = {3};

  HAPFrame frame;
  frame.dest = HAPPath::fromBytes(destHops, 2);
  frame.src = HAPPath::fromBytes(srcHops, 1);

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = frame.encode(buffer, sizeof(buffer));
  CHECK(size == HAP_HEADER_SIZE);

  // Destination in the high nibble, source in the low.
  CHECK(buffer[7] == 0x21);
  CHECK_BYTES(buffer + 8, HAP_MAX_DEPTH * 2, "0104000000 0300000000");

  HAPFrame decoded;
  CHECK(HAPFrame::decode(buffer, size, decoded) == HAPFrameError::None);
  CHECK(decoded.dest.length() == 2);
  CHECK(decoded.src.length() == 1);
  CHECK(decoded.dest.hop(1) == 4);
}

void testFlags() noexcept {
  HAPFrame frame;
  CHECK(!frame.isUpstream());

  frame.set(HAPFlags::Upstream | HAPFlags::AckRequested);
  CHECK(frame.isUpstream());
  CHECK(frame.has(HAPFlags::AckRequested));
  CHECK(frame.has(HAPFlags::Upstream | HAPFlags::AckRequested));
  CHECK(!frame.has(HAPFlags::Queued));

  frame.unset(HAPFlags::AckRequested);
  CHECK(!frame.has(HAPFlags::AckRequested));
  CHECK(frame.isUpstream());
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

  // A path claiming more hops than the tree may be deep.
  std::memcpy(corrupt, buffer, size);
  corrupt[7] = 0x60;
  CHECK(HAPFrame::decode(corrupt, size, decoded) == HAPFrameError::BadPathLength);

  // A zero hop inside an address would route the frame somewhere nobody meant.
  std::memcpy(corrupt, buffer, size);
  corrupt[7] = 0x20;
  corrupt[8] = 1;
  corrupt[9] = 0;
  CHECK(HAPFrame::decode(corrupt, size, decoded) == HAPFrameError::BadHop);

  // 0xFF is reserved, so it is not a child index either.
  corrupt[9] = 0xFF;
  CHECK(HAPFrame::decode(corrupt, size, decoded) == HAPFrameError::BadHop);
}

void testDecodeIsLenientAboutPadding() noexcept {
  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = buildPing(buffer, sizeof(buffer));

  // A sender that left rubbish past a path's length is sloppy, not hostile.
  buffer[7] = 0x10;
  buffer[8] = 2;
  buffer[9] = 0x77;

  HAPFrame decoded;
  CHECK(HAPFrame::decode(buffer, size, decoded) == HAPFrameError::None);
  CHECK(decoded.dest.length() == 1);
  CHECK(decoded.dest.hop(0) == 2);

  // Re-encoding normalises it, so this node's own frames always compare equal.
  uint8_t reencoded[HAP_MAX_FRAME_SIZE];
  CHECK(decoded.encode(reencoded, sizeof(reencoded)) == size);
  CHECK(reencoded[9] == 0);
}

void testEncodeRefusesWhatItCannotFit() noexcept {
  uint8_t small[HAP_HEADER_SIZE - 1];
  HAPFrame frame;
  CHECK(frame.encode(small, sizeof(small)) == 0);

  // A payload pointer that is not there, with a size that says it is.
  frame.payload = nullptr;
  frame.payloadSize = 4;
  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  CHECK(frame.encode(buffer, sizeof(buffer)) == 0);
}

void testLargestLegalFrame() noexcept {
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  std::memset(payload, 0xA5, sizeof(payload));

  HAPFrame frame;
  frame.payload = payload;
  frame.payloadSize = HAP_MAX_PAYLOAD_SIZE;

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  CHECK(frame.encode(buffer, sizeof(buffer)) == HAP_MAX_FRAME_SIZE);

  HAPFrame decoded;
  CHECK(HAPFrame::decode(buffer, HAP_MAX_FRAME_SIZE, decoded) ==
        HAPFrameError::None);
  CHECK(decoded.payloadSize == HAP_MAX_PAYLOAD_SIZE);

  // One byte more than ESP-NOW v1 can carry is not a frame.
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
  testPathLengthsShareAByte();
  testFlags();
  testRejections();
  testDecodeIsLenientAboutPadding();
  testEncodeRefusesWhatItCannotFit();
  testLargestLegalFrame();
}
