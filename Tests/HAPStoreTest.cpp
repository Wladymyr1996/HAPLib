#include "HAPTest.hpp"

#include <HAPStore/HAPStore.hpp>
#include <HConfig/HConfig.hpp>
#include <HFs/HFs.hpp>

#include <cstring>

/**
 * @file HAPStoreTest.cpp
 * @brief A bind that survives.
 *
 * These run against the real HConfig on the real desktop filesystem - the same
 * parser, writer and crash-safe rename the device uses, not a stand-in. What
 * cannot be tested here is retained memory across a genuine deep sleep, since a
 * host process that exits takes its RAM with it; the mirror is exercised as
 * memory instead, and the rig's milestone A5 is where a real wake is checked.
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

HAPBindState makeBind() noexcept {
  HAPBindState state;
  state.bound = true;
  state.parent = macFor(1);
  state.indexAtParent = 2;
  state.channel = 6;

  for (uint8_t i = 0; i < HAP_KEY_LEN; ++i) {
    state.linkKey[i] = static_cast<uint8_t>(0xA0 + i);
  }

  return state;
}

/** Wipes both the files and the retained copy, so each test starts clean. */
void reset() noexcept {
  HAPStore::clearAll();
}

void testBindSurvivesTheFile() noexcept {
  reset();

  HAPBindState loaded;
  CHECK(!HAPStore::loadBind(loaded));  // Never bound.

  const HAPBindState saved = makeBind();
  CHECK(HAPStore::saveBind(saved));

  // Wipe the retained copy but leave the file, which is what a power cut does -
  // and the case where the file has to be believed.
  HAPStore::init();
  reset();
  CHECK(HAPStore::saveBind(saved));

  HAPBindState restored;
  CHECK(HAPStore::loadBind(restored));
  CHECK(restored.bound);
  CHECK(restored.parent == saved.parent);
  CHECK(restored.indexAtParent == 2);
  CHECK(restored.channel == 6);
  CHECK(std::memcmp(restored.linkKey, saved.linkKey, HAP_KEY_LEN) == 0);
}

void testTheMirrorIsPreferred() noexcept {
  reset();

  const HAPBindState saved = makeBind();
  CHECK(HAPStore::saveBind(saved));

  HAPBindState restored;
  CHECK(HAPStore::loadBind(restored));

  // A timer wake reads retained memory and never opens a file, which on a
  // battery node is most of what the wake would otherwise have cost.
  CHECK(HAPStore::restoredFromRtc());
  CHECK(restored.parent == saved.parent);
  CHECK(std::memcmp(restored.linkKey, saved.linkKey, HAP_KEY_LEN) == 0);
}

void testAFactoryResetForgetsBothCopies() noexcept {
  reset();

  CHECK(HAPStore::saveBind(makeBind()));

  HAPBindState check;
  CHECK(HAPStore::loadBind(check));

  HAPStore::clearBind();

  // Clearing only the file would leave the next wake restoring the parent that
  // was just deleted - which is the bug this test exists for.
  HAPBindState afterReset;
  CHECK(!HAPStore::loadBind(afterReset));
  CHECK(!HAPStore::restoredFromRtc());
}

void testChildrenRoundTrip() noexcept {
  reset();

  HAPRouter saved;

  for (uint8_t i = 1; i <= 3; ++i) {
    HAPChild child;
    child.index = i;
    child.mac = macFor(static_cast<uint8_t>(0x10 + i));
    child.capabilities = i == 2 ? HAPCaps::BatteryPowered : HAPCaps::CanBeMaster;
    child.deviceType = i == 2 ? HAPDeviceType::Sensor : HAPDeviceType::Controller;
    child.descriptorRev = static_cast<uint16_t>(0x9C40 + i);
    CHECK(saved.addChild(child));
  }

  CHECK(HAPStore::saveChildren(saved));

  HAPRouter restored;
  CHECK(HAPStore::loadChildren(restored) == 3);
  CHECK(restored.childCount() == 3);

  const HAPChild* battery = restored.child(2);
  CHECK(battery != nullptr);
  CHECK(battery->mac == macFor(0x12));
  CHECK(battery->isBatteryPowered());
  CHECK(battery->deviceType == HAPDeviceType::Sensor);

  // The revision matters as much as the address: a master that forgot it would
  // re-describe every child on every boot.
  CHECK(battery->descriptorRev == 0x9C42);

  CHECK(restored.child(1)->deviceType == HAPDeviceType::Controller);
  CHECK(!restored.child(3)->isBatteryPowered());

  // And the free index picks up where the stored table left off.
  CHECK(restored.freeChildIndex() == 4);
}

void testAnEmptyChildTableIsWrittenToo() noexcept {
  reset();

  // A master whose last child was removed must record the removal, or the next
  // boot restores a child that is gone.
  HAPRouter full;
  HAPChild child;
  child.index = 1;
  child.mac = macFor(0x20);
  CHECK(full.addChild(child));
  CHECK(HAPStore::saveChildren(full));

  full.clearChildren();
  CHECK(HAPStore::saveChildren(full));

  HAPRouter restored;
  CHECK(HAPStore::loadChildren(restored) == 0);
  CHECK(restored.childCount() == 0);
}

