#include "HAPTest.hpp"

#include <HAPITransport/HAPLoopback/HAPLoopback.hpp>
#include <HAPMessages/HAPMessages.hpp>
#include <HAPRouter/HAPRouter.hpp>

#include <cstring>

/**
 * @file HAPTreeTest.cpp
 * @brief A three-node tree inside one process.
 *
 * This is the test the whole transport seam exists for. Gateway, controller and
 * thermometer, each with its own router and its own radio on a shared simulated
 * bus - and the questions two boards can never answer: does a report climb two
 * hops with its originator's id intact, does a command reach its target
 * directly, and does a dead middle node hide the reports of its subtree.
 *
 * The TestNode below is a stand-in for HAPStack (phase 5): a router, a
 * transport, and the twenty lines that connect them. When HAPStack exists these
 * tests move onto it unchanged, because they only ever talk about frames.
 */

namespace {

// The network from Docs/Protocol.md section 8.
//
//   Gateway --1-- Controller --2-- Thermometer
//                            \-4-- Lamp
constexpr uint8_t kControllerIndex = 1;
constexpr uint8_t kThermometerIndex = 2;
constexpr uint8_t kLampIndex = 4;

HAPNodeId idFor(uint8_t id) noexcept {
  const uint8_t mac[HAP_MAC_LEN] = {0x24, 0x6F, 0x28, 0x00, 0x00, id};
  return HAPNodeId::fromMac(mac);
}

/** One frame a node has been handed, kept whole so a test can inspect it. */
struct ReceivedFrame {
  HAPNodeId from;
  uint8_t bytes[HAP_MAX_FRAME_SIZE] = {};
  size_t size = 0;
};

/**
 * @brief A node: a router, a radio, and the rule that connects them.
 *
 * Everything it does with a routing decision is here, and it is the whole of
 * what a node has to do to be part of a tree - which is the point of keeping
 * HAPRouter free of radios and HAPITransport free of routing.
 */
class TestNode {
 public:
  TestNode(uint8_t id, HAPLoopbackBus& bus) noexcept
      : id_(idFor(id)), transport_(id_, bus) {
    transport_.begin();
    transport_.onReceive(
        HAPITransport::Receiver::create<TestNode, &TestNode::onFrame>(*this));
  }

  HAPRouter& router() noexcept { return router_; }
  const HAPNodeId& id() const noexcept { return id_; }
  HAPAddress address() const noexcept { return HAPAddress::fromNodeId(id_); }
  HAPLoopback& transport() noexcept { return transport_; }

  /** @brief Adds a child to the routing table, and this node as its parent. */
  void adopt(uint8_t index, TestNode& child, uint8_t capabilities = HAPCaps::None) noexcept {
    HAPChild record;
    record.index = index;
    record.id = child.id();
    record.capabilities = capabilities;

    router_.addChild(record);

    child.router().setParent(id_, index);
  }

  /**
   * @brief Sends a frame this node is originating, stamped with its own id.
   *
   * A frame for the master climbs through the parent; anything else goes
   * straight to its target.
   */
  bool originate(HAPFrame& frame) noexcept {
    frame.src = id_;

    if (frame.isForMaster()) {
      if (!router_.hasParent()) {
        return false;
      }

      return transmit(router_.parentId(), frame);
    }

    const HAPRoute route = router_.resolveDownstream(frame.dst, id_);
    if (route.action == HAPRouteAction::ToChild) {
      const HAPChild* child = router_.child(route.childIndex);
      return child != nullptr && transmit(child->id, frame);
    }

    if (route.action == HAPRouteAction::ToNode) {
      return transmit(frame.dst, frame);
    }

    return false;
  }

  /** Frames delivered TO this node, in order. */
  const etl::vector<ReceivedFrame, 16>& delivered() const noexcept {
    return delivered_;
  }

  /** Frames this node dropped, with the reason the router gave. */
  const etl::vector<HAPResult, 16>& drops() const noexcept { return drops_; }

  void clear() noexcept {
    delivered_.clear();
    drops_.clear();
  }

 private:
  void onFrame(const HAPAddress& from, const uint8_t* data, size_t size) noexcept {
    HAPFrame frame;
    if (HAPFrame::decode(data, size, frame) != HAPFrameError::None) {
      return;
    }

    const HAPRoute route = router_.route(frame, from.nodeId(), id_);

    switch (route.action) {
      case HAPRouteAction::Deliver: {
        // Re-encoded rather than kept as the buffer it arrived in: the payload
        // of a decoded frame is a VIEW, and that buffer is gone the moment this
        // callback returns.
        ReceivedFrame record;
        record.from = from.nodeId();
        record.size = frame.encode(record.bytes, sizeof(record.bytes));
        if (!delivered_.full()) {
          delivered_.push_back(record);
        }
        break;
      }

      case HAPRouteAction::ToParent:
        transmit(router_.parentId(), frame);
        break;

      case HAPRouteAction::ToChild:
      case HAPRouteAction::ToNode:
        // Only an originated frame goes straight to its target; a node that
        // receives one for somebody else drops it instead.
        break;

      case HAPRouteAction::Drop:
        if (!drops_.full()) {
          drops_.push_back(route.reason);
        }
        break;
    }
  }

