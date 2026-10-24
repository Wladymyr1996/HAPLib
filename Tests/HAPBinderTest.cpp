#include "HAPTest.hpp"

#include <HAPBinder/HAPBinder.hpp>
#include <HAPITransport/HAPLoopback/HAPLoopback.hpp>
#include <HAPRandom/HAPRandom.hpp>
#include <HSystemUtils/HSystemUtils.hpp>

#include <cstring>

/**
 * @file HAPBinderTest.cpp
 * @brief The handshake, on the simulated radio.
 *
 * Two nodes that have never met, a broadcast, and four frames later a tree.
 *
 * Timers are real HTimers against the real clock, so anything that has to wait
 * is waited for. The tests keep those waits to the announce period and the
 * confirmation timeout and never to a bind window (60 s), which is checked by
 * driving state directly instead.
 */

namespace {

HAPNodeId idFor(uint8_t id) noexcept {
  const uint8_t mac[HAP_MAC_LEN] = {0x24, 0x6F, 0x28, 0x00, 0x00, id};
  return HAPNodeId::fromMac(mac);
}

/** A node with just enough around it to bind: radio, router, model, binder. */
class BindableNode {
 public:
  BindableNode(uint8_t id, HAPLoopbackBus& bus, HAPDeviceType type,
               uint8_t capabilities, const char* name) noexcept
      : id_(idFor(id)),
        transport_(id_, bus),
        binder_(transport_, router_, node_) {
    node_.begin(type, capabilities, HAPName(name));
    node_.setReportIntervalSec(60);
    transport_.begin();
    transport_.onReceive(HAPITransport::Receiver::create<BindableNode,
                                                        &BindableNode::onFrame>(
        *this));

    binder_.onChildBound(
        HAPBinder::ChildBoundHook::create<BindableNode,
                                          &BindableNode::childBound>(*this));
    binder_.onBound(
        HAPBinder::BoundHook::create<BindableNode, &BindableNode::bound>(*this));
  }

  HAPNode& node() noexcept { return node_; }
  HAPRouter& router() noexcept { return router_; }
  HAPBinder& binder() noexcept { return binder_; }
  HAPLoopback& transport() noexcept { return transport_; }
  const HAPNodeId& id() const noexcept { return id_; }
  HAPAddress address() const noexcept { return HAPAddress::fromNodeId(id_); }

  /** Frames that reached this node but belonged to no handshake. */
  size_t unhandled() const noexcept { return unhandled_; }

  bool sawChildBound() const noexcept { return childBound_; }
  bool sawBound() const noexcept { return bound_; }
  const HAPChild& lastChild() const noexcept { return lastChild_; }

  /** The key this node ended up holding, whichever side of the bind it was on. */
  const uint8_t* key() const noexcept { return key_; }

  void update() noexcept { binder_.update(); }

 private:
  void onFrame(const HAPAddress& from, const uint8_t* data, size_t size) noexcept {
    HAPFrame frame;
    if (HAPFrame::decode(data, size, frame) != HAPFrameError::None) {
      return;
    }

    // The binder gets first refusal: bind traffic comes from an address that is
    // neither parent nor child, which is precisely what the router rejects.
    if (binder_.handle(from, frame)) {
      return;
    }

    ++unhandled_;
  }

  void childBound(const HAPChild& child) noexcept {
    childBound_ = true;
    lastChild_ = child;
    std::memcpy(key_, binder_.linkKey(), HAP_KEY_LEN);
  }

  void bound(const HAPAddress& parent, uint8_t index, const uint8_t* linkKey,
             uint8_t channel) noexcept {
    (void)parent;
    (void)index;
    (void)channel;
    bound_ = true;
    std::memcpy(key_, linkKey, HAP_KEY_LEN);
  }

  HAPNodeId id_;
  HAPLoopback transport_;
  HAPRouter router_;
  HAPNode node_;
  HAPBinder binder_;

