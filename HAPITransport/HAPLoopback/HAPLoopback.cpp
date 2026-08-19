#include <HAPITransport/HAPLoopback/HAPLoopback.hpp>

#include <cstring>

// ---------------------------------------------------------------------------
// HAPLoopbackBus
// ---------------------------------------------------------------------------

void HAPLoopbackBus::attach(HAPLoopback& node) noexcept {
  if (find(node.localMac()) != nullptr || nodes_.full()) {
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

HAPLoopback* HAPLoopbackBus::find(const HAPMac& mac) const noexcept {
  for (HAPLoopback* node : nodes_) {
    if (node->localMac() == mac) {
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

void HAPLoopbackBus::setReachable(const HAPMac& a, const HAPMac& b,
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

bool HAPLoopbackBus::isReachable(const HAPMac& a, const HAPMac& b) const noexcept {
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
    if (node->localMac() == packet.from) {
      continue;  // A radio does not hear itself.
    }

    const bool addressed =
        packet.to.isBroadcast() || node->localMac() == packet.to;
    if (!addressed) {
      continue;
    }

    // Every one of these three is a real way to receive nothing, and each one
    // looks exactly like a protocol bug from the far end.
    if (node->channel() != packet.channel) {
      continue;
    }

    if (!isReachable(packet.from, node->localMac())) {
      continue;
    }

    const HAPLoopback::Peer* peer = node->findPeer(packet.from);

    if (packet.encrypted) {
      if (peer == nullptr || !peer->encrypted ||
          std::memcmp(peer->key, packet.key, HAP_KEY_LEN) != 0) {
        continue;
      }
    } else if (!packet.to.isBroadcast() && peer != nullptr && peer->encrypted) {
      // THE REVERSE CASE, and the one that had to be found on a bench.
      //
      // A node holding the sender as an ENCRYPTED peer will not take plain text
      // from it: ESP-NOW expects everything from that address to be protected,
      // and discards what is not - while still acknowledging it at the MAC
      // layer, so the sender sees a perfectly successful transmission.
      //
      // Not modelling this is what let a factory reset that forgot to drop its
      // peers pass 1112 host checks and then fail on three boards: the reset
      // node kept its old parent as an encrypted peer, so the next BindAccept -
      // which must arrive in the CLEAR, because it carries the new key - was
      // dropped before any protocol code saw it, for ever, until a power cycle.
      //
      // Broadcast is exempt, and really is: a broadcast is addressed to the
      // broadcast peer rather than to the sender's entry, which is why an
      // unbound node can still be heard by everybody.
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

HAPLoopback::HAPLoopback(const HAPMac& mac, HAPLoopbackBus& bus) noexcept
    : mac_(mac), bus_(bus) {
  bus_.attach(*this);
}

HAPLoopback::~HAPLoopback() {
  bus_.detach(*this);
}

bool HAPLoopback::begin(uint8_t channel) {
  channel_ = channel;
  started_ = true;
  return true;
}

uint8_t HAPLoopback::channel() const noexcept {
  return channel_;
}

const HAPMac& HAPLoopback::localMac() const noexcept {
  return mac_;
}

const HAPLoopback::Peer* HAPLoopback::findPeer(const HAPMac& mac) const noexcept {
  for (const Peer& peer : peers_) {
    if (peer.mac == mac) {
      return &peer;
    }
  }

  return nullptr;
}

bool HAPLoopback::addPeer(const HAPMac& mac, const uint8_t* key) {
  if (peers_.full()) {
    return false;
  }

  Peer peer;
  peer.mac = mac;
  peer.encrypted = key != nullptr;

  if (key != nullptr) {
    std::memcpy(peer.key, key, HAP_KEY_LEN);
  }

  // Re-adding replaces, which is what an implementation must do for a peer
  // whose key has just been renegotiated.
  for (Peer& existing : peers_) {
    if (existing.mac == mac) {
      existing = peer;
      return true;
    }
  }

  peers_.push_back(peer);
  return true;
}

bool HAPLoopback::removePeer(const HAPMac& mac) {
  for (auto it = peers_.begin(); it != peers_.end(); ++it) {
    if (it->mac == mac) {
      peers_.erase(it);
      return true;
    }
  }

  return false;
}

bool HAPLoopback::hasPeer(const HAPMac& mac) const noexcept {
  return findPeer(mac) != nullptr;
}

bool HAPLoopback::send(const HAPMac& mac, const uint8_t* data, size_t size) {
  if (!started_ || data == nullptr || size == 0 || size > HAP_MAX_FRAME_SIZE) {
    return false;
  }

  // The rule that matters: a radio will not unicast to a stranger. Code written
  // against a simulator that allowed it would fail on the first real board.
  const Peer* peer = findPeer(mac);
  if (peer == nullptr) {
    return false;
  }

  HAPLoopbackBus::Packet packet;
  packet.from = mac_;
  packet.to = mac;
  packet.channel = channel_;
  packet.encrypted = peer->encrypted;
  std::memcpy(packet.key, peer->key, HAP_KEY_LEN);
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
  packet.from = mac_;
  packet.to = HAPMac::broadcast();
  packet.channel = channel_;
  packet.encrypted = false;  // Broadcast never is - which is the bind window's risk.
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

void HAPLoopback::sent(const HAPMac& to, bool delivered) noexcept {
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

bool HAPLoopback::setChannel(uint8_t channel) {
  channel_ = channel;
  return true;
}

size_t HAPLoopback::sentCount() const noexcept {
  return sent_;
}

size_t HAPLoopback::receivedCount() const noexcept {
  return received_;
}
