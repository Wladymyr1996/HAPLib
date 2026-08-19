#include <HAPMessages/HAPControlMessages.hpp>

void HAPPing::encode(HAPWriter& writer) const noexcept {
  // Nothing to write: the frame's header is the whole message. Kept as a type
  // anyway so the dispatch table in HAPStack has no gap in it.
  (void)writer;
}

bool HAPPing::decode(HAPReader& reader) noexcept {
  return reader.ok();
}

void HAPPong::encode(HAPWriter& writer) const noexcept {
  writer.u16(descriptorRev);
}

bool HAPPong::decode(HAPReader& reader) noexcept {
  descriptorRev = reader.u16();
  return reader.ok();
}

void HAPAck::encode(HAPWriter& writer) const noexcept {
  writer.u16(seq);
}

bool HAPAck::decode(HAPReader& reader) noexcept {
  seq = reader.u16();
  return reader.ok();
}

void HAPNack::encode(HAPWriter& writer) const noexcept {
  writer.u16(seq);
  writer.u8(static_cast<uint8_t>(reason));
}

bool HAPNack::decode(HAPReader& reader) noexcept {
  seq = reader.u16();
  reason = static_cast<HAPResult>(reader.u8());
  return reader.ok();
}

void HAPRouteError::encode(HAPWriter& writer) const noexcept {
  writer.u16(seq);
  writer.u8(failedHop);
  writer.u8(static_cast<uint8_t>(reason));
}

bool HAPRouteError::decode(HAPReader& reader) noexcept {
  seq = reader.u16();
  failedHop = reader.u8();
  reason = static_cast<HAPResult>(reader.u8());
  return reader.ok();
}
