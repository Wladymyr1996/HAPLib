#include "HAPTest.hpp"

#include <HAPPath/HAPPath.hpp>

#include <cstring>

namespace {

/** The tree from Docs/Protocol.md section 8, as hop indices. */
const uint8_t kGatewayToThermometer[] = {1, 2};

void testEmpty() noexcept {
  HAPPath path;

  CHECK(path.isEmpty());
  CHECK(path.length() == 0);
  CHECK(!path.isFull());
  CHECK(path.first() == 0);
  CHECK(path.hop(0) == 0);
  CHECK(std::strcmp(path.toString().c_str(), ".") == 0);

  // An empty path never forwards: the frame holding it had arrived.
  CHECK(!path.shift());
}

void testFromBytes() noexcept {
  const HAPPath path = HAPPath::fromBytes(kGatewayToThermometer, 2);

  CHECK(path.length() == 2);
  CHECK(path.hop(0) == 1);
  CHECK(path.hop(1) == 2);
  CHECK(path.first() == 1);
  CHECK(std::strcmp(path.toString().c_str(), "1.2") == 0);

  // Past the length there is nothing, and the wire form is zero-padded so two
  // equal addresses always encode identically.
  CHECK(path.hop(2) == 0);
  CHECK(path.bytes()[2] == 0);
  CHECK(path.bytes()[HAP_MAX_DEPTH - 1] == 0);
}

void testIllegalPaths() noexcept {
  const uint8_t tooDeep[] = {1, 2, 3, 4, 5, 6};
  CHECK(HAPPath::fromBytes(tooDeep, 6).isEmpty());

  // Zero is padding and 0xFF is reserved, so neither can be a child index.
  const uint8_t withZero[] = {1, 0, 2};
  CHECK(HAPPath::fromBytes(withZero, 3).isEmpty());

  const uint8_t withReserved[] = {1, 0xFF};
  CHECK(HAPPath::fromBytes(withReserved, 2).isEmpty());

  CHECK(HAPPath::fromBytes(nullptr, 2).isEmpty());
  CHECK(HAPPath::fromBytes(nullptr, 0).isEmpty());
}

void testDownstreamShift() noexcept {
  // Docs/Protocol.md section 8.3: the gateway addresses 1.4, and each node eats
  // the hop that named its own child.
  HAPPath path = HAPPath::fromBytes(kGatewayToThermometer, 2);

  CHECK(path.first() == 1);
  CHECK(path.shift());
  CHECK(path.length() == 1);
  CHECK(path.first() == 2);
  CHECK(std::strcmp(path.toString().c_str(), "2") == 0);

  CHECK(path.shift());
  CHECK(path.isEmpty());
  CHECK(!path.shift());
}

void testUpstreamPrepend() noexcept {
  // Docs/Protocol.md section 8.2: the source path builds itself on the way up,
  // and at every node it is the downward path from THAT node to the origin.
  HAPPath path;

  CHECK(path.prepend(2));  // At the controller: arrived from child 2.
  CHECK(path.length() == 1);
  CHECK(std::strcmp(path.toString().c_str(), "2") == 0);

  CHECK(path.prepend(1));  // At the gateway: arrived from child 1.
  CHECK(path.length() == 2);
  CHECK(std::strcmp(path.toString().c_str(), "1.2") == 0);
  CHECK(path == HAPPath::fromBytes(kGatewayToThermometer, 2));
}

void testLoopGuard() noexcept {
  HAPPath path;

  for (uint8_t i = 0; i < HAP_MAX_DEPTH; ++i) {
    CHECK(path.prepend(1));
  }

  CHECK(path.isFull());

  // A frame that has climbed further than the tree may be deep has nowhere
  // legitimate left to go. Refusing the prepend is what stops it circulating.
  CHECK(!path.prepend(1));
  CHECK(!path.append(1));
  CHECK(path.length() == HAP_MAX_DEPTH);
}

void testAppend() noexcept {
  // How a master names a grandchild it has just been told about: the path the
  // notice arrived by, plus the child index inside it.
  HAPPath path;
  CHECK(path.append(1));
  CHECK(path.append(2));
  CHECK(std::strcmp(path.toString().c_str(), "1.2") == 0);

  CHECK(!path.append(0));
  CHECK(!path.append(0xFF));
  CHECK(path.length() == 2);
}

void testComparison() noexcept {
  const HAPPath a = HAPPath::fromBytes(kGatewayToThermometer, 2);
  HAPPath b;
  b.append(1);
  b.append(2);

  CHECK(a == b);
  CHECK(!(a != b));

  b.shift();
  CHECK(a != b);

  HAPPath cleared = a;
  cleared.clear();
  CHECK(cleared.isEmpty());
  CHECK(cleared.bytes()[0] == 0);
}

void testWidestText() noexcept {
  const uint8_t widest[] = {254, 254, 254, 254, 254};
  const HAPPath path = HAPPath::fromBytes(widest, HAP_MAX_DEPTH);

  CHECK(std::strcmp(path.toString().c_str(), "254.254.254.254.254") == 0);
  CHECK(path.toString().size() == HAP_PATH_TEXT_LEN);
}

}  // namespace

void runPathTests() noexcept {
  HAPTest::begin("HAPPath");

  testEmpty();
  testFromBytes();
  testIllegalPaths();
  testDownstreamShift();
  testUpstreamPrepend();
  testLoopGuard();
  testAppend();
  testComparison();
  testWidestText();
}
