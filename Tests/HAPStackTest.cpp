#include "HAPTest.hpp"

#include <HAPITransport/HAPLoopback/HAPLoopback.hpp>
#include <HAPStack/HAPStack.hpp>
#include <HConfig/HConfig.hpp>
#include <HFs/HFs.hpp>
#include <HSystemUtils/HSystemUtils.hpp>

#include <cstring>

/**
 * @file HAPStackTest.cpp
 * @brief A whole network of whole devices, in one process.
 *
 * Every earlier suite tests one mechanism. This one builds three HAPStacks on a
 * shared simulated radio and drives them the way three boards would be driven -
 * bind, report, command, sleep, link - which is the same ground Docs/TestPlan.md
 * covers on hardware.
 *
 * Where they overlap is deliberate. What passes here and fails there is a fact
 * about radios; what fails in both is a fact about the protocol, and this one
 * says so in a millisecond.
 */

namespace {

HAPMac macFor(uint8_t id) noexcept {
  HAPMac mac;
  mac.bytes[0] = 0x24;
  mac.bytes[1] = 0x6F;
  mac.bytes[2] = 0x28;
  mac.bytes[5] = id;
  return mac;
}

HAPPath pathOf(uint8_t a, uint8_t b = 0) noexcept {
  HAPPath path;
  path.append(a);

  if (b != 0) {
    path.append(b);
  }

  return path;
}

/** One device: a radio, a model of itself, and the stack that runs it. */
class Device {
 public:
  Device(uint8_t id, HAPLoopbackBus& bus, HAPDeviceType type,
         uint8_t capabilities, const char* name, uint16_t intervalSec) noexcept
      : mac_(macFor(id)), radio_(mac_, bus), stack_(radio_, node_) {
    node_.begin(type, capabilities, HAPName(name));
    node_.setReportIntervalSec(intervalSec);
  }

  HAPStack& stack() noexcept { return stack_; }
  HAPNode& node() noexcept { return node_; }
  HAPLoopback& radio() noexcept { return radio_; }
  const HAPMac& mac() const noexcept { return mac_; }

  void useModel() noexcept { stack_.setModel(model_); }
  HAPModel& model() noexcept { return model_; }

