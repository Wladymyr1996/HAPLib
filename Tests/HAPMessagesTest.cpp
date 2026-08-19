#include "HAPTest.hpp"

#include <HAPMessages/HAPMessages.hpp>

#include <cstring>

/**
 * @file HAPMessagesTest.cpp
 * @brief What the documented examples do not reach: the edges of each payload.
 *
 * Wildcards, empty pages, values that are not floats, refusals, and the several
 * ways a short frame can lie about its own contents.
 */

namespace {

/** Encodes a message and hands back a reader over the bytes it produced. */
template <typename Message>
size_t encodeInto(const Message& message, uint8_t* buffer,
                  size_t capacity) noexcept {
  HAPWriter writer(buffer, capacity);
  message.encode(writer);
  return writer.ok() ? writer.size() : 0;
}

// -------------------------------------------------------------------------
// The HValue trap
// -------------------------------------------------------------------------

void testValueEntryKeepsItsType() noexcept {
  // HValue fixes its type at construction and COERCES on assignment, so an
  // entry built by assignment would silently keep the type it started with.
  // This is the bug that would turn every decoded reading into Null, and it is
  // why HAPValueEntry is constructed and never assigned.
  HAPValueEntry fresh(1, 0, 0, HValue(21.5f));
  CHECK(fresh.value.isFloat());
  CHECK(fresh.value.asFloat() == 21.5f);

  HAPValueEntry defaulted;
  CHECK(defaulted.value.isNull());

  defaulted.setValue(HValue(21.5f));
  CHECK(defaulted.value.isFloat());
  CHECK(defaulted.value.asFloat() == 21.5f);

  defaulted.setValue(HValue(true));
  CHECK(defaulted.value.isBool());
  CHECK(defaulted.value.asBool());
}

void testReportCarriesEveryValueType() noexcept {
  HAPReport report;
  report.descriptorRev = 0x1234;
  report.entries.push_back(HAPValueEntry(0x01, 0, 0, HValue(21.5f)));
  report.entries.push_back(HAPValueEntry(0x11, 1, 0, HValue(true)));
  report.entries.push_back(HAPValueEntry(0x06, 2, 0, HValue(42)));
  report.entries.push_back(HAPValueEntry(0x08, 3, 0, HValue("ok")));

  // A sensor that could not read: Null, which is NOT zero and must survive the
  // round trip as itself.
  report.entries.push_back(HAPValueEntry(0x01, 4, 0, HValue()));

  uint8_t buffer[HAP_MAX_PAYLOAD_SIZE];
  const size_t size = encodeInto(report, buffer, sizeof(buffer));
  CHECK(size > 0);

  HAPReader reader(buffer, size);
  HAPReport parsed;
  CHECK(parsed.decode(reader));
  CHECK(parsed.entries.size() == 5);
  CHECK(parsed.entries[0].value.isFloat());
  CHECK(parsed.entries[1].value.isBool());
  CHECK(parsed.entries[2].value.isInt() && parsed.entries[2].value.asInt() == 42);
  CHECK(parsed.entries[3].value.isString());
  CHECK(parsed.entries[4].value.isNull());
  CHECK(parsed.entries[4].instanceId == 4);
}

void testReportRefusesMoreEntriesThanItCanHold() noexcept {
  // The count is stated rather than implied, so an over-long report is refused
  // outright instead of being parsed into a silently short one.
  uint8_t payload[8];
  HAPWriter writer(payload, sizeof(payload));
  writer.u16(0x1234);
  writer.u8(HAP_MAX_REPORT_ENTRIES + 1);

  HAPReader reader(payload, writer.size());
  HAPReport parsed;
  CHECK(!parsed.decode(reader));
}

void testEmptyReportIsLegal() noexcept {
  // A node with nothing new to say still carries its revision, which is how a
  // master notices a descriptor it has not seen.
  HAPReport report;
  report.descriptorRev = 0x9C4A;

  uint8_t buffer[HAP_MAX_PAYLOAD_SIZE];
  const size_t size = encodeInto(report, buffer, sizeof(buffer));
  CHECK(size == 3);

  HAPReader reader(buffer, size);
  HAPReport parsed;
  CHECK(parsed.decode(reader));
  CHECK(parsed.entries.empty());
  CHECK(parsed.descriptorRev == 0x9C4A);
}

// -------------------------------------------------------------------------
// Descriptors and pages
// -------------------------------------------------------------------------

void testDescribeResponsePaging() noexcept {
  HAPDescribeResponse page;
  page.descriptorRev = 0x9C4A;
  page.instanceCount = 8;  // Across all pages...
  page.pageIndex = 1;
  page.pageCount = 2;
  page.nodeName = "Boiler";

  HAPInstanceDescriptor instance;
  instance.classId = static_cast<uint8_t>(HAPClassId::Thermometer);
  instance.instanceId = 5;
  instance.valueType = static_cast<uint8_t>(HAPValueType::Float);
  instance.name = "Return";
  page.instances.push_back(instance);  // ...but only one on this one.

  uint8_t buffer[HAP_MAX_PAYLOAD_SIZE];
  const size_t size = encodeInto(page, buffer, sizeof(buffer));

  HAPReader reader(buffer, size);
  HAPDescribeResponse parsed;
  CHECK(parsed.decode(reader));

  // The node name repeats on every page, so a page can be parsed on its own.
  CHECK(std::strcmp(parsed.nodeName.c_str(), "Boiler") == 0);
  CHECK(parsed.instanceCount == 8);
  CHECK(parsed.instances.size() == 1);
  CHECK(parsed.pageIndex == 1);
  CHECK(std::strcmp(parsed.instances[0].name.c_str(), "Return") == 0);
}

void testDescriptorSizeMatchesWhatItWrites() noexcept {
  // HAPNode will use encodedSize() to decide what fits on a page, so it has to
  // agree with the encoder exactly - one byte out and a page overflows.
  HAPInstanceDescriptor instance;
  instance.classId = 1;
  instance.name = "Температура";

  uint8_t buffer[HAP_MAX_PAYLOAD_SIZE];
  const size_t size = encodeInto(instance, buffer, sizeof(buffer));

  CHECK(size == instance.encodedSize());
  CHECK(size == 4 + 1 + 22);  // Eleven Cyrillic characters, two bytes each.
}

void testDescriptorPageRefusesTooMany() noexcept {
  // More descriptors in one page than this build can hold: refused, rather than
  // quietly dropping the ones past the end.
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));

  writer.u16(0x9C4A);
  writer.u8(HAP_MAX_INSTANCES + 1);
  writer.u8(0);
  writer.u8(1);
  writer.name("Node");

  for (int i = 0; i < HAP_MAX_INSTANCES + 1; ++i) {
    writer.u8(1);
    writer.u8(static_cast<uint8_t>(i));
    writer.u8(0);
    writer.u8(3);
    writer.name("x");
  }

  CHECK(writer.ok());

  HAPReader reader(payload, writer.size());
  HAPDescribeResponse parsed;
  CHECK(!parsed.decode(reader));
}

