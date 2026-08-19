#include "HAPTest.hpp"

#include <HAPClasses/HAPClasses.hpp>
#include <HAPCrc16/HAPCrc16.hpp>
#include <HAPNode/HAPNode.hpp>

#include <cstring>

namespace {

// -------------------------------------------------------------------------
// CRC
// -------------------------------------------------------------------------

void testCrcAgainstAKnownVector() noexcept {
  // The check value every CRC-16/CCITT-FALSE implementation publishes. If this
  // is right, a node built from another toolchain computes the same revision.
  const char* check = "123456789";
  CHECK(HAPCrc16(reinterpret_cast<const uint8_t*>(check), 9) == 0x29B1);

  CHECK(HAPCrc16(nullptr, 4) == kHAPCrc16Init);
  CHECK(HAPCrc16(reinterpret_cast<const uint8_t*>(""), 0) == kHAPCrc16Init);

  // Running it in two pieces must equal running it in one, or paging a
  // descriptor would change its revision.
  const uint16_t whole = HAPCrc16(reinterpret_cast<const uint8_t*>(check), 9);
  uint16_t split = HAPCrc16(reinterpret_cast<const uint8_t*>(check), 4);
  split = HAPCrc16Update(split, reinterpret_cast<const uint8_t*>(check + 4), 5);
  CHECK(whole == split);
}

// -------------------------------------------------------------------------
// The class table
// -------------------------------------------------------------------------

void testClassTableMatchesTheDocuments() noexcept {
  const HAPClassSpec* thermometer =
      HAPClasses::find(static_cast<uint8_t>(HAPClassId::Thermometer));
  CHECK(thermometer != nullptr);
  CHECK(thermometer->countPorts(HAPPortDirection::Out) == 1);
  CHECK(thermometer->countPorts(HAPPortDirection::In) == 0);

  const HAPPortSpec* temperature =
      thermometer->find(HAPPortDirection::Out, 0);
  CHECK(temperature != nullptr);
  CHECK(temperature->kind == HAPKind::Temperature);
  CHECK(temperature->valueType == HAPValueType::Float);
  CHECK(std::strcmp(temperature->name, "Temperature") == 0);

  // Docs/Links.md's worked example depends on this class having exactly these
  // ports.
  const HAPClassSpec* regulator =
      HAPClasses::find(static_cast<uint8_t>(HAPClassId::Regulator));
  CHECK(regulator != nullptr);
  CHECK(regulator->countPorts(HAPPortDirection::In) == 2);
  CHECK(regulator->countPorts(HAPPortDirection::Out) == 1);
  CHECK(std::strcmp(regulator->find(HAPPortDirection::In, 0)->name, "Measured") == 0);
  CHECK(std::strcmp(regulator->find(HAPPortDirection::In, 1)->name, "Setpoint") == 0);
  CHECK(regulator->find(HAPPortDirection::Out, 0)->kind == HAPKind::Ratio);

  // Private classes are nobody else's business and are not in the table.
  CHECK(HAPClasses::find(0x80) == nullptr);
  CHECK(HAPClasses::find(0x77) == nullptr);
}

void testAPortNumberMeansNothingWithoutADirection() noexcept {
  // A lamp's state is out port 0 AND in port 0 - the same lamp from two sides.
  // Nothing on the wire ever names a port without saying which way it faces, so
  // sharing the number costs nothing.
  const uint8_t lamp = static_cast<uint8_t>(HAPClassId::Lamp);

  CHECK(HAPClasses::port(lamp, HAPPortDirection::Out, 0) != nullptr);
  CHECK(HAPClasses::port(lamp, HAPPortDirection::In, 0) != nullptr);
  CHECK(HAPClasses::isWritable(lamp));

  const uint8_t thermometer = static_cast<uint8_t>(HAPClassId::Thermometer);
  CHECK(HAPClasses::port(thermometer, HAPPortDirection::Out, 0) != nullptr);
  CHECK(HAPClasses::port(thermometer, HAPPortDirection::In, 0) == nullptr);
  CHECK(!HAPClasses::isWritable(thermometer));
}

void testLinkValidation() noexcept {
  const uint8_t thermometer = static_cast<uint8_t>(HAPClassId::Thermometer);
  const uint8_t hygrometer = static_cast<uint8_t>(HAPClassId::Hygrometer);
  const uint8_t regulator = static_cast<uint8_t>(HAPClassId::Regulator);
  const uint8_t lamp = static_cast<uint8_t>(HAPClassId::Lamp);

  // Docs/Links.md section 8: a thermometer into a regulator's Measured input.
  CHECK(HAPClasses::validateLink(thermometer, 0, regulator, 0) == HAPResult::Ok);
  CHECK(HAPClasses::validateLink(thermometer, 0, regulator, 1) == HAPResult::Ok);

  // The check the whole quantity-kind idea exists for. Both of these are floats
  // in a plausible range, and a comparison of ENCODINGS would allow it.
  CHECK(HAPClasses::validateLink(hygrometer, 0, regulator, 0) ==
        HAPResult::TypeMismatch);

  // A thermometer has nothing to drive.
  CHECK(HAPClasses::validateLink(thermometer, 0, thermometer, 0) ==
        HAPResult::NotWritable);

  // A lamp has an input, but not a second one.
  CHECK(HAPClasses::validateLink(lamp, 0, lamp, 0) == HAPResult::Ok);
  CHECK(HAPClasses::validateLink(lamp, 0, lamp, 3) == HAPResult::NoSuchPort);

  CHECK(HAPClasses::validateLink(thermometer, 7, regulator, 0) ==
        HAPResult::NoSuchPort);
  CHECK(HAPClasses::validateLink(0x80, 0, regulator, 0) == HAPResult::NoSuchClass);
}

// -------------------------------------------------------------------------
// Instances
// -------------------------------------------------------------------------

void testInstanceStartsWithNoReading() noexcept {
  HAPInstance instance;
  CHECK(!instance.isConfigured());

  CHECK(instance.configure(static_cast<uint8_t>(HAPClassId::Thermometer), 0,
                           HAPName("Temp")));
  CHECK(instance.isConfigured());

  // Null, not zero. A thermometer whose sensor has not answered yet must not
  // appear to be reading 0 degrees.
  CHECK(instance.read(0).isNull());

  CHECK(instance.publish(0, HValue(21.5f)));
  CHECK(instance.read(0).isFloat());
  CHECK(instance.read(0).asFloat() == 21.5f);

  // And it can go back to having none, which is what a failed conversion means.
  CHECK(instance.publish(0, HValue()));
  CHECK(instance.read(0).isNull());
}

void testInstanceRefusesTheWrongType() noexcept {
  HAPInstance instance;
  CHECK(instance.configure(static_cast<uint8_t>(HAPClassId::Thermometer), 0,
                           HAPName("Temp")));

  // A hygrometer publishing a Bool is a bug in the application, and a silent
  // one if this were allowed.
  CHECK(!instance.publish(0, HValue(true)));
  CHECK(!instance.publish(0, HValue(21)));
  CHECK(instance.read(0).isNull());

  // And a port that does not exist is not created by writing to it.
  CHECK(!instance.publish(3, HValue(21.5f)));
}

void testInstanceWriteRules() noexcept {
  HAPInstance regulator;
  CHECK(regulator.configure(static_cast<uint8_t>(HAPClassId::Regulator), 0,
                            HAPName("Heat")));

  // Never fed, and it says so - which is what a staleness timeout is measured
  // against, and what a class's failsafe hangs off.
  CHECK(regulator.input(0).isNull());
  CHECK(regulator.inputUpdatedAtMs(0) == 0);
  CHECK(regulator.inputUpdatedAtMs(9) == 0);

  CHECK(regulator.write(0, HValue(21.5f)) == HAPResult::Ok);
  CHECK(regulator.input(0).asFloat() == 21.5f);

  CHECK(regulator.write(1, HValue(22.0f)) == HAPResult::Ok);
  CHECK(regulator.write(2, HValue(1.0f)) == HAPResult::NoSuchPort);
  CHECK(regulator.write(0, HValue(true)) == HAPResult::BadValue);

  // A thermometer's port faces outward, and saying so is more use than
  // "no such port" at the far end.
  HAPInstance thermometer;
  CHECK(thermometer.configure(static_cast<uint8_t>(HAPClassId::Thermometer), 0,
                              HAPName("Temp")));
  CHECK(thermometer.write(0, HValue(21.5f)) == HAPResult::NotWritable);
}

void testInstanceDescribesItself() noexcept {
  HAPInstance lamp;
  CHECK(lamp.configure(static_cast<uint8_t>(HAPClassId::Lamp), 3, HAPName("Hall")));

  const HAPInstanceDescriptor descriptor = lamp.describe();
  CHECK(descriptor.classId == static_cast<uint8_t>(HAPClassId::Lamp));
  CHECK(descriptor.instanceId == 3);
  CHECK(descriptor.valueType == static_cast<uint8_t>(HAPValueType::Bool));
  CHECK((descriptor.flags & HAPInstanceFlags::Writable) != 0);
  CHECK(std::strcmp(descriptor.name.c_str(), "Hall") == 0);

  HAPInstance thermometer;
  CHECK(thermometer.configure(static_cast<uint8_t>(HAPClassId::Thermometer), 0,
                              HAPName("Temp")));
  CHECK((thermometer.describe().flags & HAPInstanceFlags::Writable) == 0);
}

// -------------------------------------------------------------------------
// The node
// -------------------------------------------------------------------------

HAPNode makeThermometerNode() noexcept {
  HAPNode node;
  node.begin(HAPDeviceType::Sensor, HAPCaps::BatteryPowered, HAPName("Bedroom"));
  node.setReportIntervalSec(60);

  node.addInstance(HAPClassId::Thermometer, HAPName("Temp"));
  node.addInstance(HAPClassId::Hygrometer, HAPName("Hum"));
  node.addInstance(HAPClassId::Battery, HAPName("Bat"));

  return node;
}

void testNodeAssignsInstanceIds() noexcept {
  HAPNode node = makeThermometerNode();

  CHECK(node.instanceCount() == 3);

  // Unique within the NODE, not within the class - which is what makes
  // (classId, instanceId) address one thing unambiguously.
  CHECK(node.instanceAt(0)->instanceId() == 0);
  CHECK(node.instanceAt(1)->instanceId() == 1);
  CHECK(node.instanceAt(2)->instanceId() == 2);

  CHECK(node.instance(static_cast<uint8_t>(HAPClassId::Hygrometer), 1) != nullptr);
  CHECK(node.instance(static_cast<uint8_t>(HAPClassId::Hygrometer), 0) == nullptr);

  // A class this build does not know cannot be added: nothing here knows its
  // ports, so it could not be read, written or wired.
  CHECK(node.addInstance(static_cast<HAPClassId>(0x80), HAPName("x")) == nullptr);
}

void testRevisionIsStableAndContentAddressed() noexcept {
  HAPNode first = makeThermometerNode();
  HAPNode second = makeThermometerNode();

  // Two nodes built the same way agree, with nothing stored anywhere and no
  // counter to get out of step.
  CHECK(first.descriptorRev() == second.descriptorRev());

  const uint16_t before = first.descriptorRev();

  // A reading is not part of the descriptor: reporting must not make a master
  // re-read what a node IS every minute.
  first.instanceAt(0)->publish(0, HValue(21.5f));
  CHECK(first.descriptorRev() == before);

  // A rename is. This is the whole mechanism from Docs/Protocol.md section 8.4.
  first.instanceAt(0)->setName(HAPName("Ліжко"));
  CHECK(first.descriptorRev() != before);

  // And it is content-addressed, so renaming it back restores the old number
  // rather than inventing a third.
  first.instanceAt(0)->setName(HAPName("Temp"));
  CHECK(first.descriptorRev() == before);

  // The node's own name counts too, as does what kind of device it is - a slot
  // whose device was swapped must not keep the old revision.
  first.setName(HAPName("Nursery"));
  CHECK(first.descriptorRev() != before);
}

void testDescribeFitsInOnePage() noexcept {
  HAPNode node = makeThermometerNode();

  CHECK(node.pageCount() == 1);

  HAPDescribeResponse response;
  CHECK(node.fillDescribe(response, 0));
  CHECK(!node.fillDescribe(response, 1));

  CHECK(response.instanceCount == 3);
  CHECK(response.instances.size() == 3);
  CHECK(response.pageCount == 1);
  CHECK(response.descriptorRev == node.descriptorRev());
  CHECK(std::strcmp(response.nodeName.c_str(), "Bedroom") == 0);

  // And it encodes inside one frame, which is what a page means.
  uint8_t buffer[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(buffer, sizeof(buffer));
  response.encode(writer);
  CHECK(writer.ok());
}

void testDescribePagesWhenItMustAndEveryPageFits() noexcept {
  // Eight instances with the longest names the protocol allows: 31 bytes each,
  // which no single frame can carry.
  HAPNode node;
  node.begin(HAPDeviceType::Controller, HAPCaps::CanBeMaster,
             HAPName("Boiler house controller unit 1"));

  const HAPName longName("Circulating pump return line 12");
  CHECK(longName.size() == HAP_MAX_NAME_LEN);

  for (size_t i = 0; i < HAP_MAX_INSTANCES; ++i) {
    CHECK(node.addInstance(HAPClassId::Thermometer, longName) != nullptr);
  }

  const uint8_t pages = node.pageCount();
  CHECK(pages > 1);

  size_t seen = 0;

  for (uint8_t page = 0; page < pages; ++page) {
    HAPDescribeResponse response;
    CHECK(node.fillDescribe(response, page));
    CHECK(response.pageCount == pages);
    CHECK(response.pageIndex == page);
    CHECK(response.instanceCount == HAP_MAX_INSTANCES);
    CHECK(!response.instances.empty());

    // The claim a page makes: it fits in one frame, on its own, node name
    // included.
    uint8_t buffer[HAP_MAX_PAYLOAD_SIZE];
    HAPWriter writer(buffer, sizeof(buffer));
    response.encode(writer);
    CHECK(writer.ok());

    seen += response.instances.size();
  }

  // Every instance appears exactly once across the pages, and there is no page
  // after the last.
  CHECK(seen == HAP_MAX_INSTANCES);

  HAPDescribeResponse pastTheEnd;
  CHECK(!node.fillDescribe(pastTheEnd, pages));
}

void testAnnouncePagesTheSameWay() noexcept {
  HAPNode node = makeThermometerNode();

  HAPBindAnnounce announce;
  CHECK(node.fillAnnounce(announce, 0));
  CHECK(announce.deviceType == HAPDeviceType::Sensor);
  CHECK((announce.capabilities & HAPCaps::BatteryPowered) != 0);
  CHECK(announce.reportIntervalSec == 60);
  CHECK(announce.instances.size() == 3);
  CHECK(announce.descriptorRev == node.descriptorRev());
}

void testReportCarriesEveryOutPort() noexcept {
  HAPNode node = makeThermometerNode();
  node.instanceAt(0)->publish(0, HValue(21.5f));
  node.instanceAt(1)->publish(0, HValue(44.0f));

  HAPReport report;
  node.fillReport(report);

  CHECK(report.descriptorRev == node.descriptorRev());
  CHECK(report.entries.size() == 3);
  CHECK(report.entries[0].value.asFloat() == 21.5f);
  CHECK(report.entries[1].value.asFloat() == 44.0f);

  // The battery has never been measured, and the report says so rather than
  // claiming zero volts.
  CHECK(report.entries[2].value.isNull());
}

void testReadWildcards() noexcept {
  HAPNode node = makeThermometerNode();
  node.instanceAt(0)->publish(0, HValue(21.5f));

  HAPReadRequest everything;  // Defaults are all wildcards.
  HAPReport report;
  CHECK(node.read(everything, report) == HAPResult::Ok);
  CHECK(report.entries.size() == 3);

  HAPReadRequest oneInstance;
  oneInstance.classId = static_cast<uint8_t>(HAPClassId::Thermometer);
  oneInstance.instanceId = 0;
  oneInstance.portId = 0;
  CHECK(node.read(oneInstance, report) == HAPResult::Ok);
  CHECK(report.entries.size() == 1);
  CHECK(report.entries[0].value.asFloat() == 21.5f);

  HAPReadRequest missing;
  missing.classId = static_cast<uint8_t>(HAPClassId::Lamp);
  missing.instanceId = 0;
  CHECK(node.read(missing, report) == HAPResult::NoSuchClass);

  // A wildcard matching nothing is an answer, not an error: "I have none of
  // those" is what an empty report means.
  HAPReadRequest wildcardOfAbsentClass;
  wildcardOfAbsentClass.classId = static_cast<uint8_t>(HAPClassId::Lamp);
  CHECK(node.read(wildcardOfAbsentClass, report) == HAPResult::NoSuchClass);
}

void testWriteReportsWhatWasTaken() noexcept {
  HAPNode node;
  node.begin(HAPDeviceType::Controller, HAPCaps::CanBeMaster, HAPName("Heat"));
  node.addInstance(HAPClassId::Regulator, HAPName("Loop"));

  const HAPWriteRequest measured(static_cast<uint8_t>(HAPClassId::Regulator), 0,
                                 0, HValue(21.5f));
  const HAPWriteResponse accepted = node.write(measured);

  CHECK(accepted.result == HAPResult::Ok);
  CHECK(accepted.value.isFloat());
  CHECK(accepted.value.asFloat() == 21.5f);

  // Read back from the port rather than echoed from the request, so an instance
  // that ignored or clamped a value says so.
  const HAPWriteRequest wrongType(static_cast<uint8_t>(HAPClassId::Regulator), 0,
                                  0, HValue(true));
  const HAPWriteResponse refused = node.write(wrongType);
  CHECK(refused.result == HAPResult::BadValue);
  CHECK(refused.value.asFloat() == 21.5f);  // Still what it was.

  const HAPWriteRequest absent(static_cast<uint8_t>(HAPClassId::Lamp), 0, 0,
                               HValue(true));
  CHECK(node.write(absent).result == HAPResult::NoSuchClass);
}

void testRenameMovesTheRevision() noexcept {
  HAPNode node = makeThermometerNode();
  const uint16_t before = node.descriptorRev();

  HAPSetNameRequest request;
  request.target = HAPSetNameRequest::Target::Instance;
  request.classId = static_cast<uint8_t>(HAPClassId::Thermometer);
  request.instanceId = 0;
  request.name = HAPName("Ліжко");

  const HAPSetNameResponse response = node.rename(request);
  CHECK(response.result == HAPResult::Ok);
  CHECK(response.descriptorRev != before);
  CHECK(response.descriptorRev == node.descriptorRev());
  CHECK(std::strcmp(node.instanceAt(0)->name().c_str(), "Ліжко") == 0);

  // The node itself, which ignores the class and instance fields.
  HAPSetNameRequest nodeRename;
  nodeRename.target = HAPSetNameRequest::Target::Node;
  nodeRename.name = HAPName("Nursery");
  CHECK(node.rename(nodeRename).result == HAPResult::Ok);
  CHECK(std::strcmp(node.name().c_str(), "Nursery") == 0);

  HAPSetNameRequest missing;
  missing.target = HAPSetNameRequest::Target::Instance;
  missing.classId = static_cast<uint8_t>(HAPClassId::Lamp);
  missing.name = HAPName("x");
  CHECK(node.rename(missing).result == HAPResult::NoSuchClass);
}

void testAnnounceMatchesTheDocumentedExample() noexcept {
  // Docs/Protocol.md section 8.1, built by a node rather than by hand: the same
  // three instances, the same names, the same instance ids.
  HAPNode node;
  node.begin(HAPDeviceType::Sensor, HAPCaps::BatteryPowered, HAPName("Bedroom"));
  node.setReportIntervalSec(60);
  node.addInstance(HAPClassId::Thermometer, HAPName("Temp"));
  node.addInstance(HAPClassId::Hygrometer, HAPName("Hum"));
  node.addInstance(HAPClassId::Battery, HAPName("Bat"));

  HAPBindAnnounce announce;
  CHECK(node.fillAnnounce(announce, 0));

  uint8_t buffer[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(buffer, sizeof(buffer));
  announce.encode(writer);
  CHECK(writer.ok());

  // Byte for byte the document's payload, either side of the revision - which
  // the document invented as 0x9C4A before any of this existed, and which is
  // now computed from the content. Everything else a node builds must match a
  // specification written by hand months earlier.
  CHECK_BYTES(buffer, 4,
              "00"     // sensor
              "01"     // battery-powered
              "3C00")  // 60 s
  ;

  CHECK_BYTES(buffer + 6, writer.size() - 6,
              "03"
              "00 01"
              "07 426564726F6F6D"
              "01 00 00 03 04 54656D70"
              "02 01 00 03 03 48756D"
              "20 02 00 03 03 426174");

  // The revision occupies the two bytes between, little-endian as everything is.
  const uint16_t encodedRev =
      static_cast<uint16_t>(buffer[4] | (buffer[5] << 8));
  CHECK(encodedRev == node.descriptorRev());
}

}  // namespace

void runNodeTests() noexcept {
  HAPTest::begin("HAPNode");

  testCrcAgainstAKnownVector();
  testClassTableMatchesTheDocuments();
  testAPortNumberMeansNothingWithoutADirection();
  testLinkValidation();
  testInstanceStartsWithNoReading();
  testInstanceRefusesTheWrongType();
  testInstanceWriteRules();
  testInstanceDescribesItself();
  testNodeAssignsInstanceIds();
  testRevisionIsStableAndContentAddressed();
  testDescribeFitsInOnePage();
  testDescribePagesWhenItMustAndEveryPageFits();
  testAnnouncePagesTheSameWay();
  testReportCarriesEveryOutPort();
  testReadWildcards();
  testWriteReportsWhatWasTaken();
  testRenameMovesTheRevision();
  testAnnounceMatchesTheDocumentedExample();
}
