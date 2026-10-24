#include <HAPAddress/HAPAddress.hpp>

#include <cstdio>
#include <cstring>

HAPAddress HAPAddress::zero() noexcept {
  return HAPAddress();
}

HAPAddress HAPAddress::master() noexcept {
  // ff01:: - the shape of a multicast scope, never a unicast address a
  // transport hands out, so it cannot be mistaken for a node.
  HAPAddress address;
  address.bytes[0] = 0xFF;
  address.bytes[1] = 0x01;
  return address;
}

HAPAddress HAPAddress::broadcast() noexcept {
  HAPAddress address;
  std::memset(address.bytes, 0xFF, sizeof(address.bytes));
  return address;
}

HAPAddress HAPAddress::fromBytes(const uint8_t* data) noexcept {
  HAPAddress address;

  if (data != nullptr) {
    std::memcpy(address.bytes, data, sizeof(address.bytes));
  }

  return address;
}

HAPAddress HAPAddress::fromMac(const uint8_t* mac) noexcept {
  HAPAddress address;

  if (mac != nullptr) {
    std::memcpy(address.bytes, mac, HAP_MAC_LEN);
  }

  return address;
}

HAPAddress HAPAddress::fromNodeId(const HAPNodeId& id) noexcept {
  uint8_t mac[HAP_MAC_LEN];
  id.toMac(mac);
  return fromMac(mac);
}

void HAPAddress::toMac(uint8_t* mac) const noexcept {
  if (mac != nullptr) {
    std::memcpy(mac, bytes, HAP_MAC_LEN);
  }
}

HAPNodeId HAPAddress::nodeId() const noexcept {
  return HAPNodeId::fromMac(bytes);
}

bool HAPAddress::isZero() const noexcept {
  return *this == zero();
}

bool HAPAddress::isMaster() const noexcept {
  return *this == master();
}

bool HAPAddress::isBroadcast() const noexcept {
  return *this == broadcast();
}

bool HAPAddress::operator==(const HAPAddress& other) const noexcept {
  return std::memcmp(bytes, other.bytes, sizeof(bytes)) == 0;
}

bool HAPAddress::operator!=(const HAPAddress& other) const noexcept {
  return !(*this == other);
}

etl::string<39> HAPAddress::toString() const noexcept {
  char text[40];
  size_t length = 0;

  for (size_t i = 0; i < sizeof(bytes); i += 2) {
    length += std::snprintf(text + length, sizeof(text) - length,
                            i == 0 ? "%02x%02x" : ":%02x%02x", bytes[i],
                            bytes[i + 1]);
  }

  etl::string<39> result(text);
  return result;
}
