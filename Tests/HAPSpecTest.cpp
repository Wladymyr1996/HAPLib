#include "HAPTest.hpp"

#include <HAPFrame/HAPFrame.hpp>
#include <HAPMessages/HAPMessages.hpp>

#include <cstring>

/**
 * @file HAPSpecTest.cpp
 * @brief The worked examples from Docs/, rebuilt from the message structures
 *        and compared against the hexadecimal those documents print.
 *
 * This is what stops the specification and the implementation drifting apart.
 * A protocol document full of byte dumps is worth exactly as much as the last
 * time somebody checked them by hand - so nothing here is checked by hand.
 * Every expected string below is copied out of a document, and when the two
 * disagree, one of them is wrong and this suite says which bytes.
 *
 * Each example goes both ways: built from a structure and compared to the
 * document, then decoded from those same bytes and checked field by field. An
 * encoder and a decoder that share a misreading of the specification would
 * agree with each other perfectly, and only the document catches that.
 */

namespace {

/** The network used by every example: Docs/Protocol.md section 8. */
constexpr uint8_t kControllerIsChildOfGateway = 1;
constexpr uint8_t kThermometerIsChildOfController = 2;
constexpr uint8_t kLampIsChildOfController = 4;

constexpr uint16_t kDescriptorRev = 0x9C4A;

/** Wraps a payload in a frame and returns what went on the wire. */
size_t buildFrame(HAPMessage type, uint8_t flags, uint16_t seq,
                  const HAPPath& dest, const HAPPath& src,
                  const uint8_t* payload, size_t payloadSize, uint8_t* out,
                  size_t capacity) noexcept {
  HAPFrame frame;
  frame.type = static_cast<uint8_t>(type);
  frame.flags = flags;
  frame.seq = seq;
  frame.dest = dest;
  frame.src = src;
  frame.payload = payload;
  frame.payloadSize = static_cast<uint8_t>(payloadSize);

  return frame.encode(out, capacity);
}

HAPInstanceDescriptor descriptor(HAPClassId classId, uint8_t instanceId,
                                 const char* name) noexcept {
  HAPInstanceDescriptor instance;
  instance.classId = static_cast<uint8_t>(classId);
  instance.instanceId = instanceId;
  instance.flags = HAPInstanceFlags::None;
  instance.valueType = static_cast<uint8_t>(HAPValueType::Float);
  instance.name = name;
  return instance;
}

// -------------------------------------------------------------------------
// Docs/Protocol.md section 8.1 - binding a thermometer to the controller
// -------------------------------------------------------------------------

const char* kBindAnnounce =
    "4841 01 01 00 0100 00 0000000000 0000000000"
    "00"
    "01"
    "3C00"
    "4A9C"
    "03"
    "00 01"
    "07 426564726F6F6D"
    "01 00 00 03 04 54656D70"
    "02 01 00 03 03 48756D"
    "20 02 00 03 03 426174";

void testBindAnnounce() noexcept {
  HAPBindAnnounce announce;
  announce.deviceType = HAPDeviceType::Sensor;
  announce.capabilities = HAPCaps::BatteryPowered;
  announce.reportIntervalSec = 60;
  announce.descriptorRev = kDescriptorRev;
  announce.instanceCount = 3;
  announce.pageIndex = 0;
  announce.pageCount = 1;
  announce.nodeName = "Bedroom";

  // The instance id is unique within the NODE, not within the class - hence
  // 0, 1, 2 rather than three zeroes.
  announce.instances.push_back(descriptor(HAPClassId::Thermometer, 0, "Temp"));
  announce.instances.push_back(descriptor(HAPClassId::Hygrometer, 1, "Hum"));
  announce.instances.push_back(descriptor(HAPClassId::BatteryState, 2, "Bat"));

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  announce.encode(writer);
  CHECK(writer.ok());

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size =
      buildFrame(HAPMessage::BindAnnounce, HAPFlags::None, 1, HAPPath(),
                 HAPPath(), payload, writer.size(), buffer, sizeof(buffer));

  CHECK(size == 60);
  CHECK_BYTES(buffer, size, kBindAnnounce);

  // And back: the document's own bytes, parsed.
  uint8_t wire[HAP_MAX_FRAME_SIZE];
  const size_t wireSize = HAPTest::parseHex(kBindAnnounce, wire, sizeof(wire));

  HAPFrame frame;
  CHECK(HAPFrame::decode(wire, wireSize, frame) == HAPFrameError::None);
  CHECK(frame.message() == HAPMessage::BindAnnounce);
  CHECK(!frame.isUpstream());  // It has no parent yet.

  HAPReader reader(frame.payload, frame.payloadSize);
  HAPBindAnnounce parsed;
  CHECK(parsed.decode(reader));
  CHECK(parsed.deviceType == HAPDeviceType::Sensor);
  CHECK((parsed.capabilities & HAPCaps::BatteryPowered) != 0);
  CHECK(parsed.reportIntervalSec == 60);
  CHECK(parsed.descriptorRev == kDescriptorRev);
  CHECK(parsed.pageCount == 1);
  CHECK(std::strcmp(parsed.nodeName.c_str(), "Bedroom") == 0);
  CHECK(parsed.instances.size() == 3);
  CHECK(parsed.instances[1].classId == static_cast<uint8_t>(HAPClassId::Hygrometer));
  CHECK(parsed.instances[1].instanceId == 1);
  CHECK(std::strcmp(parsed.instances[2].name.c_str(), "Bat") == 0);
}

void testBindAcceptCarriesTheKey() noexcept {
  HAPBindAccept accept;
  accept.childIndex = kThermometerIsChildOfController;
  accept.channel = 1;
  accept.masterName = "Heating 01";
  for (uint8_t i = 0; i < HAP_KEY_LEN; ++i) {
    accept.linkKey[i] = static_cast<uint8_t>(0x70 + i);
  }

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  accept.encode(writer);
  CHECK(writer.ok());

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size =
      buildFrame(HAPMessage::BindAccept, HAPFlags::None, 5, HAPPath(), HAPPath(),
                 payload, writer.size(), buffer, sizeof(buffer));

  // The document prints the key as an ellipsis, so only its length is fixed
  // here - but the frame's total size is stated, and that is what catches a
  // field added or dropped.
  CHECK(size == 47);
  CHECK_BYTES(buffer, HAP_HEADER_SIZE + 2,
              "4841 01 02 00 0500 00 0000000000 0000000000"
              "02 01");

  HAPReader reader(payload, writer.size());
  HAPBindAccept parsed;
  CHECK(parsed.decode(reader));
  CHECK(parsed.childIndex == kThermometerIsChildOfController);
  CHECK(parsed.channel == 1);
  CHECK(parsed.linkKey[0] == 0x70);
  CHECK(parsed.linkKey[HAP_KEY_LEN - 1] == 0x7F);
  CHECK(std::strcmp(parsed.masterName.c_str(), "Heating 01") == 0);
}

void testBindConfirm() noexcept {
  HAPBindConfirm confirm;
  confirm.descriptorRev = kDescriptorRev;

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  confirm.encode(writer);

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size =
      buildFrame(HAPMessage::BindConfirm, HAPFlags::Upstream, 2, HAPPath(),
                 HAPPath(), payload, writer.size(), buffer, sizeof(buffer));

  CHECK(size == 20);
  CHECK_BYTES(buffer, size,
              "4841 01 03 02 0200 00 0000000000 0000000000"
              "4A9C");
}

void testChildAttached() noexcept {
  HAPChildAttached attached;
  attached.childIndex = kThermometerIsChildOfController;
  attached.deviceType = HAPDeviceType::Sensor;
  attached.capabilities = HAPCaps::BatteryPowered;
  attached.reportIntervalSec = 60;
  attached.descriptorRev = kDescriptorRev;

  const uint8_t thermometerMac[HAP_MAC_LEN] = {0x24, 0x6F, 0x28,
                                               0xAA, 0xBB, 0xCC};
  std::memcpy(attached.mac, thermometerMac, sizeof(thermometerMac));

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  attached.encode(writer);

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size =
      buildFrame(HAPMessage::ChildAttached, HAPFlags::Upstream, 6, HAPPath(),
                 HAPPath(), payload, writer.size(), buffer, sizeof(buffer));

  CHECK_BYTES(buffer, size,
              "4841 01 04 02 0600 00 0000000000 0000000000"
              "02"
              "00 01"
              "3C00"
              "4A9C"
              "246F28AABBCC");

  // The gateway receives it from ITS child 1, so the new node is at 1.2 - the
  // accumulated source path with the child index inside appended.
  HAPFrame atGateway;
  CHECK(HAPFrame::decode(buffer, size, atGateway) == HAPFrameError::None);
  CHECK(atGateway.src.prepend(kControllerIsChildOfGateway));

  HAPPath discovered = atGateway.src;
  CHECK(discovered.append(kThermometerIsChildOfController));
  CHECK(std::strcmp(discovered.toString().c_str(), "1.2") == 0);
}

// -------------------------------------------------------------------------
// Docs/Protocol.md section 8.2 - a report going up
// -------------------------------------------------------------------------

/** The three stages the same frame passes through, as the documents print them. */
const char* kReportAtThermometer =
    "4841 01 12 02 0701 00 0000000000 0000000000"
    "4A9C"
    "02"
    "01 00 00 03 0000AC41"
    "02 01 00 03 00003042";

const char* kReportAtController =
    "4841 01 12 02 0701 01 0000000000 0200000000"
    "4A9C"
    "02"
    "01 00 00 03 0000AC41"
    "02 01 00 03 00003042";

const char* kReportAtGateway =
    "4841 01 12 02 0701 02 0000000000 0102000000"
    "4A9C"
    "02"
    "01 00 00 03 0000AC41"
    "02 01 00 03 00003042";

void testReportLeavesTheSensor() noexcept {
  HAPReport report;
  report.descriptorRev = kDescriptorRev;
  report.entries.push_back(HAPValueEntry(
      static_cast<uint8_t>(HAPClassId::Thermometer), 0, 0, HValue(21.5f)));
  report.entries.push_back(HAPValueEntry(
      static_cast<uint8_t>(HAPClassId::Hygrometer), 1, 0, HValue(44.0f)));

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  report.encode(writer);
  CHECK(writer.ok());

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size =
      buildFrame(HAPMessage::Report, HAPFlags::Upstream, 0x0107, HAPPath(),
                 HAPPath(), payload, writer.size(), buffer, sizeof(buffer));

  CHECK(size == 37);
  CHECK_BYTES(buffer, size, kReportAtThermometer);
}

void testReportClimbsTwoHops() noexcept {
  uint8_t atThermometer[HAP_MAX_FRAME_SIZE];
  const size_t size =
      HAPTest::parseHex(kReportAtThermometer, atThermometer, sizeof(atThermometer));
  CHECK(size == 37);

  // The controller: it arrived from child 2, so that is what goes on the front.
  HAPFrame frame;
  CHECK(HAPFrame::decode(atThermometer, size, frame) == HAPFrameError::None);
  CHECK(frame.isUpstream());
  CHECK(frame.src.isEmpty());
  CHECK(frame.src.prepend(kThermometerIsChildOfController));

  uint8_t atController[HAP_MAX_FRAME_SIZE];
  const size_t controllerSize = frame.encode(atController, sizeof(atController));
  CHECK(controllerSize == size);
  CHECK_BYTES(atController, controllerSize, kReportAtController);

  // The gateway: it arrived from child 1.
  HAPFrame climbing;
  CHECK(HAPFrame::decode(atController, controllerSize, climbing) ==
        HAPFrameError::None);
  CHECK(climbing.src.prepend(kControllerIsChildOfGateway));

  uint8_t atGateway[HAP_MAX_FRAME_SIZE];
  const size_t gatewaySize = climbing.encode(atGateway, sizeof(atGateway));
  CHECK_BYTES(atGateway, gatewaySize, kReportAtGateway);

  // The invariant the whole design rests on: at every node, the source path is
  // the downward path from THAT node back to the origin.
  HAPFrame arrived;
  CHECK(HAPFrame::decode(atGateway, gatewaySize, arrived) == HAPFrameError::None);
  CHECK(std::strcmp(arrived.src.toString().c_str(), "1.2") == 0);

  // And the payload survived the journey untouched - a frame is relayed, not
  // rewritten.
  HAPReader reader(arrived.payload, arrived.payloadSize);
  HAPReport report;
  CHECK(report.decode(reader));
  CHECK(report.descriptorRev == kDescriptorRev);
  CHECK(report.entries.size() == 2);

  CHECK(report.entries[0].classId == static_cast<uint8_t>(HAPClassId::Thermometer));
  CHECK(report.entries[0].instanceId == 0);
  CHECK(report.entries[0].portId == 0);
  CHECK(report.entries[0].value.isFloat());
  CHECK(report.entries[0].value.asFloat() == 21.5f);

  CHECK(report.entries[1].instanceId == 1);
  CHECK(report.entries[1].value.asFloat() == 44.0f);
}

// -------------------------------------------------------------------------
// Docs/Protocol.md section 8.3 - a command going down
// -------------------------------------------------------------------------

void testCommandDescends() noexcept {
  const HAPWriteRequest request(static_cast<uint8_t>(HAPClassId::Lamp), 0, 0,
                                HValue(true));

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  request.encode(writer);
  CHECK(writer.ok());

  // The gateway resolved hop 1 itself, so what it emits already carries only
  // the remainder of the address.
  const uint8_t toLamp[] = {kLampIsChildOfController};

  uint8_t atController[HAP_MAX_FRAME_SIZE];
  const size_t size = buildFrame(
      HAPMessage::WriteRequest, HAPFlags::AckRequested, 0x0020,
      HAPPath::fromBytes(toLamp, 1), HAPPath(), payload, writer.size(),
      atController, sizeof(atController));

  CHECK_BYTES(atController, size,
              "4841 01 15 01 2000 10 0400000000 0000000000"
              "11 00 00 01 01");

  // The controller eats hop 4, and an empty destination means "for you".
  HAPFrame forwarding;
  CHECK(HAPFrame::decode(atController, size, forwarding) == HAPFrameError::None);
  CHECK(forwarding.dest.first() == kLampIsChildOfController);
  CHECK(forwarding.dest.shift());
  CHECK(forwarding.dest.isEmpty());

  uint8_t atLamp[HAP_MAX_FRAME_SIZE];
  const size_t lampSize = forwarding.encode(atLamp, sizeof(atLamp));
  CHECK_BYTES(atLamp, lampSize,
              "4841 01 15 01 2000 00 0000000000 0000000000"
              "11 00 00 01 01");

  // What the lamp makes of it.
  HAPFrame arrived;
  CHECK(HAPFrame::decode(atLamp, lampSize, arrived) == HAPFrameError::None);
  HAPReader reader(arrived.payload, arrived.payloadSize);
  const HAPWriteRequest parsed = HAPWriteRequest::decode(reader);
  CHECK(reader.ok());
  CHECK(parsed.classId == static_cast<uint8_t>(HAPClassId::Lamp));
  CHECK(parsed.portId == 0);
  CHECK(parsed.value.isBool());
  CHECK(parsed.value.asBool());
}

void testWriteResponseClimbsBack() noexcept {
  const HAPWriteResponse response(HAPResult::Ok,
                                  static_cast<uint8_t>(HAPClassId::Lamp), 0, 0,
                                  HValue(true));

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  response.encode(writer);

  HAPPath src;
  CHECK(src.prepend(kLampIsChildOfController));
  CHECK(src.prepend(kControllerIsChildOfGateway));

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = buildFrame(HAPMessage::WriteResponse, HAPFlags::Upstream,
                                 0x0020,  // Echoes the request: the response IS
                                          // the acknowledgement.
                                 HAPPath(), src, payload, writer.size(), buffer,
                                 sizeof(buffer));

  CHECK_BYTES(buffer, size,
              "4841 01 16 02 2000 02 0000000000 0104000000"
              "00 11 00 00 01 01");
}

// -------------------------------------------------------------------------
// Docs/Protocol.md section 8.4 - renaming a sleeping sensor
// -------------------------------------------------------------------------

void testRenameAndItsRevision() noexcept {
  HAPSetNameRequest request;
  request.target = HAPSetNameRequest::Target::Instance;
  request.classId = static_cast<uint8_t>(HAPClassId::Thermometer);
  request.instanceId = 0;
  request.name = "Ліжко";

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  request.encode(writer);

  CHECK(writer.ok());
  CHECK_BYTES(payload, writer.size(), "01 01 00 0A D09BD196D0B6D0BAD0BE");

  HAPSetNameResponse response;
  response.result = HAPResult::Ok;
  response.descriptorRev = 0x17E2;  // Recomputed: the name is part of it.

  uint8_t responsePayload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter responseWriter(responsePayload, sizeof(responsePayload));
  response.encode(responseWriter);

  CHECK_BYTES(responsePayload, responseWriter.size(), "00 E217");

  // The revision moved, which is the whole mechanism: a master holding 0x9C4A
  // sees 0x17E2 and knows to ask again.
  HAPReader reader(responsePayload, responseWriter.size());
  HAPSetNameResponse parsed;
  CHECK(parsed.decode(reader));
  CHECK(parsed.result == HAPResult::Ok);
  CHECK(parsed.descriptorRev != kDescriptorRev);
}

// -------------------------------------------------------------------------
// Docs/Links.md section 8 - wiring a thermometer to a regulator
// -------------------------------------------------------------------------

void testSetLink() noexcept {
  const uint8_t sourceHop[] = {1};

  HAPLinkSpec link;
  link.linkId = 0;

  // Source: the controller's own child 1. A downward path, like every path in a
  // link, because both endpoints are at or below the node that holds it.
  link.source.path = HAPPath::fromBytes(sourceHop, 1);
  link.source.classId = static_cast<uint8_t>(HAPClassId::Thermometer);
  link.source.instanceId = 0;
  link.source.portId = 0;

  // Destination: itself - in port 0 of its Regulator, which is Measured.
  link.destination.path = HAPPath();
  link.destination.classId = static_cast<uint8_t>(HAPClassId::Regulator);
  link.destination.instanceId = 0;
  link.destination.portId = 0;

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  link.encode(writer);

  CHECK(writer.ok());
  CHECK(writer.size() == HAPLinkSpec::kEncodedSize);
  CHECK(writer.size() == 19);

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size =
      buildFrame(HAPMessage::SetLinkRequest, HAPFlags::AckRequested, 0x0031,
                 HAPPath(), HAPPath(), payload, writer.size(), buffer,
                 sizeof(buffer));

  CHECK(size == 37);
  CHECK_BYTES(buffer, size,
              "4841 01 50 01 3100 00 0000000000 0000000000"
              "00"
              "01 0100000000"
              "01 00 00"
              "00 0000000000"
              "30 00 00");

  HAPReader reader(payload, writer.size());
  HAPLinkSpec parsed;
  CHECK(parsed.decode(reader));
  CHECK(parsed.source.path.length() == 1);
  CHECK(parsed.source.path.hop(0) == 1);
  CHECK(parsed.destination.path.isEmpty());
  CHECK(parsed.destination.classId == static_cast<uint8_t>(HAPClassId::Regulator));
}

}  // namespace

void runSpecTests() noexcept {
  HAPTest::begin("Documented examples");

  testBindAnnounce();
  testBindAcceptCarriesTheKey();
  testBindConfirm();
  testChildAttached();
  testReportLeavesTheSensor();
  testReportClimbsTwoHops();
  testCommandDescends();
  testWriteResponseClimbsBack();
  testRenameAndItsRevision();
  testSetLink();
}
