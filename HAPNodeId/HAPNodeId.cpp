#include <HAPNodeId/HAPNodeId.hpp>

#include <cstdio>
#include <cstring>

HAPNodeId HAPNodeId::zero() noexcept {
  return HAPNodeId();
}

HAPNodeId HAPNodeId::fromBytes(const uint8_t* data) noexcept {
  HAPNodeId id;

  if (data != nullptr) {
    std::memcpy(id.bytes, data, sizeof(id.bytes));
  }

  return id;
}

HAPNodeId HAPNodeId::fromMac(const uint8_t* mac) noexcept {
  HAPNodeId id;

  if (mac == nullptr) {
    return id;
  }

  const uint8_t none[HAP_MAC_LEN] = {};
  if (std::memcmp(mac, none, HAP_MAC_LEN) == 0) {
    return id;
  }

  std::memcpy(id.bytes, mac, 3);
  id.bytes[3] = 0xFF;
  id.bytes[4] = 0xFE;
  std::memcpy(id.bytes + 5, mac + 3, 3);
  return id;
}

void HAPNodeId::toMac(uint8_t* mac) const noexcept {
  if (mac == nullptr) {
    return;
  }

  if (isZero()) {
    std::memset(mac, 0, HAP_MAC_LEN);
    return;
  }

  std::memcpy(mac, bytes, 3);
  std::memcpy(mac + 3, bytes + 5, 3);
}

bool HAPNodeId::isZero() const noexcept {
  return *this == zero();
}

bool HAPNodeId::operator==(const HAPNodeId& other) const noexcept {
  return std::memcmp(bytes, other.bytes, sizeof(bytes)) == 0;
}

bool HAPNodeId::operator!=(const HAPNodeId& other) const noexcept {
  return !(*this == other);
}

bool HAPNodeId::operator<(const HAPNodeId& other) const noexcept {
  return std::memcmp(bytes, other.bytes, sizeof(bytes)) < 0;
}

etl::string<23> HAPNodeId::toString() const noexcept {
  char text[24];
  std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                bytes[6], bytes[7]);

  etl::string<23> result(text);
  return result;
}