  size_t unhandled_ = 0;
  bool childBound_ = false;
  bool bound_ = false;
  HAPChild lastChild_;
  uint8_t key_[HAP_KEY_LEN] = {};
};

/** Pumps the bus and ticks both binders, the way a pair of devices would run. */
void run(HAPLoopbackBus& bus, BindableNode& a, BindableNode& b,
         size_t rounds) noexcept {
  for (size_t i = 0; i < rounds; ++i) {
    bus.pump();
    a.update();
    b.update();
  }
}

BindableNode makeMaster(uint8_t id, HAPLoopbackBus& bus) noexcept {
  return BindableNode(id, bus, HAPDeviceType::Controller, HAPCaps::CanBeMaster,
                      "Heating 01");
}

/** A leaf that looks like this project's thermometer. */
void fillSensor(HAPNode& node) noexcept {
  node.addInstance(HAPClassId::Thermometer, HAPName("Temp"));
  node.addInstance(HAPClassId::Hygrometer, HAPName("Hum"));
  node.addInstance(HAPClassId::BatteryState, HAPName("Bat"));
}

// -------------------------------------------------------------------------

void testTheWholeHandshake() noexcept {
  HAPLoopbackBus bus;
  BindableNode master = makeMaster(1, bus);
  BindableNode slave(2, bus, HAPDeviceType::Sensor, HAPCaps::BatteryPowered,
                     "Bedroom");
  fillSensor(slave.node());

  // Nothing happens without both presses: a slave announcing into a network
  // whose master is not listening binds to nothing.
  CHECK(slave.binder().announce());
  run(bus, master, slave, 4);
  CHECK(!master.sawChildBound());
  CHECK(!slave.sawBound());
  CHECK(master.router().childCount() == 0);

  // Now the master's window opens. The announcement already sent is gone, so
  // what binds these two is the NEXT one - which is the whole point of
  // announcing repeatedly rather than once.
  CHECK(master.binder().listen());
  CHECK(master.binder().isListening());

  HSystemUtils::sleep(HAP_ANNOUNCE_PERIOD_MS + 5);
  run(bus, master, slave, 2);

  // The announcement has been answered and the acceptance is on its way. The
  // master is holding a slot open and has NOT recorded a child: nothing is
  // bound until the other end confirms.
  CHECK(master.binder().state() == HAPBinder::State::Accepting);
  CHECK(master.router().childCount() == 0);

  run(bus, master, slave, 6);

  CHECK(master.sawChildBound());
  CHECK(slave.sawBound());

  CHECK(master.router().childCount() == 1);
  CHECK(master.lastChild().index == 1);
  CHECK(master.lastChild().id == slave.id());
  CHECK(master.lastChild().isBatteryPowered());
  CHECK(master.lastChild().deviceType == HAPDeviceType::Sensor);

  // The master learned the revision from the confirmation, so it already knows
  // whether what it caches is current.
  CHECK(master.lastChild().descriptorRev == slave.node().descriptorRev());

  CHECK(slave.router().hasParent());
  CHECK(slave.router().parentId() == master.id());
  CHECK(slave.router().indexAtParent() == 1);
  CHECK(slave.binder().state() == HAPBinder::State::Bound);

  // Both ends hold the same key - which is what makes the link work, and the
  // only thing about the key worth asserting.
  CHECK(std::memcmp(master.key(), slave.key(), HAP_KEY_LEN) == 0);

  // The window closed on success rather than running its full minute.
  CHECK(!master.binder().isListening());
}

void testTheMasterCanReachTheSlaveAfterwards() noexcept {
  HAPLoopbackBus bus;
  BindableNode master = makeMaster(1, bus);
  BindableNode slave(2, bus, HAPDeviceType::Sensor, HAPCaps::BatteryPowered,
                     "Bedroom");
  fillSensor(slave.node());

  CHECK(master.binder().listen());
  CHECK(slave.binder().announce());
  run(bus, master, slave, 4);
  run(bus, master, slave, 8);

  CHECK(master.sawChildBound());

  // A plain unicast, with no setup beyond the bind itself.
  const size_t before = slave.transport().receivedCount();

  HAPFrame ping;
  ping.type = static_cast<uint8_t>(HAPMessage::Ping);
  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = ping.encode(buffer, sizeof(buffer));

  CHECK(master.transport().send(slave.address(), buffer, size));
  bus.pumpAll();
  CHECK(slave.transport().receivedCount() == before + 1);
}

void testASlaveBindsOnlyOnce() noexcept {
  HAPLoopbackBus bus;
  BindableNode master = makeMaster(1, bus);
  BindableNode slave(2, bus, HAPDeviceType::Sensor, HAPCaps::BatteryPowered,
                     "Bedroom");
  fillSensor(slave.node());

  CHECK(master.binder().listen());
  CHECK(slave.binder().announce());
  run(bus, master, slave, 4);
  run(bus, master, slave, 8);
  CHECK(slave.sawBound());

  // One parent, for life. An accidental rebind would silently detach a whole
  // subtree, so a bound node refuses its own Bind button.
  CHECK(!slave.binder().announce());

  // Until a factory reset frees it, which is the only way out.
  slave.router().clearParent();
  CHECK(slave.binder().announce());
}

void testAFullMasterRefuses() noexcept {
  HAPLoopbackBus bus;
  BindableNode master = makeMaster(1, bus);

  for (uint8_t i = 1; i <= HAP_MAX_CHILDREN; ++i) {
    HAPChild child;
    child.index = i;
    child.id = idFor(static_cast<uint8_t>(0x40 + i));
    CHECK(master.router().addChild(child));
  }

  // The router's child table is full, and a master that let a seventh bind
  // would have nowhere to record it.
  CHECK(master.router().isFull());
  CHECK(!master.binder().listen());
  CHECK(master.binder().state() == HAPBinder::State::Idle);
}

void testAnnouncementsAreIgnoredWithNoWindowOpen() noexcept {
  HAPLoopbackBus bus;
  BindableNode master = makeMaster(1, bus);
  BindableNode slave(2, bus, HAPDeviceType::Sensor, HAPCaps::BatteryPowered,
                     "Bedroom");
  fillSensor(slave.node());

  CHECK(slave.binder().announce());
  run(bus, master, slave, 6);

  // It heard every one of them and answered none: a bind needs a press at both
  // ends, and the master's has not happened.
  CHECK(master.transport().receivedCount() > 0);
  CHECK(master.router().childCount() == 0);
  CHECK(!slave.sawBound());
  CHECK(slave.binder().isAnnouncing());
}

void testAnAbandonedAcceptGivesTheIndexBack() noexcept {
  HAPLoopbackBus bus;
  BindableNode master = makeMaster(1, bus);
  BindableNode slave(2, bus, HAPDeviceType::Sensor, HAPCaps::BatteryPowered,
                     "Bedroom");
  fillSensor(slave.node());

  CHECK(master.binder().listen());
  CHECK(slave.binder().announce());

  // The acceptance is lost on the way - the slave never hears it, so it never
  // confirms.
  bus.pump();          // the announcement reaches the master
  master.update();     // which answers
  bus.dropNext(1);     // and the answer evaporates
  bus.pumpAll();

  CHECK(master.binder().state() == HAPBinder::State::Accepting);
  CHECK(!slave.sawBound());

  // The master waits, gives up, and takes back everything it handed out: no
  // half-bound child.
  HSystemUtils::sleep(HAP_CONFIRM_TIMEOUT_MS + 50);
  master.update();

  CHECK(master.router().childCount() == 0);

  // And the window is still open for whatever is left of its minute.
  CHECK(master.binder().state() == HAPBinder::State::Listening);
}

void testTheAcceptGoesOnlyToWhoeverAnnounced() noexcept {
  HAPLoopbackBus bus;
  BindableNode master = makeMaster(1, bus);
  BindableNode slave(2, bus, HAPDeviceType::Sensor, HAPCaps::BatteryPowered,
                     "Bedroom");
  BindableNode bystander(3, bus, HAPDeviceType::Sensor, HAPCaps::BatteryPowered,
                         "Hall");
  fillSensor(slave.node());
  fillSensor(bystander.node());

  CHECK(master.binder().listen());
  CHECK(slave.binder().announce());

  for (size_t i = 0; i < 4; ++i) {
    bus.pump();
    master.update();
    slave.update();
    bystander.update();
  }


  for (size_t i = 0; i < 8; ++i) {
    bus.pump();
    master.update();
    slave.update();
    bystander.update();
  }

  // The acceptance was unicast to the address that announced, so a device in
  // range that kept quiet is not adopted by accident.
  CHECK(slave.sawBound());
  CHECK(!bystander.sawBound());
  CHECK(master.router().childCount() == 1);
  CHECK(master.router().childById(bystander.id()) == nullptr);
}

void testIndicesAreHandedOutInOrder() noexcept {
  HAPLoopbackBus bus;
  BindableNode master = makeMaster(1, bus);

  for (uint8_t i = 0; i < 3; ++i) {
    BindableNode slave(static_cast<uint8_t>(10 + i), bus, HAPDeviceType::Sensor,
                       HAPCaps::BatteryPowered, "Sensor");
    fillSensor(slave.node());

    CHECK(master.binder().listen());
    CHECK(slave.binder().announce());
    run(bus, master, slave, 4);
    run(bus, master, slave, 8);

    CHECK(slave.sawBound());
    CHECK(master.lastChild().index == i + 1);
  }

  CHECK(master.router().childCount() == 3);
}

void testRandomKeysDiffer() noexcept {
  HAPRandom::seed(1);

  uint8_t first[HAP_KEY_LEN] = {};
  uint8_t second[HAP_KEY_LEN] = {};
  HAPRandom::fill(first, sizeof(first));
  HAPRandom::fill(second, sizeof(second));

  CHECK(std::memcmp(first, second, HAP_KEY_LEN) != 0);

  // All-zero would be a generator that never ran, which is the failure worth
  // catching: it would look like encryption and be none.
  uint8_t zero[HAP_KEY_LEN] = {};
  CHECK(std::memcmp(first, zero, HAP_KEY_LEN) != 0);

  // Repeatable on the host, deliberately - see HAPRandom's own documentation.
  HAPRandom::seed(1);
  uint8_t again[HAP_KEY_LEN] = {};
  HAPRandom::fill(again, sizeof(again));
  CHECK(std::memcmp(first, again, HAP_KEY_LEN) == 0);
}

}  // namespace

void runBinderTests() noexcept {
  HAPTest::begin("HAPBinder");

  testRandomKeysDiffer();
  testTheWholeHandshake();
  testTheMasterCanReachTheSlaveAfterwards();
  testASlaveBindsOnlyOnce();
  testAFullMasterRefuses();
  testAnnouncementsAreIgnoredWithNoWindowOpen();
  testAnAbandonedAcceptGivesTheIndexBack();
  testTheAcceptGoesOnlyToWhoeverAnnounced();
  testIndicesAreHandedOutInOrder();
}