void testNamesRoundTrip() noexcept {
  reset();

  HAPNode saved;
  saved.begin(HAPDeviceType::Sensor, HAPCaps::BatteryPowered, HAPName("Bedroom"));
  saved.addInstance(HAPClassId::Thermometer, HAPName("Temp"));
  saved.addInstance(HAPClassId::Hygrometer, HAPName("Hum"));

  const uint16_t revBefore = saved.descriptorRev();

  // The rename from Docs/Protocol.md section 8.4.
  saved.instanceAt(0)->setName(HAPName("Ліжко"));
  saved.setName(HAPName("Nursery"));
  CHECK(HAPStore::saveNames(saved));

  // A fresh boot: the firmware builds its instances with the names it was
  // compiled with, and storage lays the user's over them.
  HAPNode restored;
  restored.begin(HAPDeviceType::Sensor, HAPCaps::BatteryPowered, HAPName("Bedroom"));
  restored.addInstance(HAPClassId::Thermometer, HAPName("Temp"));
  restored.addInstance(HAPClassId::Hygrometer, HAPName("Hum"));

  CHECK(restored.descriptorRev() == revBefore);  // Before names are applied.

  HAPStore::loadNames(restored);

  CHECK(std::strcmp(restored.name().c_str(), "Nursery") == 0);
  CHECK(std::strcmp(restored.instanceAt(0)->name().c_str(), "Ліжко") == 0);
  CHECK(std::strcmp(restored.instanceAt(1)->name().c_str(), "Hum") == 0);

  // The revision follows the names across the reboot, which is what stops a
  // master re-reading a descriptor it already has.
  CHECK(restored.descriptorRev() == saved.descriptorRev());
  CHECK(restored.descriptorRev() != revBefore);
}

void testAnUnnamedInstanceKeepsItsFirmwareName() noexcept {
  reset();

  HAPNode saved;
  saved.begin(HAPDeviceType::Sensor, HAPCaps::None, HAPName("Node"));
  saved.addInstance(HAPClassId::Thermometer, HAPName("Temp"));
  CHECK(HAPStore::saveNames(saved));

  // A firmware update added an instance the stored file never heard of.
  HAPNode upgraded;
  upgraded.begin(HAPDeviceType::Sensor, HAPCaps::None, HAPName("Node"));
  upgraded.addInstance(HAPClassId::Thermometer, HAPName("Temp"));
  upgraded.addInstance(HAPClassId::Barometer, HAPName("Press"));

  HAPStore::loadNames(upgraded);

  CHECK(std::strcmp(upgraded.instanceAt(0)->name().c_str(), "Temp") == 0);
  CHECK(std::strcmp(upgraded.instanceAt(1)->name().c_str(), "Press") == 0);
}

void testCorruptStorageIsRefusedRatherThanBelieved() noexcept {
  reset();

  // A link with no key: half a bind is not a bind, and a node that acted on it
  // would send unencrypted frames to a parent expecting encrypted ones.
  const HConfigEntry entries[] = {
      {"link", HValue("246f28000001020600")},
  };
  CHECK(HConfig::write("hapbind", etl::span<const HConfigEntry>(entries, 1)));

  HAPBindState state;
  CHECK(!HAPStore::loadBind(state));

  // And a truncated one is not padded with zeroes into something plausible.
  const HConfigEntry short_[] = {
      {"key", HValue("a0a1a2")},
      {"link", HValue("246f28")},
  };
  CHECK(HConfig::write("hapbind", etl::span<const HConfigEntry>(short_, 2)));
  CHECK(!HAPStore::loadBind(state));
}

void testEverythingTogether() noexcept {
  reset();

  // What a controller looks like after a bind: a parent above, a child below,
  // and a name a user chose.
  const HAPBindState bind = makeBind();
  CHECK(HAPStore::saveBind(bind));

  HAPRouter router;
  HAPChild child;
  child.index = 1;
  child.mac = macFor(0x30);
  child.capabilities = HAPCaps::BatteryPowered;
  child.descriptorRev = 0x1234;
  CHECK(router.addChild(child));
  CHECK(HAPStore::saveChildren(router));

  HAPNode node;
  node.begin(HAPDeviceType::Controller, HAPCaps::CanBeMaster, HAPName("Heating"));
  node.addInstance(HAPClassId::Regulator, HAPName("Loop"));
  CHECK(HAPStore::saveNames(node));

  // Each file is written for its own reason, so recording a rename must not
  // disturb the bind or the children - which one shared file would have done.
  node.instanceAt(0)->setName(HAPName("Kitchen loop"));
  CHECK(HAPStore::saveNames(node));

  HAPBindState restoredBind;
  CHECK(HAPStore::loadBind(restoredBind));
  CHECK(restoredBind.parent == bind.parent);

  HAPRouter restoredRouter;
  CHECK(HAPStore::loadChildren(restoredRouter) == 1);
  CHECK(restoredRouter.child(1)->descriptorRev == 0x1234);

  HAPNode restoredNode;
  restoredNode.begin(HAPDeviceType::Controller, HAPCaps::CanBeMaster,
                     HAPName("Heating"));
  restoredNode.addInstance(HAPClassId::Regulator, HAPName("Loop"));
  HAPStore::loadNames(restoredNode);
  CHECK(std::strcmp(restoredNode.instanceAt(0)->name().c_str(), "Kitchen loop") == 0);

  reset();
}

}  // namespace

void runStoreTests() noexcept {
  HAPTest::begin("HAPStore");

  // The application owns the filesystem and the config subsystem; HAPLib only
  // uses them. A test has to stand in for that application.
  HFs::HFileSystem::mount();
  HConfig::init();
  HAPStore::init();

  testBindSurvivesTheFile();
  testTheMirrorIsPreferred();
  testAFactoryResetForgetsBothCopies();
  testChildrenRoundTrip();
  testAnEmptyChildTableIsWrittenToo();
  testNamesRoundTrip();
  testAnUnnamedInstanceKeepsItsFirmwareName();
  testCorruptStorageIsRefusedRatherThanBelieved();
  testEverythingTogether();
}
