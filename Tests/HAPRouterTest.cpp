#include "HAPTest.hpp"

#include <HAPRouter/HAPRouter.hpp>

#include <cstring>

namespace {

const HAPNodeId kGateway = HAPNodeId::fromMac(
    reinterpret_cast<const uint8_t*>("\x24\x6F\x28\x00\x00\x01"));
const HAPNodeId kController = HAPNodeId::fromMac(
    reinterpret_cast<const uint8_t*>("\x24\x6F\x28\x00\x00\x02"));
const HAPNodeId kThermometer = HAPNodeId::fromMac(
    reinterpret_cast<const uint8_t*>("\x24\x6F\x28\x00\x00\x03"));
const HAPNodeId kOther = HAPNodeId::fromMac(
    reinterpret_cast<const uint8_t*>("\x24\x6F\x28\x00\x00\x04"));
const HAPNodeId kStranger = HAPNodeId::fromMac(
    reinterpret_cast<const uint8_t*>("\xAA\xBB\xCC\xDD\xEE\xFF"));

HAPChild makeChild(uint8_t index, const HAPNodeId& mac,
                   uint8_t capabilities = HAPCaps::None) noexcept {
  HAPChild child;
  child.index = index;
  child.id = mac;
  child.capabilities = capabilities;
  return child;
}

/** A frame for the master, from `src`. */
HAPFrame frameForMaster(const HAPNodeId& src) noexcept {
  HAPFrame frame;
  frame.type = static_cast<uint8_t>(HAPMessage::Report);
  frame.src = src;
  return frame;
}

/** A frame addressed to `dst`, from `src`. */
HAPFrame frameTo(const HAPNodeId& dst, const HAPNodeId& src) noexcept {
  HAPFrame frame;
  frame.type = static_cast<uint8_t>(HAPMessage::WriteRequest);
  frame.dst = dst;
  frame.src = src;
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
  CHECK(router.childById(kController) != nullptr);
  CHECK(router.childById(kStranger) == nullptr);

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
  // The child table is a configured limit. A root spends all of it on children;
  // anything bound spends one slot upward.
  HAPRouter root;
  CHECK(root.childCapacity() == HAP_MAX_CHILDREN);

  HAPRouter bound;
  bound.setParent(kGateway, 1);
  CHECK(bound.childCapacity() == HAP_MAX_CHILDREN - 1);

  for (uint8_t i = 1; i < HAP_MAX_CHILDREN; ++i) {
    HAPNodeId mac = kGateway;
    mac.bytes[5] = i;
    CHECK(bound.addChild(makeChild(i, mac)));
  }

  CHECK(bound.childCount() == HAP_MAX_CHILDREN - 1);
  CHECK(bound.isFull());
  CHECK(bound.freeChildIndex() == 0);

  // The sixth is refused HERE, where it can be logged.
  HAPNodeId extra = kGateway;
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
    HAPNodeId mac = kGateway;
    mac.bytes[5] = i;
    CHECK(router.addChild(makeChild(i, mac)));
  }

  CHECK(router.isFull());
  CHECK(router.freeChildIndex() == 0);

  HAPNodeId extra = kGateway;
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

  // The originator may be the child or something far below it: the frame is the
  // same either way, and the router never touches it.
  const HAPFrame frame = frameForMaster(kOther);
  const HAPRoute route = controller.route(frame, kThermometer, kController);

  CHECK(route.action == HAPRouteAction::ToParent);
  CHECK(frame.src == kOther);
  CHECK(frame.dst.isZero());
}

void testUpstreamArrivingAtTheRoot() noexcept {
  HAPRouter gateway;  // No parent: this is the root.
  CHECK(gateway.addChild(makeChild(1, kController)));

  const HAPFrame frame = frameForMaster(kOther);
  const HAPRoute route = gateway.route(frame, kController, kGateway);

  // The root does not forward - the frame has arrived, and its source is the
  // id to answer.
  CHECK(route.action == HAPRouteAction::Deliver);
  CHECK(frame.src == kOther);
}

