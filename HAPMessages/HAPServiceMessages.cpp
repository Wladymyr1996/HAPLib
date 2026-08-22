#include <HAPMessages/HAPServiceMessages.hpp>

// The whole point of the capacities chosen in HAP.h: a complete OtaRequest has
// to fit ONE frame. A message that paginated would take a sleeping node two
// report cycles to receive, and could be acted on half-delivered.
static_assert(1 + 1 + (1 + HAP_MAX_SSID_LEN) + (1 + HAP_MAX_PASSPHRASE_LEN) +
                      (1 + HAP_MAX_URL_LEN) <= HAP_MAX_PAYLOAD_SIZE,
              "An OtaRequest at full length must fit one frame's payload.");

void HAPOtaRequest::encode(HAPWriter& writer) const noexcept {
  writer.u8(flags);
  writer.u8(channel);
  writer.text(ssid, HAP_MAX_SSID_LEN);
  writer.text(passphrase, HAP_MAX_PASSPHRASE_LEN);
  writer.text(url, HAP_MAX_URL_LEN);
}

bool HAPOtaRequest::decode(HAPReader& reader) noexcept {
  flags = reader.u8();
  channel = reader.u8();
  reader.text(ssid);
  reader.text(passphrase);
  reader.text(url);
  return reader.ok();
}

size_t HAPOtaRequest::encodedSize() const noexcept {
  return 2 + (1 + ssid.size()) + (1 + passphrase.size()) + (1 + url.size());
}

bool HAPOtaRequest::isQueryOnly() const noexcept {
  return (flags & HAPOtaFlags::QueryOnly) != 0;
}

bool HAPOtaRequest::isActionable() const noexcept {
  // An empty SSID is a node with nothing to join, and an empty URL is a node
  // with nothing to fetch. Both are refused rather than attempted, because the
  // alternative is a battery node sitting with its radio on until its own
  // timeout gives up.
  return !isQueryOnly() && !ssid.empty() && !url.empty();
}

void HAPOtaResponse::encode(HAPWriter& writer) const noexcept {
  writer.u8(static_cast<uint8_t>(result));
  writer.name(version);
}

bool HAPOtaResponse::decode(HAPReader& reader) noexcept {
  result = static_cast<HAPResult>(reader.u8());
  version = reader.name();
  return reader.ok();
}

size_t HAPOtaResponse::encodedSize() const noexcept {
  return 1 + 1 + version.size();
}