// -------------------------------------------------------------------------
// Requests and their answers
// -------------------------------------------------------------------------

void testReadRequestWildcards() noexcept {
  HAPReadRequest request;  // Defaults to every port of every instance.

  uint8_t buffer[HAP_MAX_PAYLOAD_SIZE];
  const size_t size = encodeInto(request, buffer, sizeof(buffer));
  CHECK(size == 3);
  CHECK_BYTES(buffer, size, "FF FF FF");

  HAPReadRequest narrowed;
  narrowed.classId = static_cast<uint8_t>(HAPClassId::Thermometer);
  narrowed.instanceId = 1;
  narrowed.portId = 0;

  const size_t narrowSize = encodeInto(narrowed, buffer, sizeof(buffer));
  CHECK_BYTES(buffer, narrowSize, "01 01 00");
}

void testWriteResponseReportsWhatWasTaken() noexcept {
  // A dimmer asked for 1.5 answers 1.0. A master that assumed otherwise would
  // show a value the device is not at.
  const HAPWriteResponse response(HAPResult::Ok, 0x11, 0, 0, HValue(1.0f));

  uint8_t buffer[HAP_MAX_PAYLOAD_SIZE];
  const size_t size = encodeInto(response, buffer, sizeof(buffer));

  HAPReader reader(buffer, size);
  const HAPWriteResponse parsed = HAPWriteResponse::decode(reader);
  CHECK(reader.ok());
  CHECK(parsed.result == HAPResult::Ok);
  CHECK(parsed.value.asFloat() == 1.0f);

  const HAPWriteResponse refused(HAPResult::NotWritable, 0x01, 0, 0, HValue());
  const size_t refusedSize = encodeInto(refused, buffer, sizeof(buffer));

  HAPReader refusedReader(buffer, refusedSize);
  const HAPWriteResponse parsedRefusal = HAPWriteResponse::decode(refusedReader);
  CHECK(refusedReader.ok());
  CHECK(parsedRefusal.result == HAPResult::NotWritable);
  CHECK(parsedRefusal.value.isNull());
}

