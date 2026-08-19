#include "HAPTest.hpp"

#include <HAPFrame/HAPFrame.hpp>
#include <HAPListenWindow/HAPListenWindow.hpp>
#include <HAPQueue/HAPQueue.hpp>
#include <HAPReporter/HAPReporter.hpp>
#include <HSystemUtils/HSystemUtils.hpp>

#include <cstring>

/**
 * @file HAPReportTest.cpp
 * @brief When a node speaks, and what happens to a command it cannot receive.
 *
 * The three pieces a battery node's life is made of: a policy that decides
 * whether a reading is worth a transmission, a parent holding a frame for a
 * child that is not there, and the short window in which the two meet.
 */

namespace {

/** A thermometer node like this project's, with a battery instance too. */
void fillSensor(HAPNode& node) noexcept {
  node.begin(HAPDeviceType::Sensor, HAPCaps::BatteryPowered, HAPName("Bedroom"));
  node.addInstance(HAPClassId::Thermometer, HAPName("Temp"));
  node.addInstance(HAPClassId::Hygrometer, HAPName("Hum"));
}

// -------------------------------------------------------------------------
// Policies
// -------------------------------------------------------------------------

void testAPolicyPerOutPort() noexcept {
  HAPNode node;
  fillSensor(node);
  node.addInstance(HAPClassId::Lamp, HAPName("Bulb"));

  HAPReporter reporter(node);
  reporter.begin(60, HValue(0.2f));

  // Three instances, one out port each - a lamp's IN port is not something this
  // node reports, so it gets no policy of its own.
  CHECK(reporter.policyCount() == 3);
}

void testTheFirstReportGoesOutAtOnce() noexcept {
  HAPNode node;
  fillSensor(node);

  HAPReporter reporter(node);
  reporter.begin(60, HValue(0.2f));

  // Nothing has ever been said, so there is nothing to wait for: a node that
  // has just bound should not be silent for a minute.
  CHECK(reporter.isDue());
  CHECK(reporter.nextDueInMs() == 0);

  reporter.markReported();
  CHECK(!reporter.isDue());
  CHECK(reporter.nextDueInMs() > 0);
}

void testTheDeadbandSuppressesNoise() noexcept {
  HAPNode node;
  fillSensor(node);
  node.instanceAt(0)->publish(0, HValue(21.5f));

  HAPReporter reporter(node);
  reporter.begin(3600, HValue(0.2f));  // An hour, so only the deadband can fire.
  reporter.markReported();

  CHECK(!reporter.isDue());

  // Sensor noise, well inside the accuracy of the part. Not worth a wake.
  node.instanceAt(0)->publish(0, HValue(21.55f));
  CHECK(!reporter.isDue());

  node.instanceAt(0)->publish(0, HValue(21.69f));
  CHECK(!reporter.isDue());

  // A real change.
  node.instanceAt(0)->publish(0, HValue(21.8f));
  CHECK(reporter.isDue());

  // And once reported, quiet again from the new value.
  reporter.markReported();
  CHECK(!reporter.isDue());
  node.instanceAt(0)->publish(0, HValue(21.9f));
  CHECK(!reporter.isDue());
}

void testASensorFallingSilentIsAlwaysNews() noexcept {
  HAPNode node;
  fillSensor(node);
  node.instanceAt(0)->publish(0, HValue(21.5f));

  HAPReporter reporter(node);
  reporter.begin(3600, HValue(100.0f));  // A deadband nothing could cross.
  reporter.markReported();
  CHECK(!reporter.isDue());

  // The conversion failed. Null is not a small change from 21.5 - it is the
  // absence of a reading, and no deadband may hide it.
  node.instanceAt(0)->publish(0, HValue());
  CHECK(reporter.isDue());

  reporter.markReported();
  CHECK(!reporter.isDue());

  // And coming back is news too.
  node.instanceAt(0)->publish(0, HValue(21.5f));
  CHECK(reporter.isDue());
}

void testAnyChangeCountsWithoutADeadband() noexcept {
  HAPNode node;
  node.begin(HAPDeviceType::Controller, HAPCaps::CanBeMaster, HAPName("Hall"));
  node.addInstance(HAPClassId::Switch, HAPName("Wall"));

  HAPReporter reporter(node);
  reporter.begin(0, HValue());  // On change only, and nothing to subtract.
  reporter.markReported();

  CHECK(!reporter.isDue());

  // A switch has no deadband and no interval: the change IS the event.
  node.instanceAt(0)->publish(0, HValue(true));
  CHECK(reporter.isDue());

  reporter.markReported();
  CHECK(!reporter.isDue());

  node.instanceAt(0)->publish(0, HValue(false));
  CHECK(reporter.isDue());

  // With no interval there is no deadline, so nothing counts down.
  reporter.markReported();
  CHECK(reporter.nextDueInMs() == UINT32_MAX);
}

void testTheIntervalFiresOnItsOwn() noexcept {
  HAPNode node;
  fillSensor(node);
  node.instanceAt(0)->publish(0, HValue(21.5f));

  HAPReporter reporter(node);
  reporter.begin(1, HValue(100.0f));  // One second, unreachable deadband.
  reporter.markReported();

  CHECK(!reporter.isDue());
  CHECK(reporter.nextDueInMs() > 0);
  CHECK(reporter.nextDueInMs() <= 1000);

  HSystemUtils::sleep(1010);

  // Nothing moved, and it reports anyway: a master has to be able to tell an
  // unchanging reading from a node that has stopped.
  CHECK(reporter.isDue());
  CHECK(reporter.nextDueInMs() == 0);
}

void testAReportCarriesEverythingOnceItIsDue() noexcept {
  HAPNode node;
  fillSensor(node);
  node.instanceAt(0)->publish(0, HValue(21.5f));
  node.instanceAt(1)->publish(0, HValue(44.0f));

  HAPReporter reporter(node);
  reporter.begin(60, HValue(0.2f));

  HAPReport report;
  reporter.fillReport(report);

  // The deadband decided WHETHER to speak. Having decided, a master gets a
  // consistent snapshot rather than whichever single port happened to move.
  CHECK(report.entries.size() == 2);
  CHECK(report.entries[0].value.asFloat() == 21.5f);
  CHECK(report.entries[1].value.asFloat() == 44.0f);
  CHECK(report.descriptorRev == node.descriptorRev());
}

void testAPolicyMayBeClamped() noexcept {
  HAPNode node;
  fillSensor(node);

  HAPReporter reporter(node);
  reporter.begin(60, HValue(0.2f));

  // This node wakes once a minute; it cannot report every second whatever it is
  // asked, and saying so is worth more than silently ignoring the request.
  reporter.setMinimumIntervalSec(60);

  const HAPSetPolicyRequest tooFast(
      static_cast<uint8_t>(HAPClassId::Thermometer), 0, 0, 1, HValue(0.1f));
  const HAPSetPolicyResponse answer = reporter.setPolicy(tooFast);

  CHECK(answer.result == HAPResult::Ok);
  CHECK(answer.intervalSec == 60);
  CHECK(answer.deadband.asFloat() == 0.1f);

  // On-change-only is still allowed: it asks for nothing the battery cannot
  // afford, because a change only happens when the node is awake anyway.
  const HAPSetPolicyRequest onChange(
      static_cast<uint8_t>(HAPClassId::Thermometer), 0, 0, 0, HValue());
  CHECK(reporter.setPolicy(onChange).intervalSec == 0);

  const HAPSetPolicyRequest missing(static_cast<uint8_t>(HAPClassId::Lamp), 0, 0,
                                    60, HValue());
  CHECK(reporter.setPolicy(missing).result == HAPResult::NoSuchPort);
}

void testANewPolicyTakesEffect() noexcept {
  HAPNode node;
  fillSensor(node);
  node.instanceAt(0)->publish(0, HValue(21.5f));
  node.instanceAt(1)->publish(0, HValue(44.0f));

  HAPReporter reporter(node);
  reporter.begin(3600, HValue(5.0f));
  reporter.markReported();

  node.instanceAt(0)->publish(0, HValue(23.0f));
  CHECK(!reporter.isDue());  // Inside a 5-degree deadband.

  const HAPSetPolicyRequest tighter(
      static_cast<uint8_t>(HAPClassId::Thermometer), 0, 0, 3600, HValue(0.5f));
  CHECK(reporter.setPolicy(tighter).result == HAPResult::Ok);

  // The same unreported change now clears the new deadband.
  CHECK(reporter.isDue());
}

// -------------------------------------------------------------------------
// The queue
// -------------------------------------------------------------------------

size_t buildFrame(uint8_t* out, size_t capacity, uint16_t seq) noexcept {
  HAPFrame frame;
  frame.type = static_cast<uint8_t>(HAPMessage::SetNameRequest);
  frame.seq = seq;
  return frame.encode(out, capacity);
}

void testTheQueueHoldsOnePerChild() noexcept {
  HAPQueue queue;

  uint8_t first[HAP_MAX_FRAME_SIZE];
  const size_t firstSize = buildFrame(first, sizeof(first), 0x0001);

  CHECK(!queue.has(2));
  CHECK(queue.push(2, first, firstSize));
  CHECK(queue.has(2));
  CHECK(queue.size() == 1);

  // A second frame for the same child is refused, and the sender is owed
  // Nack(Busy). A deeper queue would reorder commands nobody is watching.
  uint8_t second[HAP_MAX_FRAME_SIZE];
  const size_t secondSize = buildFrame(second, sizeof(second), 0x0002);
  CHECK(!queue.push(2, second, secondSize));

  // What is waiting is the FIRST one - the refusal did not replace it.
  size_t peeked = 0;
  const uint8_t* frame = queue.peek(2, peeked);
  CHECK(frame != nullptr);
  CHECK(peeked == firstSize);
  CHECK(std::memcmp(frame, first, firstSize) == 0);

  // Another child is unaffected.
  CHECK(queue.push(3, second, secondSize));
  CHECK(queue.size() == 2);

  queue.pop(2);
  CHECK(!queue.has(2));
  CHECK(queue.has(3));

  // And the slot is free again once it has been delivered.
  CHECK(queue.push(2, second, secondSize));
}

void testTheQueueRefusesNonsense() noexcept {
  HAPQueue queue;

  uint8_t frame[HAP_MAX_FRAME_SIZE];
  const size_t size = buildFrame(frame, sizeof(frame), 1);

  CHECK(!queue.push(0, frame, size));                  // 0 is not a child index
  CHECK(!queue.push(HAP_MAX_CHILDREN + 1, frame, size));
  CHECK(!queue.push(1, nullptr, size));
  CHECK(!queue.push(1, frame, 0));
  CHECK(!queue.push(1, frame, HAP_MAX_FRAME_SIZE + 1));
  CHECK(queue.size() == 0);

  size_t peeked = 1;
  CHECK(queue.peek(1, peeked) == nullptr);
  CHECK(peeked == 0);

  queue.pop(1);  // Harmless.
}

void testAFrameCanBeGivenUpOn() noexcept {
  HAPQueue queue;

  uint8_t frame[HAP_MAX_FRAME_SIZE];
  const size_t size = buildFrame(frame, sizeof(frame), 1);
  CHECK(queue.push(1, frame, size));

  HSystemUtils::sleep(30);

  CHECK(queue.ageMs(1) >= 30);
  CHECK(queue.expire(10000) == 0);  // Not old enough yet.
  CHECK(queue.has(1));

  // How long to hold a command for a node that has stopped waking is the
  // application's decision, which is why the age is asked for rather than
  // assumed.
  CHECK(queue.expire(20) == 1);
  CHECK(!queue.has(1));
  CHECK(queue.ageMs(1) == 0);
}

void testTheQueueFillsUp() noexcept {
  HAPQueue queue;

  uint8_t frame[HAP_MAX_FRAME_SIZE];
  const size_t size = buildFrame(frame, sizeof(frame), 1);

  for (uint8_t i = 1; i <= HAP_MAX_CHILDREN; ++i) {
    CHECK(queue.push(i, frame, size));
  }

  CHECK(queue.size() == HAP_MAX_CHILDREN);

  queue.clear();
  CHECK(queue.size() == 0);
}

// -------------------------------------------------------------------------
// The listen window
// -------------------------------------------------------------------------

void testTheWindowOpensAndCloses() noexcept {
  HAPListenWindow window;

  // Asleep by default: a node that has not spoken is not listening.
  CHECK(!window.isOpen());
  CHECK(window.maySleep());
  CHECK(window.remainingMs() == 0);

  window.open();
  CHECK(window.isOpen());
  CHECK(!window.maySleep());
  CHECK(window.remainingMs() > 0);
  CHECK(window.remainingMs() <= HAP_LISTEN_WINDOW_MS);

  HSystemUtils::sleep(HAP_LISTEN_WINDOW_MS + 20);

  // It closed on its own. Every millisecond of it was paid for at receive
  // current, so it does not stay open hoping.
  CHECK(!window.isOpen());
  CHECK(window.maySleep());
}

void testAFrameInTheWindowExtendsIt() noexcept {
  HAPListenWindow window;
  window.open();
  CHECK(window.extensions() == 0);

  HSystemUtils::sleep(HAP_LISTEN_WINDOW_MS / 2);

  // A queued command arrived, which means the parent is mid-conversation and
  // an answer is about to be wanted. Closing now would cost a whole report
  // interval; extending costs a few milliseconds.
  window.extend();
  CHECK(window.extensions() == 1);
  CHECK(window.isOpen());

  HSystemUtils::sleep((HAP_LISTEN_WINDOW_MS / 2) + 10);
  CHECK(window.isOpen());  // Still open, because it started again.

  HSystemUtils::sleep(HAP_LISTEN_WINDOW_MS);
  CHECK(!window.isOpen());

  // Extending a closed window does nothing: traffic outside it is a mains
  // node's ordinary business, not the tail of an exchange.
  window.extend();
  CHECK(!window.isOpen());
  CHECK(window.extensions() == 1);
}

void testTheWindowCanBeClosedEarly() noexcept {
  HAPListenWindow window;
  window.open();
  CHECK(!window.maySleep());

  // The exchange finished, and the remaining milliseconds are worth more in the
  // battery than in the radio.
  window.close();
  CHECK(window.maySleep());
  CHECK(window.remainingMs() == 0);
}

// -------------------------------------------------------------------------
// Together
// -------------------------------------------------------------------------

void testAWakeCycle() noexcept {
  // What one minute of a battery node's life looks like, without the radio.
  HAPNode node;
  fillSensor(node);

  HAPReporter reporter(node);
  reporter.begin(60, HValue(0.2f));
  reporter.setMinimumIntervalSec(60);

  HAPListenWindow window;
  HAPQueue parentQueue;  // Standing in for the parent's.

  // The parent has a rename waiting for a child that is not there.
  uint8_t queued[HAP_MAX_FRAME_SIZE];
  const size_t queuedSize = buildFrame(queued, sizeof(queued), 0x0044);
  CHECK(parentQueue.push(1, queued, queuedSize));

  // The node wakes and measures.
  node.instanceAt(0)->publish(0, HValue(21.5f));
  node.instanceAt(1)->publish(0, HValue(44.0f));
  CHECK(reporter.isDue());

  HAPReport report;
  reporter.fillReport(report);
  CHECK(report.entries.size() == 2);

  // It reports, and opens its window.
  reporter.markReported();
  window.open();
  CHECK(!window.maySleep());

  // The parent answers with what it was holding, inside the window.
  size_t pending = 0;
  CHECK(parentQueue.peek(1, pending) != nullptr);
  parentQueue.pop(1);
  window.extend();

  CHECK(parentQueue.size() == 0);
  CHECK(window.isOpen());

  // The node answers that, and goes back to sleep.
  window.close();
  CHECK(window.maySleep());
  CHECK(!reporter.isDue());

  // Next wake is a minute away, not sooner.
  CHECK(reporter.nextDueInMs() > 50000);
}

}  // namespace

void runReportTests() noexcept {
  HAPTest::begin("HAPReporter, HAPQueue, HAPListenWindow");

  testAPolicyPerOutPort();
  testTheFirstReportGoesOutAtOnce();
  testTheDeadbandSuppressesNoise();
  testASensorFallingSilentIsAlwaysNews();
  testAnyChangeCountsWithoutADeadband();
  testTheIntervalFiresOnItsOwn();
  testAReportCarriesEverythingOnceItIsDue();
  testAPolicyMayBeClamped();
  testANewPolicyTakesEffect();

  testTheQueueHoldsOnePerChild();
  testTheQueueRefusesNonsense();
  testAFrameCanBeGivenUpOn();
  testTheQueueFillsUp();

  testTheWindowOpensAndCloses();
  testAFrameInTheWindowExtendsIt();
  testTheWindowCanBeClosedEarly();

  testAWakeCycle();
}
