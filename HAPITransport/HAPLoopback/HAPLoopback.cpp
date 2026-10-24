#include <HAPITransport/HAPLoopback/HAPLoopback.hpp>

#include <cstring>

// ---------------------------------------------------------------------------
// HAPLoopbackBus
// ---------------------------------------------------------------------------

void HAPLoopbackBus::attach(HAPLoopback& node) noexcept {
  if (find(node.localAddress()) != nullptr || nodes_.full()) {
    return;
  }

  nodes_.push_back(&node);
}

void HAPLoopbackBus::detach(const HAPLoopback& node) noexcept {
  for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
    if (*it == &node) {
      nodes_.erase(it);
      return;
    }
  }
}

HAPLoopback* HAPLoopbackBus::find(const HAPAddress& address) const noexcept {
  for (HAPLoopback* node : nodes_) {
    if (node->localAddress() == address) {
      return node;
    }
  }

  return nullptr;
}

bool HAPLoopbackBus::enqueue(const Packet& packet) noexcept {
  if (queue_.full()) {
    return false;
  }

  queue_.push_back(packet);
  return true;
}

void HAPLoopbackBus::setReachable(const HAPNodeId& a, const HAPNodeId& b,
                                  bool reachable) noexcept {
  for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
    const bool sameOrder = it->a == a && it->b == b;
    const bool reverseOrder = it->a == b && it->b == a;

    if (sameOrder || reverseOrder) {
      if (reachable) {
        blocks_.erase(it);
      }
      return;
    }
  }

  if (!reachable && !blocks_.full()) {
    blocks_.push_back(Block{a, b});
  }
}

bool HAPLoopbackBus::isReachable(const HAPNodeId& a,
                                 const HAPNodeId& b) const noexcept {
  for (const Block& block : blocks_) {
    if ((block.a == a && block.b == b) || (block.a == b && block.b == a)) {
      return false;
    }
  }

  return true;
}

void HAPLoopbackBus::dropNext(size_t count) noexcept {
  dropCountdown_ = count;
}

void HAPLoopbackBus::deliver(const Packet& packet) noexcept {
  HAPLoopback* sender = find(packet.from);

  if (dropCountdown_ > 0) {
    --dropCountdown_;
    ++dropped_;

    // The radio still transmitted it. A frame lost in the air is exactly the
    // case where a send reports success and nothing arrives, which is what the
    // protocol above has to be built to survive.
    if (sender != nullptr) {
      sender->sent(packet.to, packet.to.isBroadcast());
    }
    return;
  }

  size_t received = 0;

  for (HAPLoopback* node : nodes_) {
    if (node->localAddress() == packet.from) {
      continue;  // A radio does not hear itself.
    }

    const bool addressed =
        packet.to.isBroadcast() || node->localAddress() == packet.to;
    if (!addressed) {
      continue;
    }

    // A pair of nodes that cannot hear each other receives nothing, and it
    // looks exactly like a protocol bug from the far end.
    if (!isReachable(packet.from.nodeId(), node->localId())) {
      continue;
    }

    node->receive(packet);
    ++received;
    ++delivered_;
  }

  // Fired AFTER the frame is on the air but modelled as the radio's own
  // notification: a broadcast is never acknowledged by anyone, and a unicast
  // counts as delivered when somebody took it.
  if (sender != nullptr) {
    sender->sent(packet.to, packet.to.isBroadcast() || received > 0);
  }
}

size_t HAPLoopbackBus::pump() noexcept {
  // Only what was queued when this was called. Anything the deliveries below
  // enqueue belongs to the next round - which is what makes a two-hop journey
  // observable one hop at a time, and what stops a forwarding loop from
  // recursing until the stack gives out.
  const size_t round = queue_.size();

  for (size_t i = 0; i < round; ++i) {
    deliver(queue_[i]);
  }

  queue_.erase(queue_.begin(), queue_.begin() + static_cast<ptrdiff_t>(round));
  return round;
}

size_t HAPLoopbackBus::pumpAll(size_t maxRounds) noexcept {
  size_t rounds = 0;

  while (!queue_.empty() && rounds < maxRounds) {
    pump();
    ++rounds;
  }

  return rounds;
}

size_t HAPLoopbackBus::pending() const noexcept {
  return queue_.size();
}

size_t HAPLoopbackBus::deliveredCount() const noexcept {
  return delivered_;
}

size_t HAPLoopbackBus::droppedCount() const noexcept {
  return dropped_;
}

void HAPLoopbackBus::reset() noexcept {
  queue_.clear();
  blocks_.clear();
  dropCountdown_ = 0;
  delivered_ = 0;
  dropped_ = 0;
}

// ---------------------------------------------------------------------------
// HAPLoopback
// ---------------------------------------------------------------------------

HAPLoopback::HAPLoopback(const HAPNodeId& id, HAPLoopbackBus& bus) noexcept
    : id_(id), address_(HAPAddress::fromNodeId(id)), bus_(bus) {
  bus_.attach(*this);
}

HAPLoopback::~HAPLoopback() {
  bus_.detach(*this);
}

bool HAPLoopback::begin() {
  started_ = true;
  return true;
}

const HAPNodeId& HAPLoopback::localId() const noexcept {
  return id_;
}

const HAPAddress& HAPLoopback::localAddress() const noexcept {
  return address_;
}

bool HAPLoopback::send(const HAPAddress& to, const uint8_t* data, size_t size) {
  if (!started_ || data == nullptr || size == 0 || size > HAP_MAX_FRAME_SIZE) {
    return false;
  }

  HAPLoopbackBus::Packet packet;
  packet.from = address_;
  packet.to = to;
  std::memcpy(packet.data, data, size);
  packet.size = size;

  if (!bus_.enqueue(packet)) {
    return false;
  }

  ++sent_;
  return true;
}

bool HAPLoopback::broadcast(const uint8_t* data, size_t size) {
  if (!started_ || data == nullptr || size == 0 || size > HAP_MAX_FRAME_SIZE) {
    return false;
  }

  HAPLoopbackBus::Packet packet;
  packet.from = address_;
  packet.to = HAPAddress::broadcast();
  std::memcpy(packet.data, data, size);
  packet.size = size;

  if (!bus_.enqueue(packet)) {
    return false;
  }

  ++sent_;
  return true;
}

void HAPLoopback::onReceive(Receiver receiver) {
  receiver_ = receiver;
}

void HAPLoopback::onSendResult(SendResult result) {
  sendResult_ = result;
}

void HAPLoopback::sent(const HAPAddress& to, bool delivered) noexcept {
  if (sendResult_.is_valid()) {
    sendResult_(to, delivered);
  }
}

void HAPLoopback::receive(const HAPLoopbackBus::Packet& packet) noexcept {
  ++received_;

  if (receiver_.is_valid()) {
    receiver_(packet.from, packet.data, packet.size);
  }
}

size_t HAPLoopback::sentCount() const noexcept {
  return sent_;
}

size_t HAPLoopback::receivedCount() const noexcept {
  return received_;
}