void testPolicyDeadbandMayBeAbsent() noexcept {
  const HAPSetPolicyRequest withDeadband(0x01, 0, 0, 60, HValue(0.2f));

  uint8_t buffer[HAP_MAX_PAYLOAD_SIZE];
  size_t size = encodeInto(withDeadband, buffer, sizeof(buffer));
  CHECK_BYTES(buffer, size, "01 00 00 3C00 03 CDCC4C3E");

  // Null deadband: report on the interval alone.
  const HAPSetPolicyRequest onInterval(0x03, 0, 0, 300, HValue());
  size = encodeInto(onInterval, buffer, sizeof(buffer));
  CHECK_BYTES(buffer, size, "03 00 00 2C01 00");

  HAPReader reader(buffer, size);
  const HAPSetPolicyRequest parsed = HAPSetPolicyRequest::decode(reader);
  CHECK(reader.ok());
  CHECK(parsed.intervalSec == 300);
  CHECK(parsed.deadband.isNull());
}

void testSetNameCanTargetTheNode() noexcept {
  HAPSetNameRequest request;
  request.target = HAPSetNameRequest::Target::Node;
  request.name = "Hall";

  uint8_t buffer[HAP_MAX_PAYLOAD_SIZE];
  const size_t size = encodeInto(request, buffer, sizeof(buffer));

  // The class and instance bytes are still on the wire, and still ignored - a
  // fixed layout costs two bytes and saves a second parser.
  CHECK_BYTES(buffer, size, "00 00 00 04 48616C6C");

  HAPReader reader(buffer, size);
  HAPSetNameRequest parsed;
  CHECK(parsed.decode(reader));
  CHECK(parsed.target == HAPSetNameRequest::Target::Node);
  CHECK(std::strcmp(parsed.name.c_str(), "Hall") == 0);
}

// -------------------------------------------------------------------------
// Control
// -------------------------------------------------------------------------

void testControlMessages() noexcept {
  uint8_t buffer[HAP_MAX_PAYLOAD_SIZE];

  HAPPing ping;
  CHECK(encodeInto(ping, buffer, sizeof(buffer)) == 0);  // Nothing to write.

  HAPAck ack;
  ack.seq = 0x0107;
  size_t size = encodeInto(ack, buffer, sizeof(buffer));
  CHECK_BYTES(buffer, size, "0701");

  HAPNack nack;
  nack.seq = 0x0107;
  nack.reason = HAPResult::Busy;
  size = encodeInto(nack, buffer, sizeof(buffer));
  CHECK_BYTES(buffer, size, "0701 08");

  // The difference between a timeout and a diagnosis: which hop failed.
  HAPRouteError error;
  error.seq = 0x0020;
  error.failedHop = 4;
  error.reason = HAPResult::ChildUnreachable;
  size = encodeInto(error, buffer, sizeof(buffer));
  CHECK_BYTES(buffer, size, "2000 04 02");

  HAPReader reader(buffer, size);
  HAPRouteError parsed;
  CHECK(parsed.decode(reader));
  CHECK(parsed.failedHop == 4);
  CHECK(parsed.reason == HAPResult::ChildUnreachable);
  CHECK(std::strcmp(HAPResultToString(parsed.reason), "child unreachable") == 0);
}

// -------------------------------------------------------------------------
// Links
// -------------------------------------------------------------------------