 private:
  HAPMac mac_;
  HAPLoopback radio_;
  HAPNode node_;
  HAPModel model_;
  HAPStack stack_;
};

/** Pumps the bus and ticks every device, the way three boards would run. */
void run(HAPLoopbackBus& bus, Device** devices, size_t count,
         size_t rounds) noexcept {
  for (size_t i = 0; i < rounds; ++i) {
    bus.pump();

    for (size_t d = 0; d < count; ++d) {
      devices[d]->stack().update();
    }
  }
}

/** Drives a bind to completion between two devices. */
bool bindPair(HAPLoopbackBus& bus, Device& master, Device& slave) noexcept {
  Device* devices[] = {&master, &slave};

  if (!master.stack().bindAsMaster()) {
    return false;
  }

  if (!slave.stack().bindAsSlave()) {
    return false;
  }

  run(bus, devices, 2, 4);
  HSystemUtils::sleep(HAP_ANNOUNCE_PERIOD_MS + 5);
  run(bus, devices, 2, 8);

  return slave.stack().isBound();
}

void reset() noexcept {
  HAPStore::clearAll();
}

// -------------------------------------------------------------------------

void testABoundPairReportsByItself() noexcept {
  reset();

  HAPLoopbackBus bus;
  Device master(1, bus, HAPDeviceType::Controller, HAPCaps::CanBeMaster,
                "Heating", 0);
  Device slave(2, bus, HAPDeviceType::Sensor, HAPCaps::BatteryPowered, "Bedroom",
               1);
  slave.node().addInstance(HAPClassId::Thermometer, HAPName("Temp"));

  master.useModel();
  CHECK(master.stack().begin(1));
  CHECK(slave.stack().begin(1));

  CHECK(bindPair(bus, master, slave));
  CHECK(master.stack().router().childCount() == 1);

  Device* devices[] = {&master, &slave};

  // The sensor measures, and nothing tells it to report - the reporter decides
  // and the stack acts, which is the whole of a leaf's job.
  slave.node().instanceAt(0)->publish(0, HValue(21.5f));
  run(bus, devices, 2, 4);

  const HAPRemoteNode* known = master.model().find(pathOf(1));
  REQUIRE(known != nullptr);
  CHECK(known->online);

  const HValue* reading =
      known->value(static_cast<uint8_t>(HAPClassId::Thermometer), 0, 0);
  REQUIRE(reading != nullptr);
  CHECK(reading->asFloat() == 21.5f);

  // The master heard of it at bind time, from ChildAttached, so it knows the
  // interval before the first report rather than after it.
  CHECK(known->reportIntervalSec == 1);
  CHECK(known->mac == slave.mac());

  // And it knows it does not yet know what the node IS.
  CHECK(known->needsDescribe());
}

void testTheMasterFillsInWhatItDoesNotKnow() noexcept {
  reset();

  HAPLoopbackBus bus;
  Device master(1, bus, HAPDeviceType::Controller, HAPCaps::CanBeMaster,
                "Heating", 0);
  // Mains-powered on purpose. A battery slave would have every request QUEUED
  // until its next wake, which is right and is what testASleepingChildIsQueuedFor
  // checks - it just puts a report cycle between each step of this one.
  Device slave(2, bus, HAPDeviceType::Sensor, HAPCaps::None, "Bedroom", 1);
  slave.node().addInstance(HAPClassId::Thermometer, HAPName("Temp"));
  slave.node().addInstance(HAPClassId::Hygrometer, HAPName("Hum"));

  master.useModel();
  master.stack().begin(1);
  slave.stack().begin(1);
  CHECK(bindPair(bus, master, slave));

  Device* devices[] = {&master, &slave};

  HAPPath wanted;
  CHECK(master.model().nextDescribeNeeded(wanted));
  CHECK(master.stack().requestDescribe(wanted));
  run(bus, devices, 2, 4);

  const HAPRemoteNode* known = master.model().find(pathOf(1));
  REQUIRE(known != nullptr);
  CHECK(!known->needsDescribe());
  CHECK(std::strcmp(known->name.c_str(), "Bedroom") == 0);
  CHECK(known->instances.size() == 2);
  CHECK(std::strcmp(known->instances[1].name.c_str(), "Hum") == 0);

  // A rename moves the revision, and the master notices on the next report
  // without anybody telling it anything.
  HAPSetNameRequest rename;
  rename.target = HAPSetNameRequest::Target::Instance;
  rename.classId = static_cast<uint8_t>(HAPClassId::Thermometer);
  rename.instanceId = 0;
  rename.name = HAPName("Ліжко");

  CHECK(master.stack().requestSetName(pathOf(1), rename));
  run(bus, devices, 2, 4);

  CHECK(std::strcmp(slave.node().instanceAt(0)->name().c_str(), "Ліжко") == 0);

  HSystemUtils::sleep(1100);
  run(bus, devices, 2, 4);
  const HAPRemoteNode* afterRename = master.model().find(pathOf(1));
  REQUIRE(afterRename != nullptr);
  CHECK(afterRename->needsDescribe());
}

void testACommandReachesADriver() noexcept {
  reset();

  HAPLoopbackBus bus;
  Device master(1, bus, HAPDeviceType::Controller, HAPCaps::CanBeMaster, "Hall",
                0);
  Device lamp(2, bus, HAPDeviceType::Controller, HAPCaps::None, "Bulb", 0);
  lamp.node().addInstance(HAPClassId::Lamp, HAPName("Ceiling"));

  master.useModel();
  master.stack().begin(1);
  lamp.stack().begin(1);
  CHECK(bindPair(bus, master, lamp));

  static bool switched = false;
  static bool wanted = false;
  switched = false;

  lamp.stack().onWrite(HAPStack::WriteHook::create(
      [](uint8_t classId, uint8_t, uint8_t, const HValue& value) {
        if (classId == static_cast<uint8_t>(HAPClassId::Lamp)) {
          switched = true;
          wanted = value.asBool();
        }
      }));

  Device* devices[] = {&master, &lamp};

  const HAPWriteRequest request(static_cast<uint8_t>(HAPClassId::Lamp), 0, 0,
                                HValue(true));
  CHECK(master.stack().requestWrite(pathOf(1), request));
  run(bus, devices, 2, 4);

  // The command reached something that can act on it, which is the only part of
  // this a device driver cares about.
  CHECK(switched);
  CHECK(wanted);
  CHECK(lamp.node().instanceAt(0)->input(0).asBool());
}

void testASleepingChildIsQueuedFor() noexcept {
  reset();

  HAPLoopbackBus bus;
  Device master(1, bus, HAPDeviceType::Controller, HAPCaps::CanBeMaster,
                "Heating", 0);
  Device sensor(2, bus, HAPDeviceType::Sensor, HAPCaps::BatteryPowered,
                "Bedroom", 1);
  sensor.node().addInstance(HAPClassId::Thermometer, HAPName("Temp"));

  master.useModel();
  master.stack().begin(1);
  sensor.stack().begin(1);
  CHECK(bindPair(bus, master, sensor));

  Device* devices[] = {&master, &sensor};
  run(bus, devices, 2, 4);

  // The master knows this child is battery-powered, so it does not shout into
  // an empty room.
  HAPSetNameRequest rename;
  rename.target = HAPSetNameRequest::Target::Node;
  rename.name = HAPName("Nursery");

  CHECK(master.stack().requestSetName(pathOf(1), rename));
  CHECK(master.stack().queue().size() == 1);
  CHECK(master.stack().queue().has(1));

  // Nothing was acknowledged on the child's behalf, and nothing was applied.
  CHECK(std::strcmp(sensor.node().name().c_str(), "Bedroom") == 0);

  // The child wakes and reports; the queued frame goes out in that moment and
  // nowhere else.
  HSystemUtils::sleep(1100);
  run(bus, devices, 2, 6);

  CHECK(master.stack().queue().size() == 0);
  CHECK(std::strcmp(sensor.node().name().c_str(), "Nursery") == 0);
}

void testASecondCommandForASleepingChildIsRefused() noexcept {
  reset();

  HAPLoopbackBus bus;
  Device master(1, bus, HAPDeviceType::Controller, HAPCaps::CanBeMaster,
                "Heating", 0);
  Device sensor(2, bus, HAPDeviceType::Sensor, HAPCaps::BatteryPowered,
                "Bedroom", 60);
  sensor.node().addInstance(HAPClassId::Thermometer, HAPName("Temp"));

  master.stack().begin(1);
  sensor.stack().begin(1);
  CHECK(bindPair(bus, master, sensor));

  HAPSetNameRequest rename;
  rename.target = HAPSetNameRequest::Target::Node;
  rename.name = HAPName("First");
  CHECK(master.stack().requestSetName(pathOf(1), rename));

  // One frame per sleeping child. A queue that stacked them would deliver
  // commands minutes apart in an order nobody chose.
  rename.name = HAPName("Second");
  CHECK(!master.stack().requestSetName(pathOf(1), rename));
  CHECK(master.stack().queue().size() == 1);
}

void testABindSurvivesARestart() noexcept {
  reset();

  HAPLoopbackBus bus;

  {
    Device master(1, bus, HAPDeviceType::Controller, HAPCaps::CanBeMaster,
                  "Heating", 0);
    Device sensor(2, bus, HAPDeviceType::Sensor, HAPCaps::BatteryPowered,
                  "Bedroom", 60);
    sensor.node().addInstance(HAPClassId::Thermometer, HAPName("Temp"));

    master.stack().begin(1);
    sensor.stack().begin(1);
    CHECK(bindPair(bus, master, sensor));
    CHECK(sensor.stack().isBound());
  }

  // Both devices are gone - the objects destroyed, as a power cut destroys the
  // RAM. Only what was written survives.
  Device sensor(2, bus, HAPDeviceType::Sensor, HAPCaps::BatteryPowered,
                "Bedroom", 60);
  sensor.node().addInstance(HAPClassId::Thermometer, HAPName("Temp"));

  CHECK(sensor.stack().begin(1));

  // Bound on the way up, with no handshake and nobody pressing anything.
  CHECK(sensor.stack().isBound());
  CHECK(sensor.stack().router().parentMac() == macFor(1));
  CHECK(sensor.stack().router().indexAtParent() == 1);

  // And it will not announce again: one parent, for life.
  CHECK(!sensor.stack().binder().announce());
}

void testAMasterThatRebootsCanStillHearItsChildren() noexcept {
  reset();

  // Docs/TestPlan.md A2's second half, and the half that cannot be judged by
  // looking at the tables: they came back perfect while the link was dead.
  //
  // The MASTER is the one that reboots here. The child stays up, still holding
  // its parent as an encrypted peer and still transmitting - which is exactly
  // the situation a master comes back into, and the reason it must return with
  // the same key rather than a blank one.
  HAPLoopbackBus bus;
  Device sensor(2, bus, HAPDeviceType::Sensor, HAPCaps::None, "Bedroom", 1);
  sensor.node().addInstance(HAPClassId::Thermometer, HAPName("Temp"));

  {
    Device master(1, bus, HAPDeviceType::Controller, HAPCaps::CanBeMaster,
                  "Heating", 0);

    master.stack().begin(1);
    sensor.stack().begin(1);
    CHECK(bindPair(bus, master, sensor));
  }

  // One process, one HAPStore - so the file the CHILD wrote is the file the
  // rebooted master would read, and it would come up believing it is its own
  // parent. Clearing the bind leaves `hapkids` alone, which is the half this
  // test is about; the child is unaffected because its copy is in RAM and it is
  // still running.
  HAPStore::clearBind();

  Device master(1, bus, HAPDeviceType::Controller, HAPCaps::CanBeMaster,
                "Heating", 0);

  master.useModel();
  CHECK(master.stack().begin(1));
  CHECK(master.stack().isRoot());

  CHECK(master.stack().router().childCount() == 1);
  CHECK(sensor.stack().isBound());

  // The key came back with the child. Checked separately from the reading
  // below, because "restored but unencrypted" and "restored and deaf" are the
  // same symptom and different faults.
  const HAPChild* storedChild = master.stack().router().childAt(0);
  REQUIRE(storedChild != nullptr);
  CHECK(storedChild->hasLinkKey());

  // And it knows what that child PROMISED, straight away, without waiting to be
  // told again - because nothing will tell it. A report carries no MAC, no
  // capabilities and no interval; only ChildAttached does, and that happened
  // before the reboot. An interval of zero means "promises nothing", and a node
  // that promised nothing is never called offline, so a master that got this
  // wrong could watch a child die and never say so.
  const HAPRemoteNode* restored = master.model().find(pathOf(1));
  REQUIRE(restored != nullptr);
  CHECK(restored->reportIntervalSec == 1);
  CHECK(restored->mac == sensor.mac());
  CHECK(restored->capabilities == HAPCaps::None);


  // The tables above are exactly what a rebooted master showed on the bench
  // while nothing worked. THIS is the check that matters: a reading has to make
  // it across.
  Device* devices[] = {&master, &sensor};
  sensor.node().instanceAt(0)->publish(0, HValue(21.5f));
  HSystemUtils::sleep(1100);
  run(bus, devices, 2, 6);

  const HAPRemoteNode* known = master.model().find(pathOf(1));
  REQUIRE(known != nullptr);

  const HValue* reading =
      known->value(static_cast<uint8_t>(HAPClassId::Thermometer), 0, 0);
  REQUIRE(reading != nullptr);
  CHECK(reading->asFloat() == 21.5f);
}


void testANodeIsNotCalledOfflineForObeying() noexcept {
  reset();

  // Docs/TestPlan.md A3b, and the fault it left behind on the bench: the leaf
  // was told to report every 3600 s, did exactly that, and was marked OFFLINE
  // thirty seconds later - because the master went on measuring its silence
  // against the ten seconds it had announced when it bound.
  HAPLoopbackBus bus;
  Device master(1, bus, HAPDeviceType::Controller, HAPCaps::CanBeMaster,
                "Heating", 0);
  Device sensor(2, bus, HAPDeviceType::Sensor, HAPCaps::None, "Bedroom", 1);
  sensor.node().addInstance(HAPClassId::Thermometer, HAPName("Temp"));

  master.useModel();
  master.stack().begin(1);
  sensor.stack().begin(1);
  CHECK(bindPair(bus, master, sensor));

  Device* devices[] = {&master, &sensor};
  run(bus, devices, 2, 4);

  const HAPRemoteNode* known = master.model().find(pathOf(1));
  REQUIRE(known != nullptr);
  CHECK(known->reportIntervalSec == 1);

  const HAPSetPolicyRequest request(
      static_cast<uint8_t>(HAPClassId::Thermometer), 0, 0, 3600, HValue());

  CHECK(master.stack().requestSetPolicy(pathOf(1), request));
  run(bus, devices, 2, 6);

  // The answer came back and the model believed it. This is the whole fix: an
  // interval nobody promised is not one to hold a node to.
  known = master.model().find(pathOf(1));
  REQUIRE(known != nullptr);
  CHECK(known->reportIntervalSec == 3600);

  // And three of the OLD intervals of silence no longer condemn it.
  HSystemUtils::sleep(1100 * HAP_OFFLINE_INTERVALS);
  master.model().sweepOffline();

  known = master.model().find(pathOf(1));
  REQUIRE(known != nullptr);
  CHECK(known->online);
}

void testAResetDeviceCanRejoinTheParentThatStillListsIt() noexcept {
  reset();

  // A node only announces when it is UNBOUND, so an announcement from an address
  // already in the child table is proof the child has been reset and the entry
  // is the stale one. This used to be refused: the only way back was a factory
  // reset of the PARENT, which drops its other children with it, and meanwhile
  // the ghost held one of six encrypted peer slots.
  HAPLoopbackBus bus;
  Device master(1, bus, HAPDeviceType::Controller, HAPCaps::CanBeMaster,
                "Heating", 0);
  Device first(2, bus, HAPDeviceType::Sensor, HAPCaps::None, "Hall", 1);
  Device sensor(3, bus, HAPDeviceType::Sensor, HAPCaps::None, "Bedroom", 1);

  first.node().addInstance(HAPClassId::Thermometer, HAPName("Temp"));
  sensor.node().addInstance(HAPClassId::Thermometer, HAPName("Temp"));

  master.useModel();
  master.stack().begin(1);
  first.stack().begin(1);
  sensor.stack().begin(1);

  // Two children, so that "the parent keeps its others" is a claim with
  // something behind it.
  CHECK(bindPair(bus, master, first));
  CHECK(bindPair(bus, master, sensor));
  CHECK(master.stack().router().childCount() == 2);

  const HAPChild* before = master.stack().router().childByMac(sensor.mac());
  REQUIRE(before != nullptr);
  const uint8_t wasIndex = before->index;

  // The device is reset in the field. Nothing tells the parent.
  CHECK(sensor.stack().factoryReset());
  CHECK(!sensor.stack().isBound());
  CHECK(master.stack().router().childCount() == 2);

  CHECK(bindPair(bus, master, sensor));

  // Re-adopted, not duplicated, and not at the cost of the other child.
  CHECK(master.stack().router().childCount() == 2);

  const HAPChild* after = master.stack().router().childByMac(sensor.mac());
  REQUIRE(after != nullptr);

  // THE SAME INDEX. Links, paths and anything a master has cached name a child
  // by index; coming back as a different one breaks every reference silently,
  // which is the failure B8 exists to prevent.
  CHECK(after->index == wasIndex);
  CHECK(master.stack().router().childByMac(first.mac()) != nullptr);

  // And the link works: a re-adoption that left the peer keyed with the old
  // key would look exactly like a successful bind and carry nothing.
  Device* devices[] = {&master, &first, &sensor};
  sensor.node().instanceAt(0)->publish(0, HValue(21.5f));
  HSystemUtils::sleep(1100);
  run(bus, devices, 3, 6);

  HAPPath at;
  at.append(wasIndex);

  const HAPRemoteNode* known = master.model().find(at);
  REQUIRE(known != nullptr);

  const HValue* reading =
      known->value(static_cast<uint8_t>(HAPClassId::Thermometer), 0, 0);
  REQUIRE(reading != nullptr);
  CHECK(reading->asFloat() == 21.5f);
}

void testARebootedMasterStillKnowsItsGrandchildren() noexcept {
  reset();

  // ChildAttached is the only message carrying a MAC, capabilities and an
  // interval, and it is sent once. A master that restarts and waits to be told
  // again is never told: it rebuilds its picture from reports, which carry none
  // of those, and every node below its own children becomes an entry that can
  // neither be recognised when the device moves nor ever be marked offline.
  //
  // So the master keeps what it was told. This is that, end to end.
  HAPLoopbackBus bus;
  Device middle(2, bus, HAPDeviceType::Controller, HAPCaps::CanBeMaster,
                "Heating", 0);
  Device leaf(3, bus, HAPDeviceType::Sensor, HAPCaps::None, "Bedroom", 1);

  leaf.node().addInstance(HAPClassId::Thermometer, HAPName("Temp"));

  {
    Device root(1, bus, HAPDeviceType::Gateway, HAPCaps::CanBeMaster, "Gateway",
                0);
    root.useModel();
    root.stack().begin(1);
    middle.stack().begin(1);
    leaf.stack().begin(1);

    // The BOTTOM first, for two reasons. It is the order that found this on
    // three boards; and one process shares one HAPStore, so whichever node
    // saves its children last owns `hapkids` - binding this way round leaves it
    // holding the root's, which is what a rebuilt root must read.
    CHECK(bindPair(bus, middle, leaf));
    CHECK(bindPair(bus, root, middle));

    Device* devices[] = {&root, &middle, &leaf};
    run(bus, devices, 3, 6);

    const HAPRemoteNode* seen = root.model().find(pathOf(1, 1));
    REQUIRE(seen != nullptr);
    CHECK(seen->mac == leaf.mac());
  }

  // The root lost power; the tree below it did not. One process, one HAPStore -
  // so the bind file the CHILDREN wrote is the one a rebuilt root would read.
  // See testAMasterThatRebootsCanStillHearItsChildren for the same wrinkle.
  HAPStore::clearBind();

  // Checked separately from the restore below, because "nothing was written" and
  // "what was written did not come back" are the same symptom and different
  // faults.
  HAPModel written;
  CHECK(HAPStore::loadModel(written) == 2);

  Device root(1, bus, HAPDeviceType::Gateway, HAPCaps::CanBeMaster, "Gateway", 0);
  root.useModel();
  CHECK(root.stack().begin(1));
  CHECK(root.stack().isRoot());

  // The grandchild is back, by name rather than by inference, before a single
  // frame has arrived.
  CHECK(root.model().size() == 2);

  const HAPRemoteNode* known = root.model().find(pathOf(1, 1));
  REQUIRE(known != nullptr);
  CHECK(known->mac == leaf.mac());
  CHECK(known->reportIntervalSec == 1);

  // And what was NOT stored is honestly absent: the descriptor is a cache, and
  // a master that has been away asks again rather than showing what may have
  // changed while it was gone.
  CHECK(known->needsDescribe());
  CHECK(known->instances.empty());
}
void testAFactoryResetFreesADevice() noexcept {
  reset();

  HAPLoopbackBus bus;
  Device master(1, bus, HAPDeviceType::Controller, HAPCaps::CanBeMaster,
                "Heating", 0);
  Device sensor(2, bus, HAPDeviceType::Sensor, HAPCaps::BatteryPowered,
                "Bedroom", 60);
  sensor.node().addInstance(HAPClassId::Thermometer, HAPName("Temp"));

  master.stack().begin(1);
  sensor.stack().begin(1);
  CHECK(bindPair(bus, master, sensor));

  CHECK(sensor.stack().factoryReset());
  CHECK(!sensor.stack().isBound());

  // Free to bind again, which is the only reason a factory reset exists.
  CHECK(sensor.stack().binder().announce());
  sensor.stack().binder().stopAnnouncing();

  // AND IT ACTUALLY BINDS. Being willing to announce is not the same thing, and
  // the difference is what three boards found: a reset that cleared the router
  // but left the radio's peer table alone kept the old parent as an ENCRYPTED
  // peer, so the next BindAccept - which must arrive in the clear, because it
  // carries the new key - was discarded by the radio. The node announced for
  // ever and the master logged "never confirmed", through every retry, until
  // somebody power-cycled it.
  CHECK(master.stack().factoryReset());
  CHECK(bindPair(bus, master, sensor));
  CHECK(sensor.stack().isBound());
  CHECK(master.stack().router().childCount() == 1);
}

void testAThreeNodeChain() noexcept {
  reset();

  // NODE1 <- NODE2 <- NODE3, which is Docs/TestPlan.md stage B without the
  // radios.
  HAPLoopbackBus bus;
  Device root(1, bus, HAPDeviceType::Gateway, HAPCaps::CanBeMaster, "Gateway", 0);
  Device middle(2, bus, HAPDeviceType::Controller, HAPCaps::CanBeMaster,
                "Heating", 0);
  Device leaf(3, bus, HAPDeviceType::Sensor, HAPCaps::None, "Bedroom", 1);

  middle.node().addInstance(HAPClassId::Regulator, HAPName("Loop"));
  leaf.node().addInstance(HAPClassId::Thermometer, HAPName("Temp"));

  root.useModel();
  root.stack().begin(1);
  middle.stack().begin(1);
  leaf.stack().begin(1);

  CHECK(bindPair(bus, root, middle));
  CHECK(bindPair(bus, middle, leaf));

  Device* devices[] = {&root, &middle, &leaf};
  run(bus, devices, 3, 6);

  // B1: the root heard about a node it never spoke to, at an address nobody
  // computed - the ChildAttached climbed and accumulated its own hop.
  const HAPRemoteNode* known = root.model().find(pathOf(1, 1));
  REQUIRE(known != nullptr);
  CHECK(known->mac == leaf.mac());
  CHECK(known->reportIntervalSec == 1);

  // B2: a report from the bottom, two hops, arriving with a path that built
  // itself on the way.
  leaf.node().instanceAt(0)->publish(0, HValue(21.5f));
  HSystemUtils::sleep(1100);
  run(bus, devices, 3, 8);

  const HAPRemoteNode* bottom = root.model().find(pathOf(1, 1));
  REQUIRE(bottom != nullptr);

  const HValue* reading =
      bottom->value(static_cast<uint8_t>(HAPClassId::Thermometer), 0, 0);
  REQUIRE(reading != nullptr);
  CHECK(reading->asFloat() == 21.5f);

  // B4: a diagnosis rather than a timeout. Child 3 of the middle node does not
  // exist, and the middle node is the one that can say so.
  HAPReadRequest read;
  CHECK(root.stack().requestRead(pathOf(1, 3), read));
  run(bus, devices, 3, 6);
  CHECK(root.model().find(pathOf(1, 3)) == nullptr);
}


void testAControllerAdoptedLastStillReportsItsChildren() noexcept {
  reset();

  // Docs/TestPlan.md B1 builds the tree top down. Nothing requires that, and a
  // bench that does it the other way round found this: a middle node adopted the
  // leaf BEFORE it was adopted itself, and the root never heard of the leaf at
  // all. ChildAttached fires once, at the handshake, and went nowhere because
  // there was no parent yet to send it to.
  //
  // The root then built its entry for the leaf out of reports alone - which
  // carry no MAC and no interval - so it could neither recognise that device if
  // it moved (B8) nor ever call it offline (B5). An entry that cannot be
  // identified and cannot expire.
  HAPLoopbackBus bus;
  Device root(1, bus, HAPDeviceType::Gateway, HAPCaps::CanBeMaster, "Gateway", 0);
  Device middle(2, bus, HAPDeviceType::Controller, HAPCaps::CanBeMaster,
                "Heating", 0);
  Device leaf(3, bus, HAPDeviceType::Sensor, HAPCaps::None, "Bedroom", 1);

  leaf.node().addInstance(HAPClassId::Thermometer, HAPName("Temp"));

  root.useModel();
  root.stack().begin(1);
  middle.stack().begin(1);
  leaf.stack().begin(1);

  // THE BOTTOM FIRST, which is the whole point of this test.
  CHECK(bindPair(bus, middle, leaf));
  CHECK(!middle.stack().isBound());
  CHECK(middle.stack().router().childCount() == 1);

  CHECK(bindPair(bus, root, middle));

  Device* devices[] = {&root, &middle, &leaf};
  run(bus, devices, 3, 8);

  // The root knows the grandchild by NAME rather than by inference: its address,
  // its permanent identity, and what it promised.
  const HAPRemoteNode* known = root.model().find(pathOf(1, 1));
  REQUIRE(known != nullptr);
  CHECK(known->mac == leaf.mac());
  CHECK(known->reportIntervalSec == 1);
  CHECK(known->capabilities == HAPCaps::None);
}
void testALinkAtTheCommonAncestor() noexcept {
  reset();

  // Docs/TestPlan.md B6, and Docs/Links.md section 8: the root installs a link
  // that lives on the middle node, then stops being involved.
  HAPLoopbackBus bus;
  Device root(1, bus, HAPDeviceType::Gateway, HAPCaps::CanBeMaster, "Gateway", 0);
  Device middle(2, bus, HAPDeviceType::Controller, HAPCaps::CanBeMaster,
                "Heating", 0);
  Device leaf(3, bus, HAPDeviceType::Sensor, HAPCaps::None, "Bedroom", 1);

  middle.node().addInstance(HAPClassId::Regulator, HAPName("Loop"));
  leaf.node().addInstance(HAPClassId::Thermometer, HAPName("Temp"));

  root.useModel();
  root.stack().begin(1);
  middle.stack().begin(1);
  leaf.stack().begin(1);

  CHECK(bindPair(bus, root, middle));
  CHECK(bindPair(bus, middle, leaf));

  Device* devices[] = {&root, &middle, &leaf};

  HAPLinkSpec spec;
  spec.linkId = 0;
  spec.source.path = pathOf(1);  // The middle node's own child.
  spec.source.classId = static_cast<uint8_t>(HAPClassId::Thermometer);
  spec.destination.classId = static_cast<uint8_t>(HAPClassId::Regulator);

  CHECK(root.stack().requestSetLink(pathOf(1), spec));
  run(bus, devices, 3, 6);

  CHECK(middle.stack().links().size() == 1);

  leaf.node().instanceAt(0)->publish(0, HValue(21.5f));
  HSystemUtils::sleep(1100);
  run(bus, devices, 3, 8);

  // A tap, not a diversion: the regulator was fed AND the root saw the reading.
  CHECK(middle.node().instanceAt(0)->input(0).asFloat() == 21.5f);

  const HAPRemoteNode* seenAtRoot = root.model().find(pathOf(1, 1));
  REQUIRE(seenAtRoot != nullptr);

  const HValue* seen =
      seenAtRoot->value(static_cast<uint8_t>(HAPClassId::Thermometer), 0, 0);
  REQUIRE(seen != nullptr);
  CHECK(seen->asFloat() == 21.5f);

  // The root goes away, and the loop carries on: the link lives on the middle
  // node and its source is one hop below it.
  bus.setReachable(root.mac(), middle.mac(), false);

  leaf.node().instanceAt(0)->publish(0, HValue(23.0f));
  HSystemUtils::sleep(1100);
  run(bus, devices, 3, 8);

  CHECK(middle.node().instanceAt(0)->input(0).asFloat() == 23.0f);
  CHECK(!middle.node().instanceAt(0)->isInputStale(0, 5000));
}

void testASleepingNodeMaySleep() noexcept {
  reset();

  HAPLoopbackBus bus;
  Device master(1, bus, HAPDeviceType::Controller, HAPCaps::CanBeMaster,
                "Heating", 0);
  Device sensor(2, bus, HAPDeviceType::Sensor, HAPCaps::BatteryPowered,
                "Bedroom", 1);
  sensor.node().addInstance(HAPClassId::Thermometer, HAPName("Temp"));

  master.stack().begin(1);
  sensor.stack().begin(1);

  // Mid-handshake is not a moment to sleep through.
  CHECK(sensor.stack().binder().announce());
  CHECK(!sensor.stack().maySleep());

  CHECK(master.stack().bindAsMaster());
  Device* devices[] = {&master, &sensor};
  HSystemUtils::sleep(HAP_ANNOUNCE_PERIOD_MS + 5);
  run(bus, devices, 2, 8);
  CHECK(sensor.stack().isBound());

  // Just reported, so its window is open and its parent may still answer.
  sensor.node().instanceAt(0)->publish(0, HValue(21.5f));
  CHECK(sensor.stack().sendReport());
  CHECK(!sensor.stack().maySleep());

  HSystemUtils::sleep(HAP_LISTEN_WINDOW_MS + 20);
  CHECK(sensor.stack().maySleep());
}

}  // namespace

void runStackTests() noexcept {
  HAPTest::begin("HAPStack");

  HFs::HFileSystem::mount();
  HConfig::init();

  testABoundPairReportsByItself();
  testTheMasterFillsInWhatItDoesNotKnow();
  testACommandReachesADriver();
  testASleepingChildIsQueuedFor();
  testASecondCommandForASleepingChildIsRefused();
  testABindSurvivesARestart();
  testAMasterThatRebootsCanStillHearItsChildren();
  testANodeIsNotCalledOfflineForObeying();
  testAFactoryResetFreesADevice();
  testAResetDeviceCanRejoinTheParentThatStillListsIt();
  testARebootedMasterStillKnowsItsGrandchildren();
  testAThreeNodeChain();
  testAControllerAdoptedLastStillReportsItsChildren();
  testALinkAtTheCommonAncestor();
  testASleepingNodeMaySleep();

  reset();
}
