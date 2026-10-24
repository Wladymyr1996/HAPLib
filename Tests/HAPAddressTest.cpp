#include "HAPTest.hpp"

#include <HAPAddress/HAPAddress.hpp>
#include <HAPNodeId/HAPNodeId.hpp>

#include <cstring>

namespace {

const uint8_t kMac[HAP_MAC_LEN] = {0x24, 0x6F, 0x28, 0xAA, 0xBB, 0xCC};

void testNodeIdFromMac() noexcept {
  const HAPNodeId id = HAPNodeId::fromMac(kMac);

  // An EUI-48 becomes an EUI-64 by inserting FF FE in the middle.
  CHECK(std::strcmp(id.toString().c_str(), "24:6f:28:ff:fe:aa:bb:cc") == 0);
  CHECK(!id.isZero());

  uint8_t back[HAP_MAC_LEN];
  id.toMac(back);
  CHECK(std::memcmp(back, kMac, HAP_MAC_LEN) == 0);
}

void testZeroSurvivesTheMacForm() noexcept {
  const uint8_t none[HAP_MAC_LEN] = {};

  CHECK(HAPNodeId::fromMac(none).isZero());
  CHECK(HAPNodeId::fromMac(nullptr).isZero());
  CHECK(HAPNodeId::fromBytes(nullptr).isZero());

  uint8_t back[HAP_MAC_LEN];
  std::memset(back, 0x55, sizeof(back));
  HAPNodeId::zero().toMac(back);
  CHECK(std::memcmp(back, none, HAP_MAC_LEN) == 0);
}

void testNodeIdOrdering() noexcept {
  const uint8_t low[HAP_NODE_ID_LEN] = {0, 0, 0, 0, 0, 0, 0, 0xFF};
  const uint8_t high[HAP_NODE_ID_LEN] = {0, 0, 0, 0, 0, 0, 1, 0};

  const HAPNodeId a = HAPNodeId::fromBytes(low);
  const HAPNodeId b = HAPNodeId::fromBytes(high);

  // Big-endian: the earlier byte is the more significant one.
  CHECK(a < b);
  CHECK(!(b < a));
  CHECK(!(a < a));
  CHECK(a != b);
  CHECK(a == HAPNodeId::fromBytes(low));
}

void testSpecialAddresses() noexcept {
  CHECK(HAPAddress().isZero());
  CHECK(HAPAddress::master().isMaster());
  CHECK(HAPAddress::broadcast().isBroadcast());

  // Three different things: none of them may be taken for another.
  CHECK(HAPAddress::master() != HAPAddress::zero());
  CHECK(HAPAddress::master() != HAPAddress::broadcast());
  CHECK(HAPAddress::zero() != HAPAddress::broadcast());
  CHECK(!HAPAddress::fromMac(kMac).isMaster());
}

void testBridgeRoundTrip() noexcept {
  const HAPNodeId id = HAPNodeId::fromMac(kMac);
  const HAPAddress address = HAPAddress::fromNodeId(id);

  CHECK(address == HAPAddress::fromMac(kMac));
  CHECK(address.nodeId() == id);

  uint8_t mac[HAP_MAC_LEN];
  address.toMac(mac);
  CHECK(std::memcmp(mac, kMac, HAP_MAC_LEN) == 0);

  CHECK(HAPAddress::fromNodeId(HAPNodeId::zero()).isZero());
  CHECK(HAPAddress::zero().nodeId().isZero());
}

void testAddressText() noexcept {
  CHECK(std::strcmp(HAPAddress::master().toString().c_str(),
                    "ff01:0000:0000:0000:0000:0000:0000:0000") == 0);
}

}  // namespace

void runAddressTests() noexcept {
  HAPTest::begin("HAPNodeId and HAPAddress");

  testNodeIdFromMac();
  testZeroSurvivesTheMacForm();
  testNodeIdOrdering();
  testSpecialAddresses();
  testBridgeRoundTrip();
  testAddressText();
}
