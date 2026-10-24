#include "HAPTest.hpp"

#include <HAPITransport/HAPLoopback/HAPLoopback.hpp>

#include <cstring>

/**
 * @file HAPLoopbackTest.cpp
 * @brief What the simulated network promises, rule by rule.
 *
 * The loopback is only worth having while it is as unhelpful as Thread: a send
 * that says `true` has delivered nothing, an address nobody holds swallows the
 * frame, and `master()` goes to one server or to nobody. Every test here pins
 * one of those, so that making the simulator friendlier later is a decision and
 * not an accident.
 */

namespace {

HAPNodeId idFor(uint8_t id) noexcept {
  const uint8_t mac[HAP_MAC_LEN] = {0x24, 0x6F, 0x28, 0x00, 0x00, id};
  return HAPNodeId::fromMac(mac);
}

/** A transport that remembers who it last heard from. */
class Probe {
 public:
  Probe(uint8_t id, HAPLoopbackBus& bus) noexcept
      : transport_(idFor(id), bus) {
    transport_.onReceive(
        HAPITransport::Receiver::create<Probe, &Probe::onFrame>(*this));
  }

  HAPLoopback& transport() noexcept { return transport_; }
  HAPAddress address() const noexcept { return transport_.localAddress(); }

  /** @brief begin() as the application would, and then the proof it worked. */
  bool up() noexcept { return transport_.begin() && transport_.isAttached(); }

  size_t heard() const noexcept { return transport_.receivedCount(); }
  const HAPAddress& lastFrom() const noexcept { return lastFrom_; }
  uint8_t lastByte() const noexcept { return lastByte_; }

 private:
  void onFrame(const HAPAddress& from, const uint8_t* data, size_t size) noexcept {
    lastFrom_ = from;
    lastByte_ = size > 0 ? data[0] : 0;
  }

