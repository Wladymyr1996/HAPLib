#include "HAPTest.hpp"

#include <HAPRouter/HAPRouter.hpp>

namespace {

const HAPMac kGateway = HAPMac::fromBytes(
    reinterpret_cast<const uint8_t*>("\x24\x6F\x28\x00\x00\x01"));
const HAPMac kController = HAPMac::fromBytes(
    reinterpret_cast<const uint8_t*>("\x24\x6F\x28\x00\x00\x02"));
const HAPMac kThermometer = HAPMac::fromBytes(
    reinterpret_cast<const uint8_t*>("\x24\x6F\x28\x00\x00\x03"));
const HAPMac kStranger = HAPMac::fromBytes(
    reinterpret_cast<const uint8_t*>("\xAA\xBB\xCC\xDD\xEE\xFF"));

HAPChild makeChild(uint8_t index, const HAPMac& mac,
                   uint8_t capabilities = HAPCaps::None) noexcept {
  HAPChild child;
  child.index = index;
  child.mac = mac;
  child.capabilities = capabilities;
  return child;
}

HAPFrame upstreamFrame() noexcept {
  HAPFrame frame;
  frame.type = static_cast<uint8_t>(HAPMessage::Report);
  frame.set(HAPFlags::Upstream);
  return frame;
}

HAPFrame downstreamFrame(const HAPPath& dest) noexcept {
  HAPFrame frame;
  frame.type = static_cast<uint8_t>(HAPMessage::WriteRequest);
  frame.dest = dest;
  return frame;
}

void testChildBookkeeping() noexcept {
  HAPRouter router;

  CHECK(router.isRoot());
  CHECK(!router.hasParent());
  CHECK(router.childCount() == 0);
  CHECK(router.freeChildIndex() == 1);

  CHECK(router.addChild(makeChild(1, kController)));
  CHECK(router.childCount() == 1);
  CHECK(router.freeChildIndex() == 2);
  CHECK(router.child(1) != nullptr);
  CHECK(router.childByMac(kController) != nullptr);
  CHECK(router.childByMac(kStranger) == nullptr);

  // An index already in use, and a device already present under another index -
  // the second would make one node visible by two paths.
  CHECK(!router.addChild(makeChild(1, kThermometer)));
  CHECK(!router.addChild(makeChild(2, kController)));

  // Zero is padding and anything past the ceiling is not a child index.
  CHECK(!router.addChild(makeChild(0, kThermometer)));
  CHECK(!router.addChild(makeChild(HAP_MAX_CHILDREN + 1, kThermometer)));

  CHECK(router.removeChild(1));
  CHECK(!router.removeChild(1));
  CHECK(router.childCount() == 0);
}

void testTheParentCostsAChildSlot() noexcept {
  // ESP-NOW holds six ENCRYPTED peers and every HAP link is encrypted, the one
  // going up included. A root spends all six on children; anything bound spends
  // one of them upward.
  HAPRouter root;
  CHECK(root.childCapacity() == HAP_MAX_CHILDREN);

  HAPRouter bound;
  bound.setParent(kGateway, 1);
  CHECK(bound.childCapacity() == HAP_MAX_CHILDREN - 1);

  for (uint8_t i = 1; i < HAP_MAX_CHILDREN; ++i) {
    HAPMac mac = kGateway;
    mac.bytes[5] = i;
    CHECK(bound.addChild(makeChild(i, mac)));
  }

  CHECK(bound.childCount() == HAP_MAX_CHILDREN - 1);
  CHECK(bound.isFull());
  CHECK(bound.freeChildIndex() == 0);

  // The sixth is refused HERE, where it can be logged - rather than on the
  // radio, where the acceptance goes out, the peer cannot be made encrypted,
  // the confirmation is dropped below the protocol, and the only symptom is
  // "nobody confirmed".
  HAPMac extra = kGateway;
  extra.bytes[5] = HAP_MAX_CHILDREN;
  CHECK(!bound.addChild(makeChild(HAP_MAX_CHILDREN, extra)));

  // And a node that is freed gets the slot back.
  bound.clearParent();
  CHECK(!bound.isFull());
  CHECK(bound.freeChildIndex() == HAP_MAX_CHILDREN);
  CHECK(bound.addChild(makeChild(HAP_MAX_CHILDREN, extra)));
}

void testIndexReuse() noexcept {
  HAPRouter router;

  for (uint8_t i = 1; i <= HAP_MAX_CHILDREN; ++i) {
    HAPMac mac = kGateway;
    mac.bytes[5] = i;
    CHECK(router.addChild(makeChild(i, mac)));
  }

  CHECK(router.isFull());
  CHECK(router.freeChildIndex() == 0);

  HAPMac extra = kGateway;
  extra.bytes[5] = 0x99;
  CHECK(!router.addChild(makeChild(1, extra)));

  // A replaced device takes the free slot back, so a network keeps the same
  // numbering however often it is rebuilt.
  CHECK(router.removeChild(3));
  CHECK(router.freeChildIndex() == 3);
}

void testUpstreamFromChild() noexcept {
  HAPRouter controller;
  controller.setParent(kGateway, 1);
  CHECK(controller.addChild(makeChild(2, kThermometer)));
  CHECK(controller.hasParent());
  CHECK(!controller.isRoot());
  CHECK(controller.indexAtParent() == 1);

  HAPFrame frame = upstreamFrame();
  const HAPRoute route = controller.route(frame, kThermometer);

  CHECK(route.action == HAPRouteAction::ToParent);
  CHECK(frame.src.length() == 1);
  CHECK(frame.src.hop(0) == 2);
}

void testUpstreamArrivingAtTheRoot() noexcept {
  HAPRouter gateway;  // No parent: this is the root.
  CHECK(gateway.addChild(makeChild(1, kController)));

  HAPFrame frame = upstreamFrame();
  const HAPRoute route = gateway.route(frame, kController);

  // The root does not forward - the frame has arrived, and its source path is
  // now the address to answer.
  CHECK(route.action == HAPRouteAction::Deliver);
  CHECK(frame.src.length() == 1);
  CHECK(frame.src.hop(0) == 1);
}

void testDownstreamForwarding() noexcept {
  HAPRouter controller;
  controller.setParent(kGateway, 1);
  CHECK(controller.addChild(makeChild(4, kThermometer)));

  const uint8_t hops[] = {4};
  HAPFrame frame = downstreamFrame(HAPPath::fromBytes(hops, 1));

  const HAPRoute route = controller.route(frame, kGateway);
  CHECK(route.action == HAPRouteAction::ToChild);
  CHECK(route.childIndex == 4);

  // The hop that named the child is eaten, so what the child receives is
  // addressed relative to itself.
  CHECK(frame.dest.isEmpty());
}

void testDownstreamArriving() noexcept {
  HAPRouter thermometer;
  thermometer.setParent(kController, 2);

  HAPFrame frame = downstreamFrame(HAPPath());
  const HAPRoute route = thermometer.route(frame, kController);

  CHECK(route.action == HAPRouteAction::Deliver);
}

void testNoSuchChild() noexcept {
  HAPRouter controller;
  controller.setParent(kGateway, 1);
  CHECK(controller.addChild(makeChild(2, kThermometer)));

  const uint8_t hops[] = {5};  // Nothing is child 5.
  HAPFrame frame = downstreamFrame(HAPPath::fromBytes(hops, 1));

  const HAPRoute route = controller.route(frame, kGateway);
  CHECK(route.isDrop());
  CHECK(route.reason == HAPResult::NoSuchChild);

  // Which hop failed, so the caller can name it in a RouteError rather than
  // leaving the originator with a timeout.
  CHECK(route.childIndex == 5);
}

void testFramesFromStrangers() noexcept {
  HAPRouter controller;
  controller.setParent(kGateway, 1);
  CHECK(controller.addChild(makeChild(2, kThermometer)));

  HAPFrame upstream = upstreamFrame();
  CHECK(controller.route(upstream, kStranger).reason == HAPResult::NotBound);

  HAPFrame downstream = downstreamFrame(HAPPath());
  CHECK(controller.route(downstream, kStranger).reason == HAPResult::NotBound);

  // A downstream frame from a child is a node forwarding the wrong way.
  HAPFrame wrongWay = downstreamFrame(HAPPath());
  CHECK(controller.route(wrongWay, kThermometer).reason == HAPResult::BadRequest);

  // And an upstream frame from the parent is the same mistake mirrored.
  HAPFrame fromAbove = upstreamFrame();
  CHECK(controller.route(fromAbove, kGateway).reason == HAPResult::BadRequest);
}

void testLoopGuard() noexcept {
  HAPRouter controller;
  controller.setParent(kGateway, 1);
  CHECK(controller.addChild(makeChild(2, kThermometer)));

  HAPFrame frame = upstreamFrame();
  const uint8_t full[] = {1, 1, 1, 1, 1};
  frame.src = HAPPath::fromBytes(full, HAP_MAX_DEPTH);

  // Five hops already: this has climbed further than the tree may be deep, so
  // it is circulating rather than travelling.
  const HAPRoute route = controller.route(frame, kThermometer);
  CHECK(route.isDrop());
  CHECK(frame.src.length() == HAP_MAX_DEPTH);
}

void testOriginatingLocally() noexcept {
  HAPRouter gateway;
  CHECK(gateway.addChild(makeChild(1, kController)));

  // A root addressing 1.4 resolves the first hop itself; what it emits carries
  // only the remainder, exactly as Docs/Protocol.md section 8.3 shows.
  const uint8_t hops[] = {1, 4};
  HAPFrame frame = downstreamFrame(HAPPath::fromBytes(hops, 2));

  const HAPRoute route = gateway.resolveDownstream(frame);
  CHECK(route.action == HAPRouteAction::ToChild);
  CHECK(route.childIndex == 1);
  CHECK(frame.dest.length() == 1);
  CHECK(frame.dest.hop(0) == 4);
}

void testBatteryCapability() noexcept {
  const HAPChild mains = makeChild(1, kController);
  const HAPChild battery = makeChild(2, kThermometer, HAPCaps::BatteryPowered);

  CHECK(!mains.isBatteryPowered());
  CHECK(battery.isBatteryPowered());
}

void testParentIsKnown() noexcept {
  HAPRouter router;
  CHECK(!router.isKnown(kGateway));

  router.setParent(kGateway, 1);
  CHECK(router.isKnown(kGateway));
  CHECK(!router.isKnown(kThermometer));

  CHECK(router.addChild(makeChild(2, kThermometer)));
  CHECK(router.isKnown(kThermometer));

  // A factory reset frees the node, and is the only way to change parent.
  router.clearParent();
  CHECK(router.isRoot());
  CHECK(!router.isKnown(kGateway));
}

}  // namespace

void runRouterTests() noexcept {
  HAPTest::begin("HAPRouter");

  testChildBookkeeping();
  testTheParentCostsAChildSlot();
  testIndexReuse();
  testUpstreamFromChild();
  testUpstreamArrivingAtTheRoot();
  testDownstreamForwarding();
  testDownstreamArriving();
  testNoSuchChild();
  testFramesFromStrangers();
  testLoopGuard();
  testOriginatingLocally();
  testBatteryCapability();
  testParentIsKnown();
}
