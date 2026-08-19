#include "HAPTest.hpp"

#include <HAPITransport/HAPLoopback/HAPLoopback.hpp>
#include <HAPLinks/HAPLinks.hpp>
#include <HAPRouter/HAPRouter.hpp>
#include <HSystemUtils/HSystemUtils.hpp>

#include <cstring>

/**
 * @file HAPLinksTest.cpp
 * @brief Wiring one thing to another, and what happens when it stops working.
 *
 * The last of these builds the worked example from Docs/Links.md on the
 * simulated radio: a thermometer one hop below a controller, wired to a
 * regulator on the controller itself. It is the case the whole
 * lowest-common-ancestor rule exists for, and the one that keeps working when
 * the root is unplugged.
 */

namespace {

const uint8_t kThermometer = static_cast<uint8_t>(HAPClassId::Thermometer);
const uint8_t kHygrometer = static_cast<uint8_t>(HAPClassId::Hygrometer);
const uint8_t kRegulator = static_cast<uint8_t>(HAPClassId::Regulator);
const uint8_t kLamp = static_cast<uint8_t>(HAPClassId::Lamp);

HAPPath pathOf(uint8_t a, uint8_t b = 0) noexcept {
  HAPPath path;
  path.append(a);

  if (b != 0) {
    path.append(b);
  }

  return path;
}

HAPPortRef portRef(const HAPPath& path, uint8_t classId, uint8_t instanceId,
                   uint8_t portId) noexcept {
  HAPPortRef reference;
  reference.path = path;
  reference.classId = classId;
  reference.instanceId = instanceId;
  reference.portId = portId;
  return reference;
}

HAPLinkSpec linkOf(uint8_t id, const HAPPortRef& source,
                   const HAPPortRef& destination) noexcept {
  HAPLinkSpec spec;
  spec.linkId = id;
  spec.source = source;
  spec.destination = destination;
  return spec;
}

/** A controller carrying a regulator, as in the worked example. */
void fillController(HAPNode& node) noexcept {
  node.begin(HAPDeviceType::Controller, HAPCaps::CanBeMaster, HAPName("Heating"));
  node.addInstance(HAPClassId::Regulator, HAPName("Loop"));
}

// -------------------------------------------------------------------------
// What a link is allowed to be
// -------------------------------------------------------------------------

void testTheWorkedExampleInstalls() noexcept {
  HAPNode node;
  fillController(node);
  HAPLinks links(node);

  // Docs/Links.md section 8: the controller's own child 1 feeds its own
  // regulator. Both paths downward, because from here everything is below.
  const HAPLinkSpec spec =
      linkOf(0, portRef(pathOf(1), kThermometer, 0, 0),
             portRef(HAPPath(), kRegulator, 0, 0));

  CHECK(links.set(spec) == HAPResult::Ok);
  CHECK(links.size() == 1);
  CHECK(links.find(0) != nullptr);
  CHECK(links.find(1) == nullptr);
}

void testTheKindsHaveToMatch() noexcept {
  HAPNode node;
  fillController(node);
  HAPLinks links(node);

  // Both ends are floats in a plausible range, and only the quantity kind says
  // one is degrees and the other percent. This is the check that idea exists
  // for.
  CHECK(links.set(linkOf(0, portRef(pathOf(1), kHygrometer, 0, 0),
                         portRef(HAPPath(), kRegulator, 0, 0))) ==
        HAPResult::TypeMismatch);

  // A thermometer has nothing to drive.
  CHECK(links.set(linkOf(0, portRef(pathOf(1), kThermometer, 0, 0),
                         portRef(pathOf(2), kThermometer, 0, 0))) ==
        HAPResult::NotWritable);

  // A regulator has two inputs, not three.
  CHECK(links.set(linkOf(0, portRef(pathOf(1), kThermometer, 0, 0),
                         portRef(HAPPath(), kRegulator, 0, 5))) ==
        HAPResult::NoSuchPort);

  CHECK(links.size() == 0);
}

void testLocalEndpointsAreCheckedProperly() noexcept {
  HAPNode node;
  fillController(node);
  HAPLinks links(node);

  // This node has a Regulator at instance 0 and nothing else. A destination
  // here is knowable, so it is known rather than trusted.
  CHECK(links.set(linkOf(0, portRef(pathOf(1), kThermometer, 0, 0),
                         portRef(HAPPath(), kRegulator, 3, 0))) ==
        HAPResult::NoSuchClass);

  // The class-level answer comes first, and is the more useful one: a
  // thermometer cannot drive a lamp on ANY node, so saying "this node has no
  // lamp" would send somebody looking for the wrong problem.
  CHECK(links.set(linkOf(0, portRef(pathOf(1), kThermometer, 0, 0),
                         portRef(HAPPath(), kLamp, 0, 0))) ==
        HAPResult::TypeMismatch);

  // A source somewhere below is a different matter: the class rules still
  // apply, but whether that node HAS the instance is the configuring node's
  // word, because only it holds the descriptors.
  CHECK(links.set(linkOf(0, portRef(pathOf(1, 4), kThermometer, 7, 0),
                         portRef(HAPPath(), kRegulator, 0, 0))) ==
        HAPResult::Ok);
}

void testOneDriverPerInput() noexcept {
  HAPNode node;
  fillController(node);
  HAPLinks links(node);

  CHECK(links.set(linkOf(0, portRef(pathOf(1), kThermometer, 0, 0),
                         portRef(HAPPath(), kRegulator, 0, 0))) ==
        HAPResult::Ok);

  // Two sources into one input is last-writer-wins, which shows up as an input
  // flickering between two values with nothing to say why.
  CHECK(links.set(linkOf(1, portRef(pathOf(2), kThermometer, 0, 0),
                         portRef(HAPPath(), kRegulator, 0, 0))) ==
        HAPResult::InputBusy);

  // The regulator's OTHER input is free, and one output may feed many things.
  CHECK(links.set(linkOf(1, portRef(pathOf(1), kThermometer, 0, 0),
                         portRef(HAPPath(), kRegulator, 0, 1))) ==
        HAPResult::Ok);
  CHECK(links.size() == 2);

  // Replacing a link in its own slot is not a conflict with itself.
  CHECK(links.set(linkOf(0, portRef(pathOf(3), kThermometer, 0, 0),
                         portRef(HAPPath(), kRegulator, 0, 0))) ==
        HAPResult::Ok);
  CHECK(links.size() == 2);
  CHECK(links.find(0)->source.path == pathOf(3));
}

void testTheTableFillsAndEmpties() noexcept {
  HAPNode node;
  node.begin(HAPDeviceType::Controller, HAPCaps::CanBeMaster, HAPName("Big"));

  HAPLinks links(node);

  // Destinations below this node, so each is a different input and none is a
  // conflict.
  for (uint8_t i = 0; i < HAP_MAX_LINKS; ++i) {
    CHECK(links.set(linkOf(i, portRef(pathOf(1), kThermometer, 0, 0),
                           portRef(pathOf(2, static_cast<uint8_t>(i + 1)),
                                   kRegulator, 0, 0))) == HAPResult::Ok);
  }

  CHECK(links.isFull());
  CHECK(links.set(linkOf(HAP_MAX_LINKS, portRef(pathOf(1), kThermometer, 0, 0),
                         portRef(pathOf(3), kRegulator, 0, 0))) ==
        HAPResult::BadRequest);

  CHECK(links.clear(0) == HAPResult::Ok);
  CHECK(links.clear(0) == HAPResult::NoLinkSlot);
  CHECK(links.size() == HAP_MAX_LINKS - 1);

  CHECK(links.clear(HAPClearLinkRequest::kAllLinks) == HAPResult::Ok);
  CHECK(links.size() == 0);
}

void testListingPaginates() noexcept {
  HAPNode node;
  node.begin(HAPDeviceType::Controller, HAPCaps::CanBeMaster, HAPName("Big"));
  HAPLinks links(node);

  CHECK(links.pageCount() == 1);  // Empty, and still one page.

  for (uint8_t i = 0; i < HAP_MAX_LINKS; ++i) {
    links.set(linkOf(i, portRef(pathOf(1), kThermometer, 0, 0),
                     portRef(pathOf(2, static_cast<uint8_t>(i + 1)), kRegulator,
                             0, 0)));
  }

  // Sixteen records at nineteen bytes each is more than a frame holds.
  const uint8_t pages = links.pageCount();
  CHECK(pages > 1);

  size_t seen = 0;

  for (uint8_t page = 0; page < pages; ++page) {
    HAPListLinksResponse response;
    CHECK(links.fillList(response, page));
    CHECK(response.count == HAP_MAX_LINKS);
    CHECK(response.pageCount == pages);
    CHECK(!response.links.empty());

    uint8_t buffer[HAP_MAX_PAYLOAD_SIZE];
    HAPWriter writer(buffer, sizeof(buffer));
    response.encode(writer);
    CHECK(writer.ok());

    seen += response.links.size();
  }

  CHECK(seen == HAP_MAX_LINKS);

  HAPListLinksResponse pastTheEnd;
  CHECK(!links.fillList(pastTheEnd, pages));
}

// -------------------------------------------------------------------------
// Delivery
// -------------------------------------------------------------------------

struct Delivery {
  HAPPortRef destination;
  HValue value;
};

etl::vector<Delivery, 8> gDeliveries;

void record(const HAPPortRef& destination, const HValue& value) noexcept {
  if (!gDeliveries.full()) {
    Delivery delivery;
    delivery.destination = destination;
    HAPAssign(delivery.value, value);
    gDeliveries.push_back(delivery);
  }
}

void testAValueFindsItsWire() noexcept {
  gDeliveries.clear();

  HAPNode node;
  fillController(node);
  HAPLinks links(node);
  links.onDeliver(HAPLinks::DeliveryHook::create<record>());

  links.set(linkOf(0, portRef(pathOf(1), kThermometer, 0, 0),
                   portRef(HAPPath(), kRegulator, 0, 0)));

  HAPReport report;
  report.descriptorRev = 0x9C4A;
  report.entries.push_back(HAPValueEntry(kThermometer, 0, 0, HValue(21.5f)));
  report.entries.push_back(HAPValueEntry(kHygrometer, 1, 0, HValue(44.0f)));

  CHECK(links.deliver(pathOf(1), report) == 1);
  CHECK(gDeliveries.size() == 1);
  CHECK(gDeliveries[0].destination.classId == kRegulator);
  CHECK(gDeliveries[0].destination.path.isEmpty());
  CHECK(gDeliveries[0].value.asFloat() == 21.5f);

  // The same reading from a different node is a different wire, or none.
  gDeliveries.clear();
  CHECK(links.deliver(pathOf(2), report) == 0);
  CHECK(gDeliveries.empty());

  // And a path one hop deeper is not the same path.
  CHECK(links.deliver(pathOf(1, 1), report) == 0);
}

void testOneOutputCanFeedMany() noexcept {
  gDeliveries.clear();

  HAPNode node;
  fillController(node);
  HAPLinks links(node);
  links.onDeliver(HAPLinks::DeliveryHook::create<record>());

  links.set(linkOf(0, portRef(pathOf(1), kThermometer, 0, 0),
                   portRef(HAPPath(), kRegulator, 0, 0)));
  links.set(linkOf(1, portRef(pathOf(1), kThermometer, 0, 0),
                   portRef(pathOf(4), kRegulator, 0, 0)));

  HAPReport report;
  report.entries.push_back(HAPValueEntry(kThermometer, 0, 0, HValue(19.0f)));

  CHECK(links.deliver(pathOf(1), report) == 2);
  CHECK(gDeliveries.size() == 2);

  // One stays here and never touches the radio; the other costs a frame.
  CHECK(gDeliveries[0].destination.path.isEmpty());
  CHECK(gDeliveries[1].destination.path == pathOf(4));
}

void testNullTravelsToo() noexcept {
  gDeliveries.clear();

  HAPNode node;
  fillController(node);
  HAPLinks links(node);
  links.onDeliver(HAPLinks::DeliveryHook::create<record>());

  links.set(linkOf(0, portRef(pathOf(1), kThermometer, 0, 0),
                   portRef(HAPPath(), kRegulator, 0, 0)));

  // The sensor stopped answering. A link that swallowed this would leave the
  // regulator holding a reading that no longer exists.
  HAPReport report;
  report.entries.push_back(HAPValueEntry(kThermometer, 0, 0, HValue()));

  CHECK(links.deliver(pathOf(1), report) == 1);
  CHECK(gDeliveries.size() == 1);
  CHECK(gDeliveries[0].value.isNull());
}

// -------------------------------------------------------------------------
// Staleness
// -------------------------------------------------------------------------

void testAnInputGoesStale() noexcept {
  HAPNode node;
  fillController(node);

  HAPInstance* regulator = node.instanceAt(0);

  // Never fed is stale from the start: a regulator with no temperature must run
  // its failsafe rather than wait forever for a first reading.
  CHECK(regulator->isInputStale(0, 100));
  CHECK(regulator->inputAgeMs(0) == UINT32_MAX);

  CHECK(regulator->write(0, HValue(21.5f)) == HAPResult::Ok);
  CHECK(!regulator->isInputStale(0, 100));
  CHECK(regulator->inputAgeMs(0) < 100);

  HSystemUtils::sleep(120);

  // This one timer catches every way a link can stop working: a dead source, a
  // broken hop, an ancestor that lost power, or a wire nobody ever installed.
  CHECK(regulator->isInputStale(0, 100));

  CHECK(regulator->write(0, HValue(21.6f)) == HAPResult::Ok);
  CHECK(!regulator->isInputStale(0, 100));

  // A value going Null is stale immediately, whatever the timeout - there is no
  // reading to be recent.
  CHECK(regulator->write(0, HValue()) == HAPResult::Ok);
  CHECK(regulator->isInputStale(0, 100000));

  // An output is not an input, and cannot answer the question.
  CHECK(!regulator->isInputStale(9, 1));
}

// -------------------------------------------------------------------------
// The worked example, on the simulated radio
// -------------------------------------------------------------------------

void testTheLoopSurvivesTheRootGoingAway() noexcept {
  // Docs/Links.md section 8:
  //   Gateway --1-- Heat controller --1-- Thermometer
  // The link's lowest common ancestor is the CONTROLLER, so that is where it
  // lives and the gateway steps out of the way.
  HAPLoopbackBus bus;

  HAPMac gatewayMac;
  gatewayMac.bytes[5] = 1;
  HAPMac controllerMac;
  controllerMac.bytes[5] = 2;
  HAPMac thermometerMac;
  thermometerMac.bytes[5] = 3;

  HAPLoopback gatewayRadio(gatewayMac, bus);
  HAPLoopback controllerRadio(controllerMac, bus);
  HAPLoopback thermometerRadio(thermometerMac, bus);

  gatewayRadio.begin(1);
  controllerRadio.begin(1);
  thermometerRadio.begin(1);

  gatewayRadio.addPeer(controllerMac, nullptr);
  controllerRadio.addPeer(gatewayMac, nullptr);
  controllerRadio.addPeer(thermometerMac, nullptr);
  thermometerRadio.addPeer(controllerMac, nullptr);

  HAPNode controllerNode;
  fillController(controllerNode);

  HAPRouter controllerRouter;
  controllerRouter.setParent(gatewayMac, 1);

  HAPChild child;
  child.index = 1;
  child.mac = thermometerMac;
  child.capabilities = HAPCaps::BatteryPowered;
  CHECK(controllerRouter.addChild(child));

  HAPLinks links(controllerNode);

  // The gateway installed this and then stopped being involved.
  CHECK(links.set(linkOf(0, portRef(pathOf(1), kThermometer, 0, 0),
                         portRef(HAPPath(), kRegulator, 0, 0))) ==
        HAPResult::Ok);

  static HAPNode* target = &controllerNode;
  static size_t applied = 0;
  applied = 0;

  links.onDeliver(HAPLinks::DeliveryHook::create(
      [](const HAPPortRef& destination, const HValue& value) {
        if (destination.path.isEmpty()) {
          HAPInstance* instance =
              target->instance(destination.classId, destination.instanceId);
          if (instance != nullptr &&
              instance->write(destination.portId, value) == HAPResult::Ok) {
            ++applied;
          }
        }
      }));

  // The thermometer reports. Building the frame the way it really would.
  HAPReport report;
  report.descriptorRev = 0x9C4A;
  report.entries.push_back(HAPValueEntry(kThermometer, 0, 0, HValue(21.5f)));

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  report.encode(writer);

  HAPFrame frame;
  frame.type = static_cast<uint8_t>(HAPMessage::Report);
  frame.set(HAPFlags::Upstream);
  frame.seq = 0x0107;
  frame.payload = payload;
  frame.payloadSize = static_cast<uint8_t>(writer.size());

  uint8_t wire[HAP_MAX_FRAME_SIZE];
  const size_t size = frame.encode(wire, sizeof(wire));
  CHECK(thermometerRadio.send(controllerMac, wire, size));
  bus.pump();

  // At the controller: decode, route, and the source path becomes the downward
  // path from here - which is exactly what the link stored.
  HAPFrame arrived;
  CHECK(HAPFrame::decode(wire, size, arrived) == HAPFrameError::None);
  const HAPRoute route = controllerRouter.route(arrived, thermometerMac);
  CHECK(route.action == HAPRouteAction::ToParent);
  CHECK(arrived.src == pathOf(1));

  HAPReader reader(arrived.payload, arrived.payloadSize);
  HAPReport parsed;
  CHECK(parsed.decode(reader));

  CHECK(links.deliver(arrived.src, parsed) == 1);
  CHECK(applied == 1);
  CHECK(controllerNode.instanceAt(0)->input(0).asFloat() == 21.5f);

  // A tap, not a diversion: the report still climbs to the gateway.
  uint8_t upward[HAP_MAX_FRAME_SIZE];
  const size_t upwardSize = arrived.encode(upward, sizeof(upward));
  CHECK(controllerRadio.send(gatewayMac, upward, upwardSize));
  bus.pump();
  CHECK(gatewayRadio.receivedCount() == 1);

  // Now the gateway loses power, and the loop carries on: the link lives here,
  // its source is one hop away, and nothing above was ever involved.
  bus.setReachable(gatewayMac, controllerMac, false);

  HAPReport later;
  later.entries.push_back(HAPValueEntry(kThermometer, 0, 0, HValue(22.5f)));
  CHECK(links.deliver(pathOf(1), later) == 1);
  CHECK(applied == 2);
  CHECK(controllerNode.instanceAt(0)->input(0).asFloat() == 22.5f);
  CHECK(!controllerNode.instanceAt(0)->isInputStale(0, 1000));
}

}  // namespace

void runLinksTests() noexcept {
  HAPTest::begin("HAPLinks");

  testTheWorkedExampleInstalls();
  testTheKindsHaveToMatch();
  testLocalEndpointsAreCheckedProperly();
  testOneDriverPerInput();
  testTheTableFillsAndEmpties();
  testListingPaginates();
  testAValueFindsItsWire();
  testOneOutputCanFeedMany();
  testNullTravelsToo();
  testAnInputGoesStale();
  testTheLoopSurvivesTheRootGoingAway();
}