  HAPLoopback transport_;
  HAPAddress lastFrom_;
  uint8_t lastByte_ = 0;
};

constexpr uint8_t kPayload[] = {0xA5, 0x01, 0x02};

bool sendTo(Probe& from, const HAPAddress& to) noexcept {
  return from.transport().send(to, kPayload, sizeof(kPayload));
}

bool multicastFrom(Probe& from) noexcept {
  return from.transport().multicast(kPayload, sizeof(kPayload));
}

// -------------------------------------------------------------------------

void testNothingWorksBeforeBegin() noexcept {
  HAPLoopbackBus bus;
  Probe a(1, bus);
  Probe b(2, bus);
  CHECK(b.up());

  // R1: a node that has not begun can neither send nor multicast.
  CHECK(!a.transport().isAttached());
  CHECK(!sendTo(a, b.address()));
  CHECK(!multicastFrom(a));
  CHECK(bus.pending() == 0);

  CHECK(a.up());
  CHECK(sendTo(a, b.address()));
  CHECK(multicastFrom(a));
  CHECK(bus.pending() == 2);
}

void testANodeThatNeverBeganHearsNothing() noexcept {
  HAPLoopbackBus bus;
  Probe a(1, bus);
  Probe sleeper(2, bus);
  CHECK(a.up());

  // Attached is started AND attached: setAttached() alone does not make a node
  // that never began receive anything.
  sleeper.transport().setAttached(true);
  CHECK(!sleeper.transport().isAttached());

  CHECK(sendTo(a, sleeper.address()));
  CHECK(multicastFrom(a));
  bus.pumpAll();

  CHECK(sleeper.heard() == 0);
}

void testADetachedNodeNeitherSendsNorReceives() noexcept {
  HAPLoopbackBus bus;
  Probe a(1, bus);
  Probe b(2, bus);
  CHECK(a.up());
  CHECK(b.up());

  // R2: a frame already queued for a node that then leaves is not received.
  CHECK(sendTo(a, b.address()));
  b.transport().setAttached(false);
  CHECK(!b.transport().isAttached());
  bus.pumpAll();
  CHECK(b.heard() == 0);

  CHECK(!sendTo(b, a.address()));
  CHECK(!multicastFrom(b));

  // And it is a state, not a loss: attached again, everything works.
  b.transport().setAttached(true);
  CHECK(b.transport().isAttached());
  CHECK(sendTo(a, b.address()));
  bus.pumpAll();
  CHECK(b.heard() == 1);
  CHECK(sendTo(b, a.address()));
}

void testBeginAttachesAgain() noexcept {
  HAPLoopbackBus bus;
  Probe a(1, bus);
  CHECK(a.up());

  // Decided, not incidental: begin() always leaves the node attached, even
  // after the test took it off the network.
  a.transport().setAttached(false);
  CHECK(!a.transport().isAttached());
  CHECK(a.up());
}

void testAUnicastToNobodyIsLostSilently() noexcept {
  HAPLoopbackBus bus;
  Probe a(1, bus);
  Probe b(2, bus);
  CHECK(a.up());
  CHECK(b.up());

  // R3: true is not delivery, and an address nobody holds is not a drop either
  // - droppedCount() counts only what dropNext() asked for.
  CHECK(sendTo(a, HAPAddress::fromNodeId(idFor(0x77))));
  CHECK(bus.pending() == 1);
  bus.pumpAll();

  CHECK(bus.pending() == 0);
  CHECK(bus.deliveredCount() == 0);
  CHECK(bus.droppedCount() == 0);
  CHECK(b.heard() == 0);
}

void testAnyTwoAttachedNodesCanTalk() noexcept {
  HAPLoopbackBus bus;
  Probe a(1, bus);
  Probe b(2, bus);
  CHECK(a.up());
  CHECK(b.up());

  // R4: no peer, no key, no channel - nothing to set up first, both ways.
  CHECK(sendTo(a, b.address()));
  bus.pumpAll();
  CHECK(b.heard() == 1);
  CHECK(b.lastFrom() == a.address());
  CHECK(b.lastByte() == kPayload[0]);

  CHECK(sendTo(b, a.address()));
  bus.pumpAll();
  CHECK(a.heard() == 1);
  CHECK(a.lastFrom() == b.address());

  CHECK(a.transport().sentCount() == 1);
  CHECK(bus.deliveredCount() == 2);
}

void testBadArgumentsAreLocalFailures() noexcept {
  HAPLoopbackBus bus;
  Probe a(1, bus);
  Probe b(2, bus);
  CHECK(a.up());
  CHECK(b.up());

  uint8_t big[HAP_MAX_FRAME_SIZE + 1] = {};

  CHECK(!a.transport().send(b.address(), nullptr, 3));
  CHECK(!a.transport().send(b.address(), kPayload, 0));
  CHECK(!a.transport().send(b.address(), big, sizeof(big)));
  CHECK(!a.transport().multicast(nullptr, 3));
  CHECK(bus.pending() == 0);
}

void testMasterIsAnycast() noexcept {
  HAPLoopbackBus bus;
  Probe a(1, bus);
  Probe b(2, bus);
  Probe c(3, bus);
  CHECK(a.up());
  CHECK(b.up());
  CHECK(c.up());

  // R5, nobody serves it: the send is fine and the frame vanishes - a stale
  // ALOC, and not a drop.
  CHECK(sendTo(a, HAPAddress::master()));
  bus.pumpAll();
  CHECK(b.heard() == 0);
  CHECK(c.heard() == 0);
  CHECK(bus.droppedCount() == 0);

  // One server: it gets the frame, and sees the SENDER's own address.
  c.transport().setServesMaster(true);
  CHECK(sendTo(a, HAPAddress::master()));
  bus.pumpAll();
  CHECK(c.heard() == 1);
  CHECK(c.lastFrom() == a.address());
  CHECK(!c.lastFrom().isMaster());
  CHECK(b.heard() == 0);
}

void testTwoServersExactlyOneHears() noexcept {
  HAPLoopbackBus bus;
  Probe a(1, bus);
  Probe low(2, bus);
  Probe high(3, bus);
  CHECK(a.up());
  CHECK(low.up());
  CHECK(high.up());

  low.transport().setServesMaster(true);
  high.transport().setServesMaster(true);

  // The lowest node id wins. That is the simulator's tie-break and not a
  // promise; what IS promised is that it is one node, not all of them.
  CHECK(sendTo(a, HAPAddress::master()));
  bus.pumpAll();
  CHECK(low.heard() == 1);
  CHECK(high.heard() == 0);
  CHECK(bus.deliveredCount() == 1);
}

void testAnycastSkipsServersThatCannotHear() noexcept {
  HAPLoopbackBus bus;
  Probe a(1, bus);
  Probe low(2, bus);
  Probe middle(3, bus);
  Probe high(4, bus);
  CHECK(a.up());
  CHECK(low.up());
  CHECK(middle.up());
  CHECK(high.up());

  low.transport().setServesMaster(true);
  middle.transport().setServesMaster(true);
  high.transport().setServesMaster(true);

  // The lowest server that is reachable and attached gets it. An unreachable
  // or detached one does not swallow the frame.
  bus.setReachable(a.transport().localId(), low.transport().localId(), false);
  CHECK(sendTo(a, HAPAddress::master()));
  bus.pumpAll();
  CHECK(low.heard() == 0);
  CHECK(middle.heard() == 1);

  middle.transport().setAttached(false);
  CHECK(sendTo(a, HAPAddress::master()));
  bus.pumpAll();
  CHECK(middle.heard() == 1);
  CHECK(high.heard() == 1);
}

void testAServerDoesNotHearItself() noexcept {
  HAPLoopbackBus bus;
  Probe server(1, bus);
  CHECK(server.up());
  server.transport().setServesMaster(true);

  // Local delivery is the stack's business: the bus never loops a frame back.
  CHECK(sendTo(server, HAPAddress::master()));
  bus.pumpAll();
  CHECK(server.heard() == 0);

  Probe other(2, bus);
  CHECK(other.up());
  CHECK(sendTo(server, HAPAddress::master()));
  bus.pumpAll();
  CHECK(server.heard() == 0);
  CHECK(other.heard() == 0);
}

void testMulticastReachesEveryAttachedNodeButTheSender() noexcept {
  HAPLoopbackBus bus;
  Probe a(1, bus);
  Probe b(2, bus);
  Probe c(3, bus);
  Probe gone(4, bus);
  CHECK(a.up());
  CHECK(b.up());
  CHECK(c.up());
  CHECK(gone.up());
  gone.transport().setAttached(false);

  // R6.
  CHECK(multicastFrom(a));
  bus.pumpAll();

  CHECK(a.heard() == 0);
  CHECK(b.heard() == 1);
  CHECK(c.heard() == 1);
  CHECK(gone.heard() == 0);
  CHECK(b.lastFrom() == a.address());
  CHECK(bus.deliveredCount() == 2);
}

void testReachabilityApplies() noexcept {
  HAPLoopbackBus bus;
  Probe a(1, bus);
  Probe b(2, bus);
  Probe c(3, bus);
  CHECK(a.up());
  CHECK(b.up());
  CHECK(c.up());
  c.transport().setServesMaster(true);

  // R7: a pair that cannot hear each other is cut for every kind of frame -
  // both ways, and without the sender finding out.
  bus.setReachable(a.transport().localId(), b.transport().localId(), false);
  bus.setReachable(a.transport().localId(), c.transport().localId(), false);
  CHECK(!bus.isReachable(b.transport().localId(), a.transport().localId()));

  CHECK(sendTo(a, b.address()));
  CHECK(sendTo(a, HAPAddress::master()));
  CHECK(multicastFrom(a));
  bus.pumpAll();
  CHECK(b.heard() == 0);
  CHECK(c.heard() == 0);

  CHECK(sendTo(b, a.address()));
  bus.pumpAll();
  CHECK(a.heard() == 0);

  // The rest of the bus is untouched, and the cut can be mended.
  CHECK(sendTo(b, c.address()));
  bus.pumpAll();
  CHECK(c.heard() == 1);

  bus.setReachable(a.transport().localId(), b.transport().localId(), true);
  CHECK(sendTo(a, b.address()));
  bus.pumpAll();
  CHECK(b.heard() == 1);
}

void testDropNextAppliesToEveryKind() noexcept {
  HAPLoopbackBus bus;
  Probe a(1, bus);
  Probe b(2, bus);
  CHECK(a.up());
  CHECK(b.up());
  b.transport().setServesMaster(true);

  // R7: the next three frames die in the air, whatever they were.
  bus.dropNext(3);
  CHECK(sendTo(a, b.address()));
  CHECK(sendTo(a, HAPAddress::master()));
  CHECK(multicastFrom(a));
  bus.pumpAll();

  CHECK(b.heard() == 0);
  CHECK(bus.droppedCount() == 3);
  CHECK(bus.deliveredCount() == 0);

  // And only those three.
  CHECK(sendTo(a, b.address()));
  bus.pumpAll();
  CHECK(b.heard() == 1);
  CHECK(bus.droppedCount() == 3);
}

void testNothingIsDeliveredInsideTheCallersFrame() noexcept {
  HAPLoopbackBus bus;
  Probe a(1, bus);
  Probe b(2, bus);
  CHECK(a.up());
  CHECK(b.up());

  CHECK(sendTo(a, b.address()));
  CHECK(b.heard() == 0);
  CHECK(bus.pending() == 1);

  CHECK(bus.pump() == 1);
  CHECK(b.heard() == 1);
}

}  // namespace

void runLoopbackTests() noexcept {
  HAPTest::begin("HAPLoopback");

  testNothingWorksBeforeBegin();
  testANodeThatNeverBeganHearsNothing();
  testADetachedNodeNeitherSendsNorReceives();
  testBeginAttachesAgain();
  testAUnicastToNobodyIsLostSilently();
  testAnyTwoAttachedNodesCanTalk();
  testBadArgumentsAreLocalFailures();
  testMasterIsAnycast();
  testTwoServersExactlyOneHears();
  testAnycastSkipsServersThatCannotHear();
  testAServerDoesNotHearItself();
  testMulticastReachesEveryAttachedNodeButTheSender();
  testReachabilityApplies();
  testDropNextAppliesToEveryKind();
  testNothingIsDeliveredInsideTheCallersFrame();
}
