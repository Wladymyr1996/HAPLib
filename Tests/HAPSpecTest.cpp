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
 * Every expected string below is written out byte by byte, and when the
 * encoder disagrees, one of them is wrong and this suite says which bytes.
 *
 * The frames follow Docs/ThreadDesign.md section 5 (version 2: a 23-byte
 * header with the destination id at offset 7 and the source id at 15). The
 * documents Protocol.md and Links.md still print version 1 headers until they
 * are rewritten, so for now only the payloads match them; the headers here are
 * derived by hand from the layout, never from the encoder.
 *
 * Each example goes both ways: built from a structure and compared to the
 * document, then decoded from those same bytes and checked field by field. An
 * encoder and a decoder that share a misreading of the specification would
 * agree with each other perfectly, and only the document catches that.
 */

namespace {

/** The network used by every example: Docs/Protocol.md section 8. */
constexpr uint8_t kThermometerIsChildOfController = 2;

/**
 * The four nodes, by id. The thermometer's MAC is the one ChildAttached prints
 * (24:6F:28:AA:BB:CC); the others follow the same pattern.
 */
const uint8_t kGatewayBytes[HAP_NODE_ID_LEN] = {0x24, 0x6F, 0x28, 0xFF,
                                                0xFE, 0x00, 0x00, 0x01};
const uint8_t kControllerBytes[HAP_NODE_ID_LEN] = {0x24, 0x6F, 0x28, 0xFF,
                                                   0xFE, 0x00, 0x00, 0x02};
const uint8_t kThermometerBytes[HAP_NODE_ID_LEN] = {0x24, 0x6F, 0x28, 0xFF,
                                                    0xFE, 0xAA, 0xBB, 0xCC};
const uint8_t kLampBytes[HAP_NODE_ID_LEN] = {0x24, 0x6F, 0x28, 0xFF,
                                             0xFE, 0x00, 0x00, 0x04};

const HAPNodeId kGateway = HAPNodeId::fromBytes(kGatewayBytes);
const HAPNodeId kController = HAPNodeId::fromBytes(kControllerBytes);
const HAPNodeId kThermometer = HAPNodeId::fromBytes(kThermometerBytes);
const HAPNodeId kLamp = HAPNodeId::fromBytes(kLampBytes);

constexpr uint16_t kDescriptorRev = 0x9C4A;

/** Wraps a payload in a frame and returns what went on the wire. */
size_t buildFrame(HAPMessage type, uint8_t flags, uint16_t seq,
                  const HAPNodeId& dst, const HAPNodeId& src,
                  const uint8_t* payload, size_t payloadSize, uint8_t* out,
                  size_t capacity) noexcept {
  HAPFrame frame;
  frame.type = static_cast<uint8_t>(type);
  frame.flags = flags;
  frame.seq = seq;
  frame.dst = dst;
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
    "4841 02 01 00 0100"       // magic, version 2, BindAnnounce, flags, seq 1
    "0000000000000000"         // dst at 7: nobody is addressed yet
    "246F28FFFEAABBCC"         // src at 15: the thermometer
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
      buildFrame(HAPMessage::BindAnnounce, HAPFlags::None, 1, HAPNodeId::zero(),
                 kThermometer, payload, writer.size(), buffer, sizeof(buffer));

  CHECK(size == 65);
  CHECK_BYTES(buffer, size, kBindAnnounce);

  // And back: the document's own bytes, parsed.
  uint8_t wire[HAP_MAX_FRAME_SIZE];
  const size_t wireSize = HAPTest::parseHex(kBindAnnounce, wire, sizeof(wire));

  HAPFrame frame;
  CHECK(HAPFrame::decode(wire, wireSize, frame) == HAPFrameError::None);
  CHECK(frame.message() == HAPMessage::BindAnnounce);
  CHECK(frame.isForMaster());  // It has no parent yet, so nobody is named.
  CHECK(frame.src == kThermometer);

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
      buildFrame(HAPMessage::BindAccept, HAPFlags::None, 5, kThermometer,
                 kController, payload, writer.size(), buffer, sizeof(buffer));

  // The document prints the key as an ellipsis, so only its length is fixed
  // here - but the frame's total size is stated, and that is what catches a
  // field added or dropped.
  CHECK(size == 52);
  CHECK_BYTES(buffer, HAP_HEADER_SIZE + 2,
              "4841 02 02 00 0500"
              "246F28FFFEAABBCC"    // dst at 7: the thermometer
              "246F28FFFE000002"    // src at 15: the controller
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
      buildFrame(HAPMessage::BindConfirm, HAPFlags::None, 2, kController,
                 kThermometer, payload, writer.size(), buffer, sizeof(buffer));

  CHECK(size == 25);
  CHECK_BYTES(buffer, size,
              "4841 02 03 00 0200"
              "246F28FFFE000002"    // dst at 7: the parent
              "246F28FFFEAABBCC"    // src at 15: the thermometer
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
      buildFrame(HAPMessage::ChildAttached, HAPFlags::None, 6, HAPNodeId::zero(),
                 kController, payload, writer.size(), buffer, sizeof(buffer));

  CHECK(size == 23 + 13);
  CHECK_BYTES(buffer, size,
              "4841 02 04 00 0600"
              "0000000000000000"    // dst at 7: the master
              "246F28FFFE000002"    // src at 15: the controller, who is telling
              "02"
              "00 01"
              "3C00"
              "4A9C"
              "246F28AABBCC");

  // The gateway learns who spoke from the source and who is new from the MAC
  // inside: the thermometer, whatever the path the notice took.
  HAPFrame atGateway;
  CHECK(HAPFrame::decode(buffer, size, atGateway) == HAPFrameError::None);
  CHECK(atGateway.src == kController);

  HAPReader reader(atGateway.payload, atGateway.payloadSize);
  HAPChildAttached parsed;
  CHECK(parsed.decode(reader));
  CHECK(HAPNodeId::fromMac(parsed.mac) == HAPNodeId::fromMac(thermometerMac));
  CHECK(parsed.childIndex == kThermometerIsChildOfController);
}

// -------------------------------------------------------------------------
// Docs/Protocol.md section 8.2 - a report going up
// -------------------------------------------------------------------------

/**
 * The same bytes at every hop: a relay forwards a frame without rewriting it,
 * and the source is the originator's id wherever it is read.
 */
const char* kReport =
    "4841 02 12 00 0701"       // magic, version 2, Report, flags, seq 0x0107
    "0000000000000000"         // dst at 7: the master
    "246F28FFFEAABBCC"         // src at 15: the thermometer
    "4A9C"                     // payload at 23
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
      buildFrame(HAPMessage::Report, HAPFlags::None, 0x0107, HAPNodeId::zero(),
                 kThermometer, payload, writer.size(), buffer, sizeof(buffer));

  CHECK(size == 42);
  CHECK_BYTES(buffer, size, kReport);
}

void testReportClimbsTwoHops() noexcept {
  uint8_t atThermometer[HAP_MAX_FRAME_SIZE];
  const size_t size = HAPTest::parseHex(kReport, atThermometer, sizeof(atThermometer));
  CHECK(size == 42);

  // The controller: it arrived from the thermometer, is for the master, and is
  // passed on exactly as it came.
  HAPFrame frame;
  CHECK(HAPFrame::decode(atThermometer, size, frame) == HAPFrameError::None);
  CHECK(frame.isForMaster());
  CHECK(frame.src == kThermometer);

  uint8_t atController[HAP_MAX_FRAME_SIZE];
  const size_t controllerSize = frame.encode(atController, sizeof(atController));
  CHECK(controllerSize == size);
  CHECK_BYTES(atController, controllerSize, kReport);

  // The gateway: the same again. Nothing is prepended on the way up any more.
  HAPFrame climbing;
  CHECK(HAPFrame::decode(atController, controllerSize, climbing) ==
        HAPFrameError::None);

  uint8_t atGateway[HAP_MAX_FRAME_SIZE];
  const size_t gatewaySize = climbing.encode(atGateway, sizeof(atGateway));
  CHECK_BYTES(atGateway, gatewaySize, kReport);

  // The invariant the design rests on now: the source is the originator, at
  // every node it passes.
  HAPFrame arrived;
  CHECK(HAPFrame::decode(atGateway, gatewaySize, arrived) == HAPFrameError::None);
  CHECK(arrived.src == kThermometer);

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

void testCommandGoesStraightToTheLamp() noexcept {
  const HAPWriteRequest request(static_cast<uint8_t>(HAPClassId::Lamp), 0, 0,
                                HValue(true));

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  request.encode(writer);
  CHECK(writer.ok());

  // The gateway addresses the lamp by id and sends it there: no hop is eaten by
  // anyone on the way, because nobody is on the way.
  uint8_t atLamp[HAP_MAX_FRAME_SIZE];
  const size_t lampSize = buildFrame(
      HAPMessage::WriteRequest, HAPFlags::AckRequested, 0x0020, kLamp, kGateway,
      payload, writer.size(), atLamp, sizeof(atLamp));

  CHECK(lampSize == 28);
  CHECK_BYTES(atLamp, lampSize,
              "4841 02 15 01 2000"   // magic, version 2, WriteRequest, flags, seq
              "246F28FFFE000004"     // dst at 7: the lamp
              "246F28FFFE000001"     // src at 15: the gateway
              "11 00 00 01 01");     // payload at 23

  // What the lamp makes of it.
  HAPFrame arrived;
  CHECK(HAPFrame::decode(atLamp, lampSize, arrived) == HAPFrameError::None);
  CHECK(arrived.dst == kLamp);
  CHECK(arrived.src == kGateway);
  CHECK(!arrived.isForMaster());
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

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = buildFrame(HAPMessage::WriteResponse, HAPFlags::None,
                                 0x0020,  // Echoes the request: the response IS
                                          // the acknowledgement.
                                 HAPNodeId::zero(), kLamp, payload, writer.size(),
                                 buffer, sizeof(buffer));

  CHECK_BYTES(buffer, size,
              "4841 02 16 00 2000"
              "0000000000000000"     // dst at 7: the master
              "246F28FFFE000004"     // src at 15: the lamp
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
  HAPLinkSpec link;
  link.linkId = 0;

  // Source: the thermometer, by id.
  link.source.node = kThermometer;
  link.source.classId = static_cast<uint8_t>(HAPClassId::Thermometer);
  link.source.instanceId = 0;
  link.source.portId = 0;

  // Destination: the node holding the link, which a zero id says - in port 0
  // of its Regulator, which is Measured.
  link.destination.node = HAPNodeId::zero();
  link.destination.classId = static_cast<uint8_t>(HAPClassId::Regulator);
  link.destination.instanceId = 0;
  link.destination.portId = 0;

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  link.encode(writer);

  CHECK(writer.ok());
  CHECK(writer.size() == HAPLinkSpec::kEncodedSize);
  CHECK(writer.size() == 23);

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size =
      buildFrame(HAPMessage::SetLinkRequest, HAPFlags::AckRequested, 0x0031,
                 kController, kGateway, payload, writer.size(), buffer,
                 sizeof(buffer));

  CHECK(size == 46);
  CHECK_BYTES(buffer, size,
              "4841 02 50 01 3100"
              "246F28FFFE000002"     // dst at 7: the controller, who holds the link
              "246F28FFFE000001"     // src at 15: the gateway
              "00"                   // link id, payload at 23
              "246F28FFFEAABBCC 01 00 00"   // source: thermometer, class, instance, port
              "0000000000000000 30 00 00"); // destination: this node, Regulator in 0

  HAPReader reader(payload, writer.size());
  HAPLinkSpec parsed;
  CHECK(parsed.decode(reader));
  CHECK(parsed.source.node == kThermometer);
  CHECK(parsed.destination.node.isZero());
  CHECK(parsed.destination.classId == static_cast<uint8_t>(HAPClassId::Regulator));
}

/**
 * Docs/Protocol.md section 8.5 - a firmware update pushed to a sleeping node.
 *
 * The whole request fits ONE frame, which is the reason the message exists at
 * all: a parent queues one frame per sleeping child, so anything that needed
 * two would take two report cycles to arrive and could be acted on half
 * delivered.
 */
void testOtaRequest() noexcept {
  HAPOtaRequest request;
  request.flags = HAPOtaFlags::None;
  request.channel = 1;
  request.ssid = "Hatynka";
  request.passphrase = "s3cret";
  request.url = "http://192.168.1.10/HTermo-1.2.0.bin";

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  request.encode(writer);

  CHECK(writer.size() == request.encodedSize());

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size =
      buildFrame(HAPMessage::OtaRequest, HAPFlags::AckRequested, 0x0030,
                 kThermometer, kGateway, payload, writer.size(), buffer,
                 sizeof(buffer));

  CHECK(size == 77);
  CHECK_BYTES(buffer, size,
              "4841 02 60 01 3000"
              "246F28FFFEAABBCC"    // dst at 7: the thermometer
              "246F28FFFE000001"    // src at 15: the gateway
              "00 01"
              "07 48617479 6E6B61"
              "06 733363 726574"
              "24 687474 703A2F2F 3139322E 3136382E 312E3130"
              "2F485465 726D6F2D 312E322E 302E6269 6E");

  HAPReader reader(payload, writer.size());
  HAPOtaRequest parsed;
  CHECK(parsed.decode(reader));
  CHECK(parsed.channel == 1);
  CHECK(std::strcmp(parsed.ssid.c_str(), "Hatynka") == 0);
  CHECK(std::strcmp(parsed.passphrase.c_str(), "s3cret") == 0);
  CHECK(std::strcmp(parsed.url.c_str(), "http://192.168.1.10/HTermo-1.2.0.bin") == 0);
  CHECK(!parsed.isQueryOnly());
  CHECK(parsed.isActionable());
}

/** The answer, which says what is running NOW - not what will be. */
void testOtaResponse() noexcept {
  HAPOtaResponse response;
  response.result = HAPResult::Ok;
  response.version = "1.1.0";

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  response.encode(writer);

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size =
      buildFrame(HAPMessage::OtaResponse, HAPFlags::None, 0x0030,
                 HAPNodeId::zero(), kThermometer, payload, writer.size(), buffer,
                 sizeof(buffer));

  CHECK(size == 30);
  CHECK_BYTES(buffer, size,
              "4841 02 61 00 3000"
              "0000000000000000"    // dst at 7: the master
              "246F28FFFEAABBCC"    // src at 15: the thermometer
              "00 05 312E312E30");

  HAPReader reader(payload, writer.size());
  HAPOtaResponse parsed;
  CHECK(parsed.decode(reader));
  CHECK(parsed.result == HAPResult::Ok);
  CHECK(std::strcmp(parsed.version.c_str(), "1.1.0") == 0);
}

/**
 * A query carries no network and asks for nothing to happen.
 *
 * The half of this message pair that makes it a version QUERY: it is the only
 * way a master can confirm an update afterwards, since the firmware that would
 * have reported the outcome has been replaced by the one it installed.
 */
void testOtaQueryIsNotActionable() noexcept {
  HAPOtaRequest query;
  query.flags = HAPOtaFlags::QueryOnly;

  CHECK(query.isQueryOnly());
  CHECK(!query.isActionable());

  // Nor is a request that names a network but nowhere to fetch from - refused
  // rather than attempted, because the alternative is a battery node sitting
  // with its radio on until its own timeout gives up.
  HAPOtaRequest noUrl;
  noUrl.ssid = "Hatynka";
  CHECK(!noUrl.isActionable());

  HAPOtaRequest noSsid;
  noSsid.url = "http://host/f.bin";
  CHECK(!noSsid.isActionable());
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
  testCommandGoesStraightToTheLamp();
  testWriteResponseClimbsBack();
  testRenameAndItsRevision();
  testSetLink();
  testOtaRequest();
  testOtaResponse();
  testOtaQueryIsNotActionable();
}
