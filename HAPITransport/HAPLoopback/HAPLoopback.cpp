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
  if (dropCountdown_ > 0) {
    --dropCountdown_;
    ++dropped_;

    // A frame lost in the air is exactly the case where a send reports success
    // and nothing arrives, which is what the protocol above has to be built to
    // survive.
    return;
  }

  switch (packet.kind) {
    case Kind::Unicast: {
      // An address nobody holds is not an error and not a drop: the frame was
      // handed to the network, which had nowhere to take it.
      HAPLoopback* node = find(packet.to);

      if (node != nullptr && hears(*node, packet)) {
        node->receive(packet);
        ++delivered_;
      }
      break;
    }

    case Kind::Anycast: {
      // Resolved NOW, among the servers that could actually receive it, and
      // not when it was sent - the way an ALOC is resolved in the mesh. With
      // none, the frame is lost without a word.
      HAPLoopback* server = nullptr;

      for (HAPLoopback* node : nodes_) {
        if (node->servesMaster() && hears(*node, packet) &&
            (server == nullptr || node->localId() < server->localId())) {
          server = node;
        }
      }

      if (server != nullptr) {
        server->receive(packet);
        ++delivered_;
      }
      break;
    }

    case Kind::Multicast:
      for (HAPLoopback* node : nodes_) {
        if (hears(*node, packet)) {
          node->receive(packet);
          ++delivered_;
        }
      }
      break;
  }
}

bool HAPLoopbackBus::hears(const HAPLoopback& node,
                           const Packet& packet) const noexcept {
  // A radio does not hear itself, a node that has left the network hears
  // nothing, and a pair that cannot hear each other receives nothing - which
  // looks exactly like a protocol bug from the far end.
  return node.isAttached() && node.localAddress() != packet.from &&
         isReachable(packet.from.nodeId(), node.localId());
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
  attached_ = true;
  return true;
}

bool HAPLoopback::isAttached() const noexcept {
  return started_ && attached_;
}

void HAPLoopback::setAttached(bool attached) noexcept {
  attached_ = attached;
}

void HAPLoopback::setServesMaster(bool serves) noexcept {
  servesMaster_ = serves;
}

bool HAPLoopback::servesMaster() const noexcept {
  return servesMaster_;
}

const HAPNodeId& HAPLoopback::localId() const noexcept {
  return id_;
}

const HAPAddress& HAPLoopback::localAddress() const noexcept {
  return address_;
}

bool HAPLoopback::send(const HAPAddress& to, const uint8_t* data, size_t size) {
  const HAPLoopbackBus::Kind kind = to.isMaster()
                                        ? HAPLoopbackBus::Kind::Anycast
                                        : HAPLoopbackBus::Kind::Unicast;

  return post(kind, to, data, size);
}

bool HAPLoopback::multicast(const uint8_t* data, size_t size) {
  return post(HAPLoopbackBus::Kind::Multicast, HAPAddress::zero(), data, size);
}

bool HAPLoopback::post(HAPLoopbackBus::Kind kind, const HAPAddress& to,
                       const uint8_t* data, size_t size) noexcept {
  if (!isAttached() || data == nullptr || size == 0 ||
      size > HAP_MAX_FRAME_SIZE) {
    return false;
  }

  HAPLoopbackBus::Packet packet;
  packet.kind = kind;
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

void HAPLoopback::onReceive(Receiver receiver) {
  receiver_ = receiver;
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
