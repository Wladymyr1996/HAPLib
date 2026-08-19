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
 * hops with the right source path, does a command descend, does a dead middle
 * node hide its subtree, and does the loop guard hold.
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
constexpr uint8_t kChannel = 1;

HAPMac macFor(uint8_t id) noexcept {
  HAPMac mac;
  mac.bytes[0] = 0x24;
  mac.bytes[1] = 0x6F;
  mac.bytes[2] = 0x28;
  mac.bytes[5] = id;
  return mac;
}

/** One frame a node has been handed, kept whole so a test can inspect it. */
struct ReceivedFrame {
  HAPMac from;
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
      : mac_(macFor(id)), transport_(mac_, bus) {
    transport_.begin(kChannel);
    transport_.onReceive(
        HAPITransport::Receiver::create<TestNode, &TestNode::onFrame>(*this));
  }

  HAPRouter& router() noexcept { return router_; }
  const HAPMac& mac() const noexcept { return mac_; }
  HAPLoopback& transport() noexcept { return transport_; }

  /** @brief Adds a child, both to the routing table and to the radio. */
  void adopt(uint8_t index, TestNode& child, uint8_t capabilities = HAPCaps::None) noexcept {
    HAPChild record;
    record.index = index;
    record.mac = child.mac();
    record.capabilities = capabilities;

    router_.addChild(record);
    transport_.addPeer(child.mac(), nullptr);

    child.router().setParent(mac_, index);
    child.transport().addPeer(mac_, nullptr);
  }