void testLinkRefusesAnIllegalPath() noexcept {
  // A port reference whose path contains a zero hop addresses nothing real.
  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));

  writer.u8(0);                 // linkId
  writer.u8(2);                 // source path length
  writer.u8(1);                 // hop 1
  writer.u8(0);                 // hop 2 - illegal
  writer.u8(0);
  writer.u8(0);
  writer.u8(0);
  writer.u8(1);
  writer.u8(0);
  writer.u8(0);

  HAPReader reader(payload, writer.size());
  HAPLinkSpec parsed;
  CHECK(!parsed.decode(reader));
}

void testListLinksPage() noexcept {
  HAPListLinksResponse response;
  response.count = 2;
  response.pageIndex = 0;
  response.pageCount = 1;

  const uint8_t hop[] = {3};

  for (uint8_t i = 0; i < 2; ++i) {
    HAPLinkSpec link;
    link.linkId = i;
    link.source.path = HAPPath::fromBytes(hop, 1);
    link.source.classId = static_cast<uint8_t>(HAPClassId::Thermometer);
    link.source.instanceId = i;
    link.destination.classId = static_cast<uint8_t>(HAPClassId::Regulator);
    link.destination.portId = i;
    response.links.push_back(link);
  }

  uint8_t buffer[HAP_MAX_PAYLOAD_SIZE];
  const size_t size = encodeInto(response, buffer, sizeof(buffer));
  CHECK(size == HAPListLinksResponse::kFixedSize + 2 * HAPLinkSpec::kEncodedSize);

  HAPReader reader(buffer, size);
  HAPListLinksResponse parsed;
  CHECK(parsed.decode(reader));
  CHECK(parsed.links.size() == 2);
  CHECK(parsed.links[1].linkId == 1);
  CHECK(parsed.links[1].source.path.hop(0) == 3);
  CHECK(parsed.links[1].destination.portId == 1);

  // A whole table takes two frames: sixteen records is 304 bytes.
  CHECK(HAP_MAX_LINKS * HAPLinkSpec::kEncodedSize > HAP_MAX_PAYLOAD_SIZE);
}

// -------------------------------------------------------------------------
// Truncation
// -------------------------------------------------------------------------

void testEveryMessageRefusesATruncatedPayload() noexcept {
  // One byte is never enough for any of these, and none of them may report
  // success on it.
  const uint8_t stub[] = {0x01};

  {
    HAPReader reader(stub, sizeof(stub));
    HAPBindAccept message;
    CHECK(!message.decode(reader));
  }
  {
    HAPReader reader(stub, sizeof(stub));
    HAPBindConfirm message;
    CHECK(!message.decode(reader));
  }
  {
    HAPReader reader(stub, sizeof(stub));
    HAPChildAttached message;
    CHECK(!message.decode(reader));
  }
  {
    HAPReader reader(stub, sizeof(stub));
    HAPReport message;
    CHECK(!message.decode(reader));
  }
  {
    HAPReader reader(stub, sizeof(stub));
    HAPSetNameResponse message;
    CHECK(!message.decode(reader));
  }
  {
    HAPReader reader(stub, sizeof(stub));
    HAPRouteError message;
    CHECK(!message.decode(reader));
  }
  {
    HAPReader reader(stub, sizeof(stub));
    HAPLinkSpec message;
    CHECK(!message.decode(reader));
  }
}

}  // namespace

void runMessageTests() noexcept {
  HAPTest::begin("HAPMessages");

  testValueEntryKeepsItsType();
  testReportCarriesEveryValueType();
  testReportRefusesMoreEntriesThanItCanHold();
  testEmptyReportIsLegal();
  testDescribeResponsePaging();
  testDescriptorSizeMatchesWhatItWrites();
  testDescriptorPageRefusesTooMany();
  testReadRequestWildcards();
  testWriteResponseReportsWhatWasTaken();
  testPolicyDeadbandMayBeAbsent();
  testSetNameCanTargetTheNode();
  testControlMessages();
  testLinkRefusesAnIllegalPath();
  testListLinksPage();
  testEveryMessageRefusesATruncatedPayload();
}
