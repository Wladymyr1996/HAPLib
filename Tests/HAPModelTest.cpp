#include "HAPTest.hpp"

#include <HAPModel/HAPModel.hpp>
#include <HSystemUtils/HSystemUtils.hpp>

#include <cstring>

/**
 * @file HAPModelTest.cpp
 * @brief What the root learns without being told.
 *
 * Nothing announces the shape of a HAP network. These check that a model built
 * only by listening ends up correct - and, just as importantly, that it knows
 * when what it holds has stopped being true.
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

HAPReport reportOf(uint16_t rev, float celsius) noexcept {
  HAPReport report;
  report.descriptorRev = rev;
  report.entries.push_back(HAPValueEntry(
      static_cast<uint8_t>(HAPClassId::Thermometer), 0, 0, HValue(celsius)));
  return report;
}

HAPDescribeResponse describeOf(uint16_t rev, const char* name) noexcept {
  HAPDescribeResponse response;
  response.descriptorRev = rev;
  response.instanceCount = 1;
  response.pageIndex = 0;
  response.pageCount = 1;
  response.nodeName = name;

  HAPInstanceDescriptor instance;
  instance.classId = static_cast<uint8_t>(HAPClassId::Thermometer);
  instance.instanceId = 0;
  instance.valueType = static_cast<uint8_t>(HAPValueType::Float);
  instance.name = "Temp";
  response.instances.push_back(instance);

  return response;
}

// -------------------------------------------------------------------------

void testAReportIsDiscovery() noexcept {
  HAPModel model;
  CHECK(model.size() == 0);

  // Nobody told the root this node exists. Its report arriving with src = 1.2
  // is the whole of the announcement.
  const HAPRemoteNode* node = model.noteReport(pathOf(1, 2), reportOf(0x9C4A, 21.5f));

  CHECK(node != nullptr);
  CHECK(model.size() == 1);
  CHECK(std::strcmp(node->path.toString().c_str(), "1.2") == 0);
  CHECK(node->online);
  CHECK(node->descriptorRev == 0x9C4A);

  const HValue* temperature =
      node->value(static_cast<uint8_t>(HAPClassId::Thermometer), 0, 0);
  CHECK(temperature != nullptr);
  CHECK(temperature->asFloat() == 21.5f);

  // A second report from the same node is the same node, not another one.
  model.noteReport(pathOf(1, 2), reportOf(0x9C4A, 21.6f));
  CHECK(model.size() == 1);
  CHECK(model.find(pathOf(1, 2))
            ->value(static_cast<uint8_t>(HAPClassId::Thermometer), 0, 0)
            ->asFloat() == 21.6f);
}

void testTheRevisionDrivesEverything() noexcept {
  HAPModel model;

  model.noteReport(pathOf(1), reportOf(0x9C4A, 21.5f));

  // Heard of, but not known: the model has a revision it has never seen a
  // descriptor for.
  CHECK(model.find(pathOf(1))->needsDescribe());
  CHECK(model.describeBacklog() == 1);

  HAPPath wanted;
  CHECK(model.nextDescribeNeeded(wanted));
  CHECK(wanted == pathOf(1));

  model.noteDescribe(pathOf(1), describeOf(0x9C4A, "Bedroom"));

  CHECK(!model.find(pathOf(1))->needsDescribe());
  CHECK(model.describeBacklog() == 0);
  CHECK(!model.nextDescribeNeeded(wanted));
  CHECK(std::strcmp(model.find(pathOf(1))->name.c_str(), "Bedroom") == 0);
  CHECK(model.find(pathOf(1))->instances.size() == 1);

  // Ordinary reports do not disturb it, or a master would re-read what a node
  // IS every minute.
  model.noteReport(pathOf(1), reportOf(0x9C4A, 22.0f));
  CHECK(!model.find(pathOf(1))->needsDescribe());

  // A rename moved the revision, and the model notices without being told.
  model.noteReport(pathOf(1), reportOf(0x17E2, 22.0f));
  CHECK(model.find(pathOf(1))->needsDescribe());

  // The cached descriptor is still there and still readable - it is stale, not
  // absent, which is what lets a user interface keep showing something.
  CHECK(std::strcmp(model.find(pathOf(1))->name.c_str(), "Bedroom") == 0);

  model.noteDescribe(pathOf(1), describeOf(0x17E2, "Nursery"));
  CHECK(!model.find(pathOf(1))->needsDescribe());
  CHECK(std::strcmp(model.find(pathOf(1))->name.c_str(), "Nursery") == 0);
}

void testAPongCarriesTheRevisionToo() noexcept {
  HAPModel model;
  model.noteDescribe(pathOf(1), describeOf(0x1111, "Hall"));
  CHECK(!model.find(pathOf(1))->needsDescribe());

  // A mains node that reports nothing still answers a ping, and that is enough
  // to notice it has changed.
  HAPPong pong;
  pong.descriptorRev = 0x2222;
  model.notePong(pathOf(1), pong);

  CHECK(model.find(pathOf(1))->needsDescribe());
}

void testAHalfArrivedDescriptorIsNotADescriptor() noexcept {
  HAPModel model;

  HAPDescribeResponse first = describeOf(0x9C4A, "Boiler");
  first.pageIndex = 0;
  first.pageCount = 2;
  first.instanceCount = 2;
  model.noteDescribe(pathOf(1), first);

  // Still incomplete, so the model keeps asking. Marking it current here would
  // leave half a descriptor cached forever.
  CHECK(model.find(pathOf(1))->needsDescribe());
  CHECK(model.find(pathOf(1))->instances.size() == 1);

  HAPDescribeResponse second = describeOf(0x9C4A, "Boiler");
  second.pageIndex = 1;
  second.pageCount = 2;
  second.instanceCount = 2;
  second.instances[0].instanceId = 1;
  second.instances[0].name = "Return";
  model.noteDescribe(pathOf(1), second);

  CHECK(!model.find(pathOf(1))->needsDescribe());

  // Both pages, in order, and not a duplicate of the first.
  CHECK(model.find(pathOf(1))->instances.size() == 2);
  CHECK(std::strcmp(model.find(pathOf(1))->instances[1].name.c_str(), "Return") == 0);

  // A new page 0 replaces rather than appends, or a re-read would double
  // everything.
  model.noteDescribe(pathOf(1), describeOf(0x9C4A, "Boiler"));
  CHECK(model.find(pathOf(1))->instances.size() == 1);
}

void testChildAttachedNamesTheNodeBeforeItSpeaks() noexcept {
  HAPModel model;

  // The controller's notice climbed one hop, so the root sees it with
  // src = [1] and the new node is at 1 + 2.
  HAPChildAttached attached;
  attached.childIndex = 2;
  attached.deviceType = HAPDeviceType::Sensor;
  attached.capabilities = HAPCaps::BatteryPowered;
  attached.reportIntervalSec = 60;
  attached.descriptorRev = 0x9C4A;
  std::memcpy(attached.mac, macFor(0x33).bytes, HAP_MAC_LEN);

  const HAPRemoteNode* node = model.noteChildAttached(pathOf(1), attached);

  CHECK(node != nullptr);
  CHECK(std::strcmp(node->path.toString().c_str(), "1.2") == 0);
  CHECK(node->mac == macFor(0x33));
  CHECK(node->isBatteryPowered());

  // Which is the point: a battery sensor is known at the moment it binds rather
  // than a minute later when it first reports.
  CHECK(node->reportIntervalSec == 60);
  CHECK(node->needsDescribe());

  CHECK(model.findByMac(macFor(0x33)) != nullptr);
  CHECK(model.findByMac(macFor(0x99)) == nullptr);
}

void testADeviceThatMovedIsOneDeviceStill() noexcept {
  HAPModel model;

  HAPChildAttached attached;
  attached.childIndex = 2;
  attached.reportIntervalSec = 60;
  attached.descriptorRev = 0x9C4A;
  std::memcpy(attached.mac, macFor(0x33).bytes, HAP_MAC_LEN);

  model.noteChildAttached(pathOf(1), attached);
  CHECK(model.size() == 1);

  // The same MAC turns up under a different parent - somebody reset it and
  // bound it elsewhere.
  attached.childIndex = 1;
  model.noteChildAttached(pathOf(3), attached);

  // One device, at its new address. Keeping both would show one thing twice,
  // and anything pointing at the old path would fail quietly forever.
  CHECK(model.size() == 1);
  CHECK(model.find(pathOf(1, 2)) == nullptr);
  CHECK(model.find(pathOf(3, 1)) != nullptr);
  CHECK(model.findByMac(macFor(0x33))->path == pathOf(3, 1));
}

void testSilenceIsMeasuredAgainstAPromise() noexcept {
  HAPModel model;

  HAPChildAttached attached;
  attached.childIndex = 1;
  attached.reportIntervalSec = 1;  // A second, so the test can wait for it.
  attached.descriptorRev = 0x9C4A;
  std::memcpy(attached.mac, macFor(0x44).bytes, HAP_MAC_LEN);
  model.noteChildAttached(HAPPath(), attached);

  CHECK(model.find(pathOf(1))->online);
  CHECK(model.sweepOffline() == 0);

  HSystemUtils::sleep(1000 * HAP_OFFLINE_INTERVALS + 100);

  // Three missed reports. The sweep reports how many CHANGED, so a caller acts
  // once rather than on every pass.
  CHECK(model.sweepOffline() == 1);
  CHECK(!model.find(pathOf(1))->online);
  CHECK(model.sweepOffline() == 0);

  // And it comes back on its own, with no rebind and nothing to reset.
  model.noteReport(pathOf(1), reportOf(0x9C4A, 21.5f));
  CHECK(model.find(pathOf(1))->online);
}

void testANodeThatPromisedNothingIsNeverCalledDead() noexcept {
  HAPModel model;

  // reportIntervalSec 0: it speaks only when something changes, so its silence
  // says nothing at all. Calling it offline would be inventing a fault.
  HAPChildAttached attached;
  attached.childIndex = 1;
  attached.reportIntervalSec = 0;
  std::memcpy(attached.mac, macFor(0x55).bytes, HAP_MAC_LEN);
  model.noteChildAttached(HAPPath(), attached);

  HSystemUtils::sleep(100);

  CHECK(model.sweepOffline() == 0);
  CHECK(model.find(pathOf(1))->online);
}

void testHooksFireOnceEach() noexcept {
  HAPModel model;

  static size_t discovered = 0;
  static size_t presence = 0;
  discovered = 0;
  presence = 0;

  model.onDiscovered(HAPModel::DiscoveredHook::create(
      [](const HAPRemoteNode&) { ++discovered; }));
  model.onPresenceChanged(
      HAPModel::PresenceHook::create([](const HAPRemoteNode&) { ++presence; }));

  model.noteReport(pathOf(1), reportOf(0x9C4A, 21.5f));
  CHECK(discovered == 1);

  // Already known: a hook that fired on every report would be a stream, not an
  // event.
  model.noteReport(pathOf(1), reportOf(0x9C4A, 21.6f));
  CHECK(discovered == 1);
  CHECK(presence == 0);

  model.noteReport(pathOf(2), reportOf(0x1234, 19.0f));
  CHECK(discovered == 2);
}

void testTheModelHasABottom() noexcept {
  HAPModel model;

  for (uint8_t i = 1; i <= HAP_MODEL_MAX_NODES; ++i) {
    CHECK(model.noteReport(pathOf(i), reportOf(0x1000 + i, 20.0f)) != nullptr);
  }

  CHECK(model.isFull());

  // A model that quietly forgot the oldest node to fit a new one would show a
  // network that keeps losing devices at random.
  CHECK(model.noteReport(pathOf(HAP_MODEL_MAX_NODES + 1),
                         reportOf(0x2000, 20.0f)) == nullptr);
  CHECK(model.size() == HAP_MODEL_MAX_NODES);

  CHECK(model.forget(pathOf(1)));
  CHECK(!model.forget(pathOf(1)));
  CHECK(model.noteReport(pathOf(HAP_MODEL_MAX_NODES + 1),
                         reportOf(0x2000, 20.0f)) != nullptr);

  model.clear();
  CHECK(model.size() == 0);
}

void testAValueThatIsNotThere() noexcept {
  HAPModel model;
  model.noteReport(pathOf(1), reportOf(0x9C4A, 21.5f));

  const HAPRemoteNode* node = model.find(pathOf(1));
  CHECK(node->value(static_cast<uint8_t>(HAPClassId::Hygrometer), 0, 0) == nullptr);
  CHECK(node->value(static_cast<uint8_t>(HAPClassId::Thermometer), 1, 0) == nullptr);
  CHECK(node->value(static_cast<uint8_t>(HAPClassId::Thermometer), 0, 1) == nullptr);

  // A sensor that stopped answering reports Null, and the model keeps it as
  // Null - "no reading" is information, and not the same as "no such port".
  HAPReport failed;
  failed.descriptorRev = 0x9C4A;
  failed.entries.push_back(HAPValueEntry(
      static_cast<uint8_t>(HAPClassId::Thermometer), 0, 0, HValue()));
  model.noteReport(pathOf(1), failed);

  const HValue* reading =
      model.find(pathOf(1))->value(static_cast<uint8_t>(HAPClassId::Thermometer), 0, 0);
  CHECK(reading != nullptr);
  CHECK(reading->isNull());
}

}  // namespace

void runModelTests() noexcept {
  HAPTest::begin("HAPModel");

  testAReportIsDiscovery();
  testTheRevisionDrivesEverything();
  testAPongCarriesTheRevisionToo();
  testAHalfArrivedDescriptorIsNotADescriptor();
  testChildAttachedNamesTheNodeBeforeItSpeaks();
  testADeviceThatMovedIsOneDeviceStill();
  testSilenceIsMeasuredAgainstAPromise();
  testANodeThatPromisedNothingIsNeverCalledDead();
  testHooksFireOnceEach();
  testTheModelHasABottom();
  testAValueThatIsNotThere();
}
