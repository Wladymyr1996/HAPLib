#include "HAPTest.hpp"

#include <HAPITransport/HAPThread/HAPThread.hpp>

#include <cstring>

#ifdef HAP_TEST_THREADS
#include <atomic>
#include <thread>
#endif

/**
 * @file HAPThreadTest.cpp
 * @brief What HAPThread promises, against a fake of the OpenThread calls.
 *
 * The fake records every sendTo() and lets a test inject datagrams as the
 * OpenThread task would. What the real stack does with them - source address
 * selection, ff03::2 on a REED, anycast to self - cannot be shown here and is
 * left to the boards.
 */

namespace {

HAPAddress eid(uint8_t last) noexcept {
  HAPAddress address;
  address.bytes[0] = 0xFD;
  address.bytes[HAP_ADDRESS_LEN - 1] = last;
  return address;
}

HAPNodeId euiOf(uint8_t first) noexcept {
  const uint8_t bytes[HAP_NODE_ID_LEN] = {first, 2, 3, 4, 5, 6, 7, 8};
  return HAPNodeId::fromBytes(bytes);
}

class FakeOpenThread : public HAPIOpenThread {
 public:
  struct Sent {
    HAPAddress to;
    HAPAddress from;
    uint8_t data[HAP_MAX_FRAME_SIZE] = {};
    size_t size = 0;
  };

  bool open(uint16_t port, Handler handler) override {
    ++opens;
    openedPort = port;

    if (failOpen) {
      return false;
    }

    handler_ = handler;
    return true;
  }

  void close() override { ++closes; }
  bool isAttached() override { return attached; }

  bool meshLocalEid(HAPAddress& out) override {
    if (!hasEid) {
      return false;
    }

    out = ownEid;
    return true;
  }

  HAPNodeId factoryId() override { return id; }

  bool serviceAloc(uint32_t enterprise, const uint8_t* data, uint8_t size,
                   HAPAddress& out) override {
    lastEnterprise = enterprise;
    serviceDataOk = size == sizeof(HAPThread::kMasterServiceData) &&
                    std::memcmp(data, HAPThread::kMasterServiceData, size) == 0;

    if (!hasService) {
      return false;
    }

    out = aloc;
    return true;
  }

  bool isLocalAddress(const HAPAddress& address) override {
    return alocIsLocal && address == aloc;
  }

  bool sendTo(const HAPAddress& to, const HAPAddress& from, const uint8_t* data,
              size_t size) override {
    if (failSend) {
      return false;
    }

    Sent& entry = sent[count < kMaxSent ? count : kMaxSent - 1];
    entry.to = to;
    entry.from = from;
    entry.size = size;
    std::memcpy(entry.data, data, size);
    ++count;
    return true;
  }

  /** What the OpenThread task does when a datagram arrives. */
  void inject(const HAPAddress& from, const uint8_t* data, size_t size) {
    handler_(from, data, size);
  }

  static constexpr size_t kMaxSent = 4;

  bool attached = true;
  bool hasEid = true;
  bool hasService = false;
  bool alocIsLocal = false;
  bool failOpen = false;
  bool failSend = false;
  HAPAddress ownEid = eid(1);
  HAPAddress aloc = eid(0xA0);
  HAPNodeId id = euiOf(0x70);

  size_t opens = 0;
  size_t closes = 0;
  uint16_t openedPort = 0;
  uint32_t lastEnterprise = 0;
  bool serviceDataOk = false;
  Sent sent[kMaxSent];
  size_t count = 0;

 private:
  Handler handler_;
};

constexpr uint8_t kPayload[] = {0xA5, 0x01, 0x02};

/** A transport on a fake, collecting what update() delivers. */
struct Rig {
  FakeOpenThread ot;
  HAPThread transport{ot};

  struct Got {
    HAPAddress from;
    size_t size = 0;
    uint8_t first = 0;
  };

  static constexpr size_t kMaxGot = 64;
  Got got[kMaxGot];
  size_t delivered = 0;
  // Set to make the receiver inject one more datagram from inside update().
  const HAPAddress* injectFrom = nullptr;

  Rig() {
    transport.onReceive(
        HAPITransport::Receiver::create<Rig, &Rig::onFrame>(*this));
  }

  bool up() { return transport.begin() && transport.isAttached(); }

  void inject(const HAPAddress& from, uint8_t first, size_t size = 3) {
    uint8_t data[HAP_MAX_FRAME_SIZE + 8] = {};
    data[0] = first;
    ot.inject(from, data, size);
  }