  /** @brief Sends a frame this node is originating. */
  bool originate(HAPFrame& frame) noexcept {
    if (frame.isUpstream()) {
      if (!router_.hasParent()) {
        return false;
      }

      return transmit(router_.parentMac(), frame);
    }

    const HAPRoute route = router_.resolveDownstream(frame);
    if (route.action == HAPRouteAction::ToChild) {
      const HAPChild* child = router_.child(route.childIndex);
      return child != nullptr && transmit(child->mac, frame);
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
  void onFrame(const HAPMac& from, const uint8_t* data, size_t size) noexcept {
    HAPFrame frame;
    if (HAPFrame::decode(data, size, frame) != HAPFrameError::None) {
      return;
    }

    const HAPRoute route = router_.route(frame, from);

    switch (route.action) {
      case HAPRouteAction::Deliver: {
        // Re-encoded rather than kept as the buffer it arrived in: the payload
        // of a decoded frame is a VIEW, and that buffer is gone the moment this
        // callback returns.
        ReceivedFrame record;
        record.from = from;
        record.size = frame.encode(record.bytes, sizeof(record.bytes));
        if (!delivered_.full()) {
          delivered_.push_back(record);
        }
        break;
      }

      case HAPRouteAction::ToParent:
        transmit(router_.parentMac(), frame);
        break;

      case HAPRouteAction::ToChild: {
        const HAPChild* child = router_.child(route.childIndex);
        if (child != nullptr) {
          transmit(child->mac, frame);
        }
        break;
      }

      case HAPRouteAction::Drop:
        if (!drops_.full()) {
          drops_.push_back(route.reason);
        }
        break;
    }
  }

  bool transmit(const HAPMac& to, const HAPFrame& frame) noexcept {
    uint8_t buffer[HAP_MAX_FRAME_SIZE];
    const size_t size = frame.encode(buffer, sizeof(buffer));

    return size > 0 && transport_.send(to, buffer, size);
  }

  HAPMac mac_;
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
  report.set(HAPFlags::Upstream);
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
  // network, and the gateway knows the reading came from 1.2.
  CHECK(std::strcmp(arrived.src.toString().c_str(), "1.2") == 0);
  CHECK(arrived.seq == 0x0107);

  HAPReader reader(arrived.payload, arrived.payloadSize);
  HAPReport parsed;
  CHECK(parsed.decode(reader));
  CHECK(parsed.entries.size() == 1);
  CHECK(parsed.entries[0].value.asFloat() == 21.5f);
}

void testCommandDescendsTwoHops() noexcept {
  Tree tree;

  const HAPWriteRequest request(static_cast<uint8_t>(HAPClassId::Lamp), 0, 0,
                                HValue(true));

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  request.encode(writer);

  // Addressed 1.4 from the gateway: through the controller, out to the lamp.
  const uint8_t hops[] = {kControllerIndex, kLampIndex};

  HAPFrame command;
  command.type = static_cast<uint8_t>(HAPMessage::WriteRequest);
  command.set(HAPFlags::AckRequested);
  command.seq = 0x0020;
  command.dest = HAPPath::fromBytes(hops, 2);
  command.payload = payload;
  command.payloadSize = static_cast<uint8_t>(writer.size());

  CHECK(tree.gateway.originate(command));
  CHECK(tree.bus.pumpAll() == 2);

  CHECK(tree.lamp.delivered().size() == 1);
  CHECK(tree.thermometer.delivered().empty());  // The other branch heard nothing.

  HAPFrame arrived;
  const ReceivedFrame& record = tree.lamp.delivered()[0];
  CHECK(HAPFrame::decode(record.bytes, record.size, arrived) ==
        HAPFrameError::None);
  CHECK(arrived.dest.isEmpty());
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
  reply.set(HAPFlags::Upstream);
  reply.seq = 0x0020;
  reply.payload = payload;
  reply.payloadSize = static_cast<uint8_t>(writer.size());

  CHECK(tree.lamp.originate(reply));
  CHECK(tree.bus.pumpAll() == 2);

  HAPFrame arrived;
  const ReceivedFrame& record = tree.gateway.delivered()[0];
  CHECK(HAPFrame::decode(record.bytes, record.size, arrived) ==
        HAPFrameError::None);

  // 1.4 - the same address the command was sent to, drawn by the reply itself.
  CHECK(std::strcmp(arrived.src.toString().c_str(), "1.4") == 0);
  CHECK(arrived.seq == 0x0020);
}

void testADeadNodeHidesItsSubtree() noexcept {
  Tree tree;

  // The controller loses power. There is no second way in to anything under it.
  tree.bus.setReachable(tree.gateway.mac(), tree.controller.mac(), false);
  tree.bus.setReachable(tree.controller.mac(), tree.thermometer.mac(), false);

  const uint8_t hops[] = {kControllerIndex, kThermometerIndex};

  HAPFrame command;
  command.type = static_cast<uint8_t>(HAPMessage::ReadRequest);
  command.seq = 0x0030;
  command.dest = HAPPath::fromBytes(hops, 2);

  // The gateway's radio still reports the frame as sent - a send is not a
  // delivery, which is exactly why the protocol above needs its own answer.
  CHECK(tree.gateway.originate(command));
  CHECK(tree.bus.pumpAll() > 0);

  CHECK(tree.controller.delivered().empty());
  CHECK(tree.thermometer.delivered().empty());

  // And the thermometer's own reports no longer reach anybody above it.
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  const size_t payloadSize = buildReport(payload, sizeof(payload), 21.5f);

  HAPFrame report;
  report.type = static_cast<uint8_t>(HAPMessage::Report);
  report.set(HAPFlags::Upstream);
  report.payload = payload;
  report.payloadSize = static_cast<uint8_t>(payloadSize);

  CHECK(tree.thermometer.originate(report));
  CHECK(tree.bus.pumpAll() > 0);
  CHECK(tree.gateway.delivered().empty());
}

void testAMissingChildIsDiagnosed() noexcept {
  Tree tree;

  // Child 3 of the controller does not exist. The controller can still answer,
  // so the gateway learns WHICH hop failed rather than merely that nothing
  // came back.
  const uint8_t hops[] = {kControllerIndex, 3};

  HAPFrame command;
  command.type = static_cast<uint8_t>(HAPMessage::ReadRequest);
  command.dest = HAPPath::fromBytes(hops, 2);

  CHECK(tree.gateway.originate(command));
  CHECK(tree.bus.pumpAll() > 0);

  CHECK(tree.controller.drops().size() == 1);
  CHECK(tree.controller.drops()[0] == HAPResult::NoSuchChild);
}

void testTheOtherBranchIsUndisturbed() noexcept {
  Tree tree;

  // The lamp goes away; the thermometer under the same parent must not care.
  tree.bus.setReachable(tree.controller.mac(), tree.lamp.mac(), false);

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  const size_t payloadSize = buildReport(payload, sizeof(payload), 19.0f);

  HAPFrame report;
  report.type = static_cast<uint8_t>(HAPMessage::Report);
  report.set(HAPFlags::Upstream);
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
  // nobody's child, so the frame goes no further than the router.
  intruder.transport().addPeer(tree.controller.mac(), nullptr);

  HAPFrame frame;
  frame.type = static_cast<uint8_t>(HAPMessage::ReadRequest);

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = frame.encode(buffer, sizeof(buffer));
  CHECK(intruder.transport().send(tree.controller.mac(), buffer, size));

  CHECK(tree.bus.pumpAll() > 0);
  CHECK(tree.controller.delivered().empty());
  CHECK(tree.controller.drops().size() == 1);
  CHECK(tree.controller.drops()[0] == HAPResult::NotBound);
}

void testARadioWillNotTalkToAStranger() noexcept {
  Tree tree;

  // The rule that catches more real bugs than any other: ESP-NOW refuses to
  // unicast to an address that is not in its peer table.
  HAPFrame frame;
  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = frame.encode(buffer, sizeof(buffer));

  CHECK(!tree.gateway.transport().send(tree.thermometer.mac(), buffer, size));
  CHECK(tree.gateway.transport().send(tree.controller.mac(), buffer, size));
}

void testTheWrongChannelIsDeafness() noexcept {
  Tree tree;

  // Not a dropped frame, not an error: the two simply cannot hear each other,
  // and every symptom of it looks like a protocol bug from the far end.
  tree.controller.transport().setChannel(kChannel + 5);

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  const size_t payloadSize = buildReport(payload, sizeof(payload), 21.5f);

  HAPFrame report;
  report.type = static_cast<uint8_t>(HAPMessage::Report);
  report.set(HAPFlags::Upstream);
  report.payload = payload;
  report.payloadSize = static_cast<uint8_t>(payloadSize);

  CHECK(tree.thermometer.originate(report));
  CHECK(tree.bus.pumpAll() > 0);

  CHECK(tree.controller.delivered().empty());
  CHECK(tree.gateway.delivered().empty());
}

void testEncryptionMustMatchBothWays() noexcept {
  HAPLoopbackBus bus;
  TestNode master(1, bus);
  TestNode slave(2, bus);

  uint8_t key[HAP_KEY_LEN];
  std::memset(key, 0xAB, sizeof(key));

  uint8_t wrongKey[HAP_KEY_LEN];
  std::memset(wrongKey, 0xCD, sizeof(wrongKey));

  HAPChild record;
  record.index = 1;
  record.mac = slave.mac();
  master.router().addChild(record);
  slave.router().setParent(master.mac(), 1);

  master.transport().addPeer(slave.mac(), key);
  slave.transport().addPeer(master.mac(), wrongKey);

  HAPFrame frame;
  frame.type = static_cast<uint8_t>(HAPMessage::Ping);
  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = frame.encode(buffer, sizeof(buffer));

  // The send succeeds and nothing arrives - which on real hardware is a whole
  // evening with two boards unless you already suspect the key.
  CHECK(master.transport().send(slave.mac(), buffer, size));
  CHECK(bus.pumpAll() > 0);
  CHECK(slave.delivered().empty());

  // Corrected, the same frame lands.
  slave.transport().addPeer(master.mac(), key);
  CHECK(master.transport().send(slave.mac(), buffer, size));
  CHECK(bus.pumpAll() > 0);
  CHECK(slave.delivered().size() == 1);
}

void testBroadcastReachesEveryoneInRange() noexcept {
  Tree tree;
  TestNode newcomer(9, tree.bus);

  // How an unbound node is heard at all: no peer entry needed, by anybody.
  HAPFrame announce;
  announce.type = static_cast<uint8_t>(HAPMessage::BindAnnounce);

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = announce.encode(buffer, sizeof(buffer));

  CHECK(newcomer.transport().broadcast(buffer, size));
  CHECK(tree.bus.pumpAll() > 0);

  // Every node on the channel received it; each router then refused it, because
  // a stranger's frame is the binder's business and not the router's.
  CHECK(tree.gateway.transport().receivedCount() == 1);
  CHECK(tree.controller.transport().receivedCount() == 1);
  CHECK(tree.gateway.drops().size() == 1);
  CHECK(tree.gateway.drops()[0] == HAPResult::NotBound);
}

void testDepthFive() noexcept {
  // The deepest tree the protocol allows, built as a chain, with a report from
  // the bottom of it.
  HAPLoopbackBus bus;
  TestNode nodes[HAP_MAX_DEPTH + 1] = {
      TestNode(1, bus), TestNode(2, bus), TestNode(3, bus),
      TestNode(4, bus), TestNode(5, bus), TestNode(6, bus)};

  for (size_t i = 0; i + 1 < HAP_MAX_DEPTH + 1; ++i) {
    nodes[i].adopt(1, nodes[i + 1]);
  }

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  const size_t payloadSize = buildReport(payload, sizeof(payload), 5.0f);

  HAPFrame report;
  report.type = static_cast<uint8_t>(HAPMessage::Report);
  report.set(HAPFlags::Upstream);
  report.payload = payload;
  report.payloadSize = static_cast<uint8_t>(payloadSize);

  CHECK(nodes[HAP_MAX_DEPTH].originate(report));
  CHECK(bus.pumpAll() == HAP_MAX_DEPTH);

  CHECK(nodes[0].delivered().size() == 1);

  HAPFrame arrived;
  const ReceivedFrame& record = nodes[0].delivered()[0];
  CHECK(HAPFrame::decode(record.bytes, record.size, arrived) ==
        HAPFrameError::None);

  // Five hops, and the path is exactly full - one more level and the frame
  // could not have been addressed at all.
  CHECK(arrived.src.length() == HAP_MAX_DEPTH);
  CHECK(arrived.src.isFull());
  CHECK(std::strcmp(arrived.src.toString().c_str(), "1.1.1.1.1") == 0);
}

}  // namespace

void runTreeTests() noexcept {
  HAPTest::begin("Simulated tree");

  testReportClimbsTwoHops();
  testCommandDescendsTwoHops();
  testResponseFindsItsWayBack();
  testADeadNodeHidesItsSubtree();
  testAMissingChildIsDiagnosed();
  testTheOtherBranchIsUndisturbed();
  testAStrangerIsIgnored();
  testARadioWillNotTalkToAStranger();
  testTheWrongChannelIsDeafness();
  testEncryptionMustMatchBothWays();
  testBroadcastReachesEveryoneInRange();
  testDepthFive();
}