  bool transmit(const HAPNodeId& to, const HAPFrame& frame) noexcept {
    uint8_t buffer[HAP_MAX_FRAME_SIZE];
    const size_t size = frame.encode(buffer, sizeof(buffer));

    return size > 0 && transport_.send(HAPAddress::fromNodeId(to), buffer, size);
  }

  HAPNodeId id_;
  HAPLoopback transport_;
  HAPRouter router_;

  etl::vector<ReceivedFrame, 16> delivered_;
  etl::vector<HAPResult, 16> drops_;
};

/** @brief Gateway, controller, thermometer and lamp, wired up and bound. */
struct Tree {
  explicit Tree() noexcept
      : gateway(1, bus), controller(2, bus), thermometer(3, bus), lamp(4, bus) {
    gateway.adopt(kControllerIndex, controller);
    controller.adopt(kThermometerIndex, thermometer, HAPCaps::BatteryPowered);
    controller.adopt(kLampIndex, lamp);
  }

  HAPLoopbackBus bus;
  TestNode gateway;
  TestNode controller;
  TestNode thermometer;
  TestNode lamp;
};

/** @brief A report of one temperature, ready to originate. */
size_t buildReport(uint8_t* payload, size_t capacity, float celsius) noexcept {
  HAPReport report;
  report.descriptorRev = 0x9C4A;
  report.entries.push_back(HAPValueEntry(
      static_cast<uint8_t>(HAPClassId::Thermometer), 0, 0, HValue(celsius)));

  HAPWriter writer(payload, capacity);
  report.encode(writer);
  return writer.ok() ? writer.size() : 0;
}

// -------------------------------------------------------------------------

void testReportClimbsTwoHops() noexcept {
  Tree tree;

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  const size_t payloadSize = buildReport(payload, sizeof(payload), 21.5f);

  HAPFrame report;
  report.type = static_cast<uint8_t>(HAPMessage::Report);
  report.seq = 0x0107;
  report.payload = payload;
  report.payloadSize = static_cast<uint8_t>(payloadSize);

  CHECK(tree.thermometer.originate(report));

  // Two hops, one round each: thermometer to controller, controller to gateway.
  CHECK(tree.bus.pump() == 1);
  CHECK(tree.bus.pump() == 1);
  CHECK(tree.bus.pending() == 0);

  CHECK(tree.gateway.delivered().size() == 1);
  CHECK(tree.controller.delivered().empty());  // It forwarded, it was not the target.

  HAPFrame arrived;
  const ReceivedFrame& record = tree.gateway.delivered()[0];
  CHECK(HAPFrame::decode(record.bytes, record.size, arrived) ==
        HAPFrameError::None);

  // The whole design in one assertion: nobody was told the shape of the
  // network, and the gateway knows who the reading came from because the frame
  // says so - it was relayed, and the source is the originator's, untouched.
  CHECK(arrived.src == tree.thermometer.id());
  CHECK(arrived.isForMaster());
  CHECK(arrived.seq == 0x0107);

  // The last hop it arrived by was the controller.
  CHECK(record.from == tree.controller.id());

  HAPReader reader(arrived.payload, arrived.payloadSize);
  HAPReport parsed;
  CHECK(parsed.decode(reader));
  CHECK(parsed.entries.size() == 1);
  CHECK(parsed.entries[0].value.asFloat() == 21.5f);
}

void testCommandGoesStraightToItsTarget() noexcept {
  Tree tree;

  const HAPWriteRequest request(static_cast<uint8_t>(HAPClassId::Lamp), 0, 0,
                                HValue(true));

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  request.encode(writer);

  // Addressed to the lamp by id: one hop, and the controller between them is
  // not involved.
  HAPFrame command;
  command.type = static_cast<uint8_t>(HAPMessage::WriteRequest);
  command.set(HAPFlags::AckRequested);
  command.seq = 0x0020;
  command.dst = tree.lamp.id();
  command.payload = payload;
  command.payloadSize = static_cast<uint8_t>(writer.size());

  CHECK(tree.gateway.originate(command));
  CHECK(tree.bus.pumpAll() == 1);

  CHECK(tree.lamp.delivered().size() == 1);
  CHECK(tree.controller.delivered().empty());  // Delivers nothing...
  CHECK(tree.controller.transport().receivedCount() == 0);  // ...forwards nothing.
  CHECK(tree.thermometer.delivered().empty());  // The other branch heard nothing.

  HAPFrame arrived;
  const ReceivedFrame& record = tree.lamp.delivered()[0];
  CHECK(HAPFrame::decode(record.bytes, record.size, arrived) ==
        HAPFrameError::None);
  CHECK(arrived.dst == tree.lamp.id());
  CHECK(arrived.src == tree.gateway.id());
  CHECK(arrived.has(HAPFlags::AckRequested));

  HAPReader reader(arrived.payload, arrived.payloadSize);
  const HAPWriteRequest parsed = HAPWriteRequest::decode(reader);
  CHECK(reader.ok());
  CHECK(parsed.value.asBool());
}

void testResponseFindsItsWayBack() noexcept {
  Tree tree;

  const HAPWriteResponse response(HAPResult::Ok,
                                  static_cast<uint8_t>(HAPClassId::Lamp), 0, 0,
                                  HValue(true));

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  response.encode(writer);

  HAPFrame reply;
  reply.type = static_cast<uint8_t>(HAPMessage::WriteResponse);
  reply.seq = 0x0020;
  reply.payload = payload;
  reply.payloadSize = static_cast<uint8_t>(writer.size());

  CHECK(tree.lamp.originate(reply));
  CHECK(tree.bus.pumpAll() == 2);

  HAPFrame arrived;
  const ReceivedFrame& record = tree.gateway.delivered()[0];
  CHECK(HAPFrame::decode(record.bytes, record.size, arrived) ==
        HAPFrameError::None);

  // The reply names its originator itself: the lamp, with the request's seq.
  CHECK(arrived.src == tree.lamp.id());
  CHECK(arrived.seq == 0x0020);
}

void testADeadNodeHidesItsSubtreesReports() noexcept {
  Tree tree;

  // The controller loses power. Reports climb through it, so there is no second
  // way up for anything under it.
  tree.bus.setReachable(tree.gateway.id(), tree.controller.id(), false);
  tree.bus.setReachable(tree.controller.id(), tree.thermometer.id(), false);

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  const size_t payloadSize = buildReport(payload, sizeof(payload), 21.5f);

  HAPFrame report;
  report.type = static_cast<uint8_t>(HAPMessage::Report);
  report.payload = payload;
  report.payloadSize = static_cast<uint8_t>(payloadSize);

  CHECK(tree.thermometer.originate(report));
  CHECK(tree.bus.pumpAll() > 0);
  CHECK(tree.gateway.delivered().empty());
}

void testACommandNoLongerNeedsTheMiddleNode() noexcept {
  Tree tree;

  // The same outage, from the other side: a command is sent to its target
  // directly, so a dead controller does not stand in the way of it.
  tree.bus.setReachable(tree.gateway.id(), tree.controller.id(), false);
  tree.bus.setReachable(tree.controller.id(), tree.thermometer.id(), false);

  HAPFrame command;
  command.type = static_cast<uint8_t>(HAPMessage::ReadRequest);
  command.seq = 0x0030;
  command.dst = tree.thermometer.id();

  CHECK(tree.gateway.originate(command));
  CHECK(tree.bus.pumpAll() == 1);

  CHECK(tree.controller.delivered().empty());
  CHECK(tree.thermometer.delivered().size() == 1);
}

void testACommandToAnUnknownIdIsLostSilently() noexcept {
  Tree tree;

  // Nobody holds this id. The radio took the frame, nothing arrives, and - with
  // no failure report any more - nobody says so either.
  HAPFrame command;
  command.type = static_cast<uint8_t>(HAPMessage::ReadRequest);
  command.dst = idFor(0x77);

  CHECK(tree.gateway.originate(command));

  const size_t delivered = tree.bus.deliveredCount();
  tree.bus.pumpAll();

  CHECK(tree.bus.deliveredCount() == delivered);
  CHECK(tree.controller.drops().empty());
  CHECK(tree.gateway.drops().empty());
  CHECK(tree.gateway.delivered().empty());
}

void testTheOtherBranchIsUndisturbed() noexcept {
  Tree tree;

  // The lamp goes away; the thermometer under the same parent must not care.
  tree.bus.setReachable(tree.controller.id(), tree.lamp.id(), false);

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  const size_t payloadSize = buildReport(payload, sizeof(payload), 19.0f);

  HAPFrame report;
  report.type = static_cast<uint8_t>(HAPMessage::Report);
  report.payload = payload;
  report.payloadSize = static_cast<uint8_t>(payloadSize);

  CHECK(tree.thermometer.originate(report));
  CHECK(tree.bus.pumpAll() == 2);
  CHECK(tree.gateway.delivered().size() == 1);
}

void testAStrangerIsIgnored() noexcept {
  Tree tree;
  TestNode intruder(9, tree.bus);

  // It can transmit - the radio has no opinion - but it is nobody's parent and
  // nobody's child, so a frame for the master goes no further than the router.
  HAPFrame frame;
  frame.type = static_cast<uint8_t>(HAPMessage::ReadRequest);
  frame.src = intruder.id();

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = frame.encode(buffer, sizeof(buffer));
  CHECK(intruder.transport().send(tree.controller.address(), buffer, size));

  CHECK(tree.bus.pumpAll() > 0);
  CHECK(tree.controller.delivered().empty());
  CHECK(tree.controller.drops().size() == 1);
  CHECK(tree.controller.drops()[0] == HAPResult::NotBound);

  // A frame addressed to the controller itself is a different matter: it is
  // delivered whoever sent it. The boundary is the network key, not the sender.
  tree.controller.clear();
  frame.dst = tree.controller.id();
  const size_t addressed = frame.encode(buffer, sizeof(buffer));
  CHECK(intruder.transport().send(tree.controller.address(), buffer, addressed));

  CHECK(tree.bus.pumpAll() > 0);
  CHECK(tree.controller.delivered().size() == 1);
  CHECK(tree.controller.drops().empty());
}

void testAUnicastToNobodyIsLostSilently() noexcept {
  Tree tree;

  // The rule that catches more real bugs than any other: a send reports only
  // that the radio took the frame. An address nobody holds is not an error, and
  // nothing arrives - so a caller that took `true` for delivery is wrong.
  HAPFrame frame;
  frame.src = tree.gateway.id();
  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = frame.encode(buffer, sizeof(buffer));

  const size_t delivered = tree.bus.deliveredCount();

  CHECK(tree.gateway.transport().send(HAPAddress::fromNodeId(idFor(0x77)),
                                      buffer, size));
  CHECK(tree.bus.pending() == 1);
  tree.bus.pumpAll();

  CHECK(tree.bus.pending() == 0);
  CHECK(tree.bus.deliveredCount() == delivered);
}

void testMulticastReachesEveryoneAttached() noexcept {
  Tree tree;
  TestNode newcomer(9, tree.bus);

  // How an unbound node is heard at all: nobody has to know it first.
  HAPFrame announce;
  announce.type = static_cast<uint8_t>(HAPMessage::BindAnnounce);
  announce.src = newcomer.id();

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = announce.encode(buffer, sizeof(buffer));

  CHECK(newcomer.transport().multicast(buffer, size));
  CHECK(tree.bus.pumpAll() > 0);

  // Every node on the bus received it; each router then refused it, because
  // a stranger's frame is the binder's business and not the router's.
  CHECK(tree.gateway.transport().receivedCount() == 1);
  CHECK(tree.controller.transport().receivedCount() == 1);
  CHECK(tree.gateway.drops().size() == 1);
  CHECK(tree.gateway.drops()[0] == HAPResult::NotBound);
}

void testAChainHasNoDepthLimit() noexcept {
  // Six nodes in a chain, more than the old five-hop ceiling allowed, with a
  // report from the bottom of it.
  constexpr size_t kNodes = 6;

  HAPLoopbackBus bus;
  TestNode nodes[kNodes] = {
      TestNode(1, bus), TestNode(2, bus), TestNode(3, bus),
      TestNode(4, bus), TestNode(5, bus), TestNode(6, bus)};

  for (size_t i = 0; i + 1 < kNodes; ++i) {
    nodes[i].adopt(1, nodes[i + 1]);
  }

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  const size_t payloadSize = buildReport(payload, sizeof(payload), 5.0f);

  HAPFrame report;
  report.type = static_cast<uint8_t>(HAPMessage::Report);
  report.payload = payload;
  report.payloadSize = static_cast<uint8_t>(payloadSize);

  CHECK(nodes[kNodes - 1].originate(report));
  CHECK(bus.pumpAll() == kNodes - 1);

  CHECK(nodes[0].delivered().size() == 1);

  HAPFrame arrived;
  const ReceivedFrame& record = nodes[0].delivered()[0];
  CHECK(HAPFrame::decode(record.bytes, record.size, arrived) ==
        HAPFrameError::None);

  // The report reaches the root with the bottom node's id: nothing along the
  // way added to the frame, so there is no longest path to exceed.
  CHECK(arrived.src == nodes[kNodes - 1].id());
}

}  // namespace

void runTreeTests() noexcept {
  HAPTest::begin("Simulated tree");

  testReportClimbsTwoHops();
  testCommandGoesStraightToItsTarget();
  testResponseFindsItsWayBack();
  testADeadNodeHidesItsSubtreesReports();
  testACommandNoLongerNeedsTheMiddleNode();
  testACommandToAnUnknownIdIsLostSilently();
  testTheOtherBranchIsUndisturbed();
  testAStrangerIsIgnored();
  testAUnicastToNobodyIsLostSilently();
  testMulticastReachesEveryoneAttached();
  testAChainHasNoDepthLimit();
}
