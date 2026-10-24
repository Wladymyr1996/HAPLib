#include <HAPITransport/HAPThread/HAPThread.hpp>

#include <cstring>

namespace {

/** ff03::2 - realm-local all-routers, which on Thread is every rx-on FTD. */
HAPAddress multicastAddress() noexcept {
  HAPAddress address;
  address.bytes[0] = 0xFF;
  address.bytes[1] = 0x03;
  address.bytes[HAP_ADDRESS_LEN - 1] = 0x02;
  return address;
}

}  // namespace

HAPThread::HAPThread(HAPIOpenThread& ot) noexcept : ot_(ot) {}

HAPThread::~HAPThread() {
  if (started_) {
    ot_.close();
  }
}

bool HAPThread::begin() {
  if (started_) {
    return true;
  }

  if (!ot_.open(HAP_THREAD_UDP_PORT,
                HAPIOpenThread::Handler::create<HAPThread, &HAPThread::onFrame>(
                    *this))) {
    return false;
  }

  // Only now: before the application started OpenThread there is no instance
  // to read the id from.
  id_ = ot_.factoryId();
  started_ = true;
  return true;
}

const HAPNodeId& HAPThread::localId() const noexcept {
  return id_;
}

bool HAPThread::isAttached() const noexcept {
  HAPAddress eid;
  return started_ && ot_.isAttached() && ot_.meshLocalEid(eid);
}

HAPAddress HAPThread::localAddress() const noexcept {
  HAPAddress eid;

  if (!started_ || !ot_.meshLocalEid(eid)) {
    return HAPAddress::zero();
  }

  return eid;
}

size_t HAPThread::droppedCount() const noexcept {
  return dropped_.load(std::memory_order_relaxed);
}

bool HAPThread::resolveMaster(HAPAddress& out) const noexcept {
  // The one place that knows how master() is reached, so that a different
  // resolution - the server with the highest term, say - changes only this.
  if (!ot_.serviceAloc(HAP_THREAD_ENTERPRISE_NUMBER, kMasterServiceData,
                       sizeof(kMasterServiceData), out)) {
    return false;
  }

  // A server does not hear itself, as in the loopback; the stack might loop
  // the frame back locally, so it is not sent at all.
  return !ot_.isLocalAddress(out);
}

bool HAPThread::send(const HAPAddress& to, const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0 || size > HAP_MAX_FRAME_SIZE ||
      !isAttached()) {
    return false;
  }

  if (to.isMaster()) {
    HAPAddress aloc;

    // No server, or this node is it: lost silently, which is not a failure.
    return !resolveMaster(aloc) || post(aloc, data, size);
  }

  // Anything that is not a unicast address is an address nobody holds. This
  // also keeps a send() from ever turning into a multicast by accident.
  if (to.isZero() || to.isBroadcast() || to.bytes[0] == 0xFF) {
    return true;
  }

  return post(to, data, size);
}

bool HAPThread::multicast(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0 || size > HAP_MAX_FRAME_SIZE ||
      !isAttached()) {
    return false;
  }

  return post(multicastAddress(), data, size);
}

bool HAPThread::post(const HAPAddress& to, const uint8_t* data,
                     size_t size) noexcept {
  HAPAddress own;

  if (!ot_.meshLocalEid(own)) {
    return false;
  }

  return ot_.sendTo(to, own, data, size);
}

void HAPThread::onReceive(Receiver receiver) {
  receiver_ = receiver;
}

void HAPThread::onFrame(const HAPAddress& from, const uint8_t* data,
                        size_t size) noexcept {
  HAPAddress own;

  // Multicast loopback and any anycast delivery to ourselves.
  if (ot_.meshLocalEid(own) && own == from) {
    return;
  }

  if (data == nullptr || size == 0 || size > HAP_MAX_FRAME_SIZE) {
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  const size_t head = head_.load(std::memory_order_relaxed);

  if (head - tail_.load(std::memory_order_acquire) >= HAP_THREAD_RX_QUEUE) {
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  Slot& slot = slots_[head % HAP_THREAD_RX_QUEUE];
  slot.from = from;
  slot.size = static_cast<uint16_t>(size);
  std::memcpy(slot.data, data, size);

  head_.store(head + 1, std::memory_order_release);
}

void HAPThread::update() noexcept {
  size_t tail = tail_.load(std::memory_order_relaxed);

  // The count at entry, so a receiver that makes more frames arrive cannot
  // keep this loop going.
  const size_t round = head_.load(std::memory_order_acquire) - tail;

  for (size_t i = 0; i < round; ++i, ++tail) {
    const Slot& slot = slots_[tail % HAP_THREAD_RX_QUEUE];

    // The slot is copied out and released before the receiver runs, so the
    // OpenThread task may reuse it while the receiver is still busy.
    const HAPAddress from = slot.from;
    uint8_t data[HAP_MAX_FRAME_SIZE];
    const size_t size = slot.size;
    std::memcpy(data, slot.data, size);

    tail_.store(tail + 1, std::memory_order_release);

    if (receiver_.is_valid()) {
      receiver_(from, data, size);
    }
  }
}