  void onFrame(const HAPAddress& from, const uint8_t* data, size_t size) {
    if (delivered < kMaxGot) {
      got[delivered].from = from;
      got[delivered].size = size;
      got[delivered].first = data[0];
    }
    ++delivered;

    if (injectFrom != nullptr) {
      const HAPAddress source = *injectFrom;
      injectFrom = nullptr;
      inject(source, 0x99);
    }
  }
};

// -------------------------------------------------------------------------

void testNothingWorksBeforeBegin() noexcept {
  Rig rig;

  // H1
  CHECK(!rig.transport.send(eid(2), kPayload, sizeof(kPayload)));
  CHECK(!rig.transport.multicast(kPayload, sizeof(kPayload)));
  CHECK(!rig.transport.isAttached());
  CHECK(rig.transport.localId().isZero());
  CHECK(rig.transport.localAddress().isZero());
  CHECK(rig.ot.count == 0);

  rig.ot.failOpen = true;
  CHECK(!rig.transport.begin());
  CHECK(!rig.transport.isAttached());
  CHECK(rig.transport.localId().isZero());

  rig.ot.failOpen = false;
  CHECK(rig.transport.begin());
  CHECK(rig.transport.begin());
  CHECK(rig.ot.opens == 2);  // the failed attempt, then one that stuck
  CHECK(rig.ot.openedPort == HAP_THREAD_UDP_PORT);
}

void testDestructorClosesOnlyWhatWasOpened() noexcept {
  FakeOpenThread never;
  never.failOpen = true;
  {
    HAPThread transport(never);
    CHECK(!transport.begin());
  }
  CHECK(never.closes == 0);

  FakeOpenThread opened;
  {
    HAPThread transport(opened);
    CHECK(transport.begin());
    CHECK(transport.begin());
  }
  CHECK(opened.closes == 1);

  FakeOpenThread untouched;
  {
    HAPThread transport(untouched);
  }
  CHECK(untouched.closes == 0);
}

void testNotAttachedNeitherSendsNorMulticasts() noexcept {
  Rig rig;
  CHECK(rig.up());

  // H2: the role alone is not enough, and neither is an ML-EID alone.
  rig.ot.attached = false;
  CHECK(!rig.transport.isAttached());
  CHECK(!rig.transport.send(eid(2), kPayload, sizeof(kPayload)));
  CHECK(!rig.transport.multicast(kPayload, sizeof(kPayload)));

  rig.ot.attached = true;
  rig.ot.hasEid = false;
  CHECK(!rig.transport.isAttached());
  CHECK(!rig.transport.send(eid(2), kPayload, sizeof(kPayload)));
  CHECK(!rig.transport.multicast(kPayload, sizeof(kPayload)));
  CHECK(rig.transport.localAddress().isZero());
  CHECK(rig.ot.count == 0);

  rig.ot.hasEid = true;
  CHECK(rig.transport.isAttached());
  CHECK(rig.transport.send(eid(2), kPayload, sizeof(kPayload)));
  CHECK(rig.ot.count == 1);
}

void testBadArgumentsAreLocalFailures() noexcept {
  Rig rig;
  CHECK(rig.up());
  uint8_t big[HAP_MAX_FRAME_SIZE + 1] = {};

  // H3
  CHECK(!rig.transport.send(eid(2), nullptr, 3));
  CHECK(!rig.transport.send(eid(2), kPayload, 0));
  CHECK(!rig.transport.send(eid(2), big, HAP_MAX_FRAME_SIZE + 1));
  CHECK(!rig.transport.multicast(nullptr, 3));
  CHECK(!rig.transport.multicast(kPayload, 0));
  CHECK(!rig.transport.multicast(big, HAP_MAX_FRAME_SIZE + 1));
  CHECK(rig.ot.count == 0);

  CHECK(rig.transport.send(eid(2), big, HAP_MAX_FRAME_SIZE));
  CHECK(rig.ot.count == 1);
  CHECK(rig.ot.sent[0].size == HAP_MAX_FRAME_SIZE);
}

void testUnicastKeepsAddressAndBytes() noexcept {
  Rig rig;
  CHECK(rig.up());
  const uint8_t payload[] = {0x01, 0xFE, 0x00, 0x7F};

  // H4
  CHECK(rig.transport.send(eid(2), payload, sizeof(payload)));
  REQUIRE(rig.ot.count == 1);
  CHECK(rig.ot.sent[0].to == eid(2));
  CHECK(rig.ot.sent[0].from == eid(1));
  CHECK_BYTES(rig.ot.sent[0].data, rig.ot.sent[0].size, "01FE007F");

  rig.ot.failSend = true;
  CHECK(!rig.transport.send(eid(2), payload, sizeof(payload)));
  CHECK(!rig.transport.multicast(payload, sizeof(payload)));
}

void testMasterGoesToTheServiceAloc() noexcept {
  Rig rig;
  CHECK(rig.up());

  // H5: no entry is a silent loss; an entry is a send to its ALOC.
  CHECK(rig.transport.send(HAPAddress::master(), kPayload, sizeof(kPayload)));
  CHECK(rig.ot.count == 0);

  rig.ot.hasService = true;
  CHECK(rig.transport.send(HAPAddress::master(), kPayload, sizeof(kPayload)));
  REQUIRE(rig.ot.count == 1);
  CHECK(rig.ot.sent[0].to == rig.ot.aloc);
  CHECK(rig.ot.sent[0].from == eid(1));
  CHECK(rig.ot.lastEnterprise == HAP_THREAD_ENTERPRISE_NUMBER);
  CHECK(rig.ot.serviceDataOk);

  // H6: a server does not send to itself.
  rig.ot.alocIsLocal = true;
  CHECK(rig.transport.send(HAPAddress::master(), kPayload, sizeof(kPayload)));
  CHECK(rig.ot.count == 1);
}

void testAddressesNobodyHoldsAreLostSilently() noexcept {
  Rig rig;
  CHECK(rig.up());

  // H7
  HAPAddress linkLocalAll;
  linkLocalAll.bytes[0] = 0xFF;
  linkLocalAll.bytes[1] = 0x02;
  linkLocalAll.bytes[HAP_ADDRESS_LEN - 1] = 0x01;

  CHECK(rig.transport.send(HAPAddress::zero(), kPayload, sizeof(kPayload)));
  CHECK(rig.transport.send(HAPAddress::broadcast(), kPayload, sizeof(kPayload)));
  CHECK(rig.transport.send(linkLocalAll, kPayload, sizeof(kPayload)));
  CHECK(rig.ot.count == 0);
}

void testMulticastGoesToAllRoutersWithTheOwnSource() noexcept {
  Rig rig;
  CHECK(rig.up());

  // H8
  CHECK(rig.transport.multicast(kPayload, sizeof(kPayload)));
  REQUIRE(rig.ot.count == 1);
  CHECK_BYTES(rig.ot.sent[0].to.bytes, HAP_ADDRESS_LEN,
              "FF030000000000000000000000000002");
  CHECK(rig.ot.sent[0].from == eid(1));
  CHECK_BYTES(rig.ot.sent[0].data, rig.ot.sent[0].size, "A50102");
}

void testFramesWaitForUpdateAndKeepOrder() noexcept {
  Rig rig;
  CHECK(rig.up());

  // H9
  rig.inject(eid(2), 0x10);
  rig.inject(eid(3), 0x11);
  CHECK(rig.delivered == 0);

  rig.transport.update();
  REQUIRE(rig.delivered == 2);
  CHECK(rig.got[0].from == eid(2));
  CHECK(rig.got[0].first == 0x10);
  CHECK(rig.got[0].size == 3);
  CHECK(rig.got[1].from == eid(3));
  CHECK(rig.got[1].first == 0x11);

  rig.transport.update();
  CHECK(rig.delivered == 2);
}

void testUpdateDeliversOnlyWhatWasQueuedAtEntry() noexcept {
  Rig rig;
  CHECK(rig.up());
  const HAPAddress late = eid(4);

  // The receiver makes a frame arrive mid-round; it waits for the next one.
  rig.inject(eid(2), 0x10);
  rig.injectFrom = &late;
  rig.transport.update();
  CHECK(rig.delivered == 1);

  rig.transport.update();
  REQUIRE(rig.delivered == 2);
  CHECK(rig.got[1].from == eid(4));
  CHECK(rig.got[1].first == 0x99);
}

void testOwnFramesAreDropped() noexcept {
  Rig rig;
  CHECK(rig.up());

  // H10
  rig.inject(eid(1), 0x10);
  rig.inject(eid(2), 0x11);
  rig.transport.update();

  REQUIRE(rig.delivered == 1);
  CHECK(rig.got[0].from == eid(2));
  CHECK(rig.transport.droppedCount() == 0);
}

void testAFullQueueDropsTheNewFrames() noexcept {
  Rig rig;
  CHECK(rig.up());
  constexpr size_t kExtra = 3;

  // H11
  for (size_t i = 0; i < HAP_THREAD_RX_QUEUE + kExtra; ++i) {
    rig.inject(eid(2), static_cast<uint8_t>(i));
  }

  CHECK(rig.transport.droppedCount() == kExtra);
  rig.transport.update();

  REQUIRE(rig.delivered == HAP_THREAD_RX_QUEUE);
  for (size_t i = 0; i < HAP_THREAD_RX_QUEUE; ++i) {
    CHECK(rig.got[i].first == i);
  }

  // The slots are free again.
  rig.inject(eid(2), 0x77);
  rig.transport.update();
  CHECK(rig.delivered == HAP_THREAD_RX_QUEUE + 1);
  CHECK(rig.transport.droppedCount() == kExtra);
}

void testBadReceiveSizesAreDropped() noexcept {
  Rig rig;
  CHECK(rig.up());

  // H12: the oversize datagram arrives with fewer bytes than its real size.
  rig.inject(eid(2), 0x10, 0);
  rig.inject(eid(2), 0x11, HAP_MAX_FRAME_SIZE + 1);
  rig.inject(eid(2), 0x12, 1);
  rig.inject(eid(2), 0x13, HAP_MAX_FRAME_SIZE);
  CHECK(rig.transport.droppedCount() == 2);

  rig.transport.update();
  REQUIRE(rig.delivered == 2);
  CHECK(rig.got[0].size == 1);
  CHECK(rig.got[1].size == HAP_MAX_FRAME_SIZE);
}

void testWithoutAReceiverFramesAreDiscarded() noexcept {
  FakeOpenThread ot;
  HAPThread transport(ot);
  CHECK(transport.begin());
  uint8_t byte = 0x42;

  // H13
  ot.inject(eid(2), &byte, 1);
  transport.update();

  Rig rig;
  CHECK(rig.up());
  rig.inject(eid(2), 0x10);
  rig.transport.update();
  CHECK(rig.delivered == 1);

  // The last receiver wins.
  size_t second = 0;
  struct Counter {
    size_t* n;
    void on(const HAPAddress&, const uint8_t*, size_t) { ++*n; }
  } counter{&second};
  rig.transport.onReceive(
      HAPITransport::Receiver::create<Counter, &Counter::on>(counter));
  rig.inject(eid(2), 0x11);
  rig.transport.update();
  CHECK(rig.delivered == 1);
  CHECK(second == 1);
}

void testIdentity() noexcept {
  Rig rig;

  // H14
  CHECK(rig.transport.localId().isZero());
  CHECK(rig.up());
  CHECK(rig.transport.localId() == euiOf(0x70));
  CHECK(rig.transport.localId().bytes[HAP_NODE_ID_LEN - 1] == 8);
  CHECK(rig.transport.localAddress() == eid(1));

  rig.ot.hasEid = false;
  CHECK(rig.transport.localAddress().isZero());
}

#ifdef HAP_TEST_THREADS
void testAProducerThreadAgainstUpdate() noexcept {
  Rig rig;
  CHECK(rig.up());
  constexpr size_t kFrames = 20000;

  // The producer is the OpenThread task. Whatever is not dropped must arrive
  // intact and in order, and every frame is either delivered or counted.
  size_t next = 0;
  bool ordered = true;
  size_t received = 0;

  struct Order {
    size_t* next;
    bool* ordered;
    size_t* received;
    void on(const HAPAddress&, const uint8_t* data, size_t size) {
      const size_t value = static_cast<size_t>(data[0]) |
                           (static_cast<size_t>(data[1]) << 8) |
                           (static_cast<size_t>(data[2]) << 16);
      // Frames may be dropped, never reordered or damaged.
      if (size != 3 || value < *next) {
        *ordered = false;
      }
      *next = value + 1;
      ++*received;
    }
  } order{&next, &ordered, &received};

  rig.transport.onReceive(
      HAPITransport::Receiver::create<Order, &Order::on>(order));

  std::atomic<bool> done{false};
  std::thread producer([&rig, &done]() {
    for (size_t i = 0; i < kFrames; ++i) {
      const uint8_t data[3] = {static_cast<uint8_t>(i),
                               static_cast<uint8_t>(i >> 8),
                               static_cast<uint8_t>(i >> 16)};
      rig.ot.inject(eid(2), data, sizeof(data));
    }
    done = true;
  });

  while (!done) {
    rig.transport.update();
  }
  producer.join();
  rig.transport.update();

  CHECK(ordered);
  CHECK(received + rig.transport.droppedCount() == kFrames);
}
#endif

}  // namespace

void runThreadTests() noexcept {
  HAPTest::begin("HAPThread");

  testNothingWorksBeforeBegin();
  testDestructorClosesOnlyWhatWasOpened();
  testNotAttachedNeitherSendsNorMulticasts();
  testBadArgumentsAreLocalFailures();
  testUnicastKeepsAddressAndBytes();
  testMasterGoesToTheServiceAloc();
  testAddressesNobodyHoldsAreLostSilently();
  testMulticastGoesToAllRoutersWithTheOwnSource();
  testFramesWaitForUpdateAndKeepOrder();
  testUpdateDeliversOnlyWhatWasQueuedAtEntry();
  testOwnFramesAreDropped();
  testAFullQueueDropsTheNewFrames();
  testBadReceiveSizesAreDropped();
  testWithoutAReceiverFramesAreDiscarded();
  testIdentity();
#ifdef HAP_TEST_THREADS
  testAProducerThreadAgainstUpdate();
#endif
}
