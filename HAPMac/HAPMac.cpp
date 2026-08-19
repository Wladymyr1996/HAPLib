#include <HAPMac/HAPMac.hpp>

#include <cstdio>
#include <cstring>

HAPMac HAPMac::zero() noexcept {
  return HAPMac();
}

HAPMac HAPMac::broadcast() noexcept {
  HAPMac mac;
  std::memset(mac.bytes, 0xFF, sizeof(mac.bytes));
  return mac;
}

HAPMac HAPMac::fromBytes(const uint8_t* data) noexcept {
  HAPMac mac;

  if (data != nullptr) {
    std::memcpy(mac.bytes, data, sizeof(mac.bytes));
  }

  return mac;
}

bool HAPMac::isZero() const noexcept {
  return *this == zero();
}

bool HAPMac::isBroadcast() const noexcept {
  return *this == broadcast();
}

bool HAPMac::operator==(const HAPMac& other) const noexcept {
  return std::memcmp(bytes, other.bytes, sizeof(bytes)) == 0;
}

bool HAPMac::operator!=(const HAPMac& other) const noexcept {
  return !(*this == other);
}

etl::string<17> HAPMac::toString() const noexcept {
  char text[18];
  std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", bytes[0],
                bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);

  etl::string<17> result(text);
  return result;
}
