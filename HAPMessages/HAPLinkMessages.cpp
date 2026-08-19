#include <HAPMessages/HAPLinkMessages.hpp>

void HAPLinkSpec::encode(HAPWriter& writer) const noexcept {
  writer.u8(linkId);
  source.encode(writer);
  destination.encode(writer);
}

bool HAPLinkSpec::decode(HAPReader& reader) noexcept {
  linkId = reader.u8();

  if (!source.decode(reader)) {
    return false;
  }

  return destination.decode(reader);
}

void HAPSetLinkResponse::encode(HAPWriter& writer) const noexcept {
  writer.u8(static_cast<uint8_t>(result));
  writer.u8(linkId);
}

bool HAPSetLinkResponse::decode(HAPReader& reader) noexcept {
  result = static_cast<HAPResult>(reader.u8());
  linkId = reader.u8();
  return reader.ok();
}

void HAPClearLinkRequest::encode(HAPWriter& writer) const noexcept {
  writer.u8(linkId);
}

bool HAPClearLinkRequest::decode(HAPReader& reader) noexcept {
  linkId = reader.u8();
  return reader.ok();
}

void HAPClearLinkResponse::encode(HAPWriter& writer) const noexcept {
  writer.u8(static_cast<uint8_t>(result));
}

bool HAPClearLinkResponse::decode(HAPReader& reader) noexcept {
  result = static_cast<HAPResult>(reader.u8());
  return reader.ok();
}

void HAPListLinksRequest::encode(HAPWriter& writer) const noexcept {
  writer.u8(fromPage);
}

bool HAPListLinksRequest::decode(HAPReader& reader) noexcept {
  fromPage = reader.u8();
  return reader.ok();
}

void HAPListLinksResponse::encode(HAPWriter& writer) const noexcept {
  writer.u8(count);
  writer.u8(pageIndex);
  writer.u8(pageCount);

  for (const HAPLinkSpec& link : links) {
    link.encode(writer);
  }
}

bool HAPListLinksResponse::decode(HAPReader& reader) noexcept {
  count = reader.u8();
  pageIndex = reader.u8();
  pageCount = reader.u8();

  links.clear();

  // Records are fixed width, so the page holds however many the frame's length
  // leaves room for - the same rule descriptor pages follow.
  while (reader.ok() && reader.remaining() > 0) {
    if (links.full()) {
      return false;
    }

    HAPLinkSpec link;
    if (!link.decode(reader)) {
      return false;
    }

    links.push_back(link);
  }

  return reader.ok();
}