void testFrameForThisNode() noexcept {
  HAPRouter controller;
  controller.setParent(kGateway, 1);
  CHECK(controller.addChild(makeChild(2, kThermometer)));

  // From the parent, from a child and from a node it has never met: addressed to
  // it, so delivered - the network key is the boundary, not the sender.
  const HAPFrame frame = frameTo(kController, kGateway);
  CHECK(controller.route(frame, kGateway, kController).action ==
        HAPRouteAction::Deliver);
  CHECK(controller.route(frame, kThermometer, kController).action ==
        HAPRouteAction::Deliver);
  CHECK(controller.route(frame, kStranger, kController).action ==
        HAPRouteAction::Deliver);

  // A leaf with only a parent counts as being in a tree too.
  HAPRouter thermometer;
  thermometer.setParent(kController, 2);
  CHECK(thermometer.route(frameTo(kThermometer, kGateway), kStranger,
                          kThermometer).action == HAPRouteAction::Deliver);

  // A root with children likewise.
  HAPRouter gateway;
  CHECK(gateway.addChild(makeChild(1, kController)));
  CHECK(gateway.route(frameTo(kGateway, kOther), kStranger, kGateway).action ==
        HAPRouteAction::Deliver);
}

void testFrameForAnUnboundNode() noexcept {
  // In no tree at all: everything not for the binder is dropped, as before.
  HAPRouter loner;
  const HAPRoute route = loner.route(frameTo(kThermometer, kGateway), kGateway,
                                     kThermometer);
  CHECK(route.isDrop());
  CHECK(route.reason == HAPResult::NotBound);
}

void testNothingForwardsDownward() noexcept {
  HAPRouter controller;
  controller.setParent(kGateway, 1);
  CHECK(controller.addChild(makeChild(4, kThermometer)));

  // For some other node, even one of its own children: the originator sends it
  // there directly, so a relay has nothing to do.
  const HAPFrame forChild = frameTo(kThermometer, kGateway);
  const HAPRoute route = controller.route(forChild, kGateway, kController);
  CHECK(route.isDrop());
  CHECK(route.reason == HAPResult::BadRequest);

  const HAPFrame forOther = frameTo(kOther, kGateway);
  CHECK(controller.route(forOther, kGateway, kController).reason ==
        HAPResult::BadRequest);
  CHECK(controller.route(forOther, kThermometer, kController).reason ==
        HAPResult::BadRequest);
}

void testFramesFromStrangers() noexcept {
  HAPRouter controller;
  controller.setParent(kGateway, 1);
  CHECK(controller.addChild(makeChild(2, kThermometer)));

  // Not for this node, and from nobody it knows: the binder's business.
  const HAPFrame forMaster = frameForMaster(kOther);
  CHECK(controller.route(forMaster, kStranger, kController).reason ==
        HAPResult::NotBound);

  const HAPFrame forOther = frameTo(kOther, kGateway);
  CHECK(controller.route(forOther, kStranger, kController).reason ==
        HAPResult::NotBound);

  // A frame for the master arriving from the parent is a node forwarding the
  // wrong way.
  const HAPFrame fromAbove = frameForMaster(kGateway);
  CHECK(controller.route(fromAbove, kGateway, kController).reason ==
        HAPResult::BadRequest);
}

void testRouteLeavesTheFrameAlone() noexcept {
  HAPRouter controller;
  controller.setParent(kGateway, 1);
  CHECK(controller.addChild(makeChild(2, kThermometer)));

  const HAPFrame frame = frameForMaster(kOther);
  uint8_t before[HAP_MAX_FRAME_SIZE];
  uint8_t after[HAP_MAX_FRAME_SIZE];
  const size_t size = frame.encode(before, sizeof(before));

  controller.route(frame, kThermometer, kController);

  CHECK(frame.encode(after, sizeof(after)) == size);
  CHECK(std::memcmp(before, after, size) == 0);
}

void testOriginatingLocally() noexcept {
  HAPRouter gateway;
  CHECK(gateway.addChild(makeChild(1, kController)));

  // A direct child goes through the child table - that is where a sleeping one
  // is queued for.
  const HAPRoute direct = gateway.resolveDownstream(kController, kGateway);
  CHECK(direct.action == HAPRouteAction::ToChild);
  CHECK(direct.childIndex == 1);

  // Anything else is sent straight to it.
  CHECK(gateway.resolveDownstream(kOther, kGateway).action ==
        HAPRouteAction::ToNode);

  // Itself is delivered; "the master" has no way down.
  CHECK(gateway.resolveDownstream(kGateway, kGateway).action ==
        HAPRouteAction::Deliver);

  const HAPRoute up = gateway.resolveDownstream(HAPNodeId::zero(), kGateway);
  CHECK(up.isDrop());
  CHECK(up.reason == HAPResult::BadRequest);
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
  testFrameForThisNode();
  testFrameForAnUnboundNode();
  testNothingForwardsDownward();
  testFramesFromStrangers();
  testRouteLeavesTheFrameAlone();
  testOriginatingLocally();
  testBatteryCapability();
  testParentIsKnown();
}
