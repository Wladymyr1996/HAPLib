#define HLOG_MODULE_NAME "HapBind"

#include <HAPBinder/HAPBinder.hpp>

#include <HAPMessages/HAPBindMessages.hpp>
#include <HAPRandom/HAPRandom.hpp>
#include <HLog/HLog.hpp>

#include <cstring>

constexpr uint8_t HAPBinder::kSweepChannels[];

HAPBinder::HAPBinder(HAPITransport& transport, HAPRouter& router,
                     const HAPNode& node) noexcept
    : transport_(transport),
      router_(router),
      node_(node),
      window_(HAP_BIND_WINDOW_MS),
      announce_(HAP_ANNOUNCE_PERIOD_MS),
      dwell_(HAP_SWEEP_DWELL_MS),
      confirm_(HAP_CONFIRM_TIMEOUT_MS) {}

// ---------------------------------------------------------------------------
// Master
// ---------------------------------------------------------------------------

bool HAPBinder::listen() noexcept {
  if (router_.isFull()) {
    // Not a policy: ESP-NOW holds six encrypted peers, and a seventh child
    // would be a link nobody could encrypt.
    HWarning("cannot bind: already %u children", HAP_MAX_CHILDREN);
    return false;
  }

  state_ = State::Listening;
  window_.start(HAP_BIND_WINDOW_MS);
  HInfo("listening for %u s", HAP_BIND_WINDOW_MS / 1000);
  return true;
}

void HAPBinder::stopListening() noexcept {
  if (state_ == State::Listening || state_ == State::Accepting) {
    abandonAccept();
    window_.stop();
    state_ = State::Idle;
  }
}

bool HAPBinder::isListening() const noexcept {
  return state_ == State::Listening || state_ == State::Accepting;
}

bool HAPBinder::handleAnnounce(const HAPMac& from,
                               const HAPFrame& frame) noexcept {
  if (state_ != State::Listening) {
    return false;
  }

  HAPReader reader(frame.payload, frame.payloadSize);
  HAPBindAnnounce announce;

  if (!announce.decode(reader)) {
    HWarning("unreadable announcement from %s", from.toString().c_str());
    return true;
  }

  // Only page 0 is looked at. A node with more instances than one frame holds
  // sends more pages, and they are ignored on purpose: the master is about to
  // cache a descriptor revision it has never seen, so it will ask for the whole
  // thing with Describe anyway.
  if (announce.pageIndex != 0) {
    return true;
  }

  // A NODE ONLY ANNOUNCES WHEN IT IS UNBOUND, so an announcement from an
  // address already in the child table is not ambiguous: it is proof that the
  // child no longer believes it has a parent, and the entry here is the stale
  // one. This used to be refused, which let a dead record outvote a live device
  // - the only way back was a factory reset of the PARENT, dropping its other
  // children with it. Worse, the ghost held one of six encrypted peer slots, so
  // a node that collected a few eventually could not adopt anybody at all.
  //
  // The old index is kept rather than a fresh one taken. Links, paths and
  // anything a master has cached name a child by its index, and a device that
  // came back as 1.3 instead of 1.2 would break every one of them silently -
  // which is the same failure B8 exists to prevent.
  const HAPChild* returning = router_.childByMac(from);
  uint8_t index = 0;

  if (returning != nullptr) {
    // The stale entry is NOT removed here. If this handshake fails - and the
    // one before the key switch is the one that does - the child must still be
    // there afterwards. handleConfirm() replaces it, once there is something to
    // replace it with.
    index = returning->index;
    HInfo("%s announced but is already child %u; it has been reset, re-adopting",
          from.toString().c_str(), index);
  } else {
    index = router_.freeChildIndex();
  }

  if (index == 0) {
    HWarning("no room for %s", from.toString().c_str());
    return true;
  }

  // EVERYTHING THIS NODE NEEDS TO RECOGNISE THE SEND REPORT IS SET FIRST, and
  // the acceptance goes out afterwards. The order is load-bearing, not tidiness.
  //
  // notifySent() is the whole mechanism for the key switch, and it guards on
  // `state_ == Accepting && to == pending_`. The send report arrives on the
  // Wi-Fi task, which pre-empts whatever task this runs on, so it can land
  // BEFORE the next line of this function executes - and it did, on the bench,
  // because the application logged the frame it had just sent and the report
  // came back during the log line. The guard then discarded it, the link stayed
  // unencrypted for the 200 ms fallback, and the child's confirmation - sent
  // encrypted about ten milliseconds later - was dropped by the radio, below
  // the protocol, where nothing can log it. The symptom is "the accept went out
  // and nobody confirmed", which is exactly what it does not look like.
  //
  // Assigning first costs nothing and closes the window entirely: a report that
  // arrives inside sendAccept() is now recognised.
  pending_ = from;
  pendingIndex_ = index;
  pendingType_ = announce.deviceType;
  pendingCapabilities_ = announce.capabilities;
  pendingRev_ = announce.descriptorRev;
  pendingInterval_ = announce.reportIntervalSec;
  keySwitched_ = false;

  state_ = State::Accepting;
  confirm_.start(HAP_CONFIRM_TIMEOUT_MS);

  if (!sendAccept(from, index)) {
    // Never left. Give back the index and the peer, and go on listening - the
    // window has not closed and somebody else may still bind in what is left.
    abandonAccept();
    state_ = window_.isExpired() ? State::Idle : State::Listening;
    return true;
  }

  HInfo("accepted %s '%s' as child %u", from.toString().c_str(),
        announce.nodeName.c_str(), index);
  return true;
}

bool HAPBinder::sendAccept(const HAPMac& to, uint8_t childIndex) noexcept {
  HAPRandom::fill(linkKey_, sizeof(linkKey_));

  // Added WITHOUT the key: this frame carries it, so it has to be readable by
  // somebody who does not have it yet.
  if (!transport_.addPeer(to, nullptr)) {
    HWarning("could not add %s as a peer", to.toString().c_str());
    return false;
  }

  HAPBindAccept accept;
  accept.childIndex = childIndex;
  accept.channel = transport_.channel();
  accept.masterName = node_.name();
  std::memcpy(accept.linkKey, linkKey_, sizeof(linkKey_));

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  accept.encode(writer);

  if (!writer.ok()) {
    return false;
  }

  return transmit(to, HAPMessage::BindAccept, HAPFlags::None, payload,
                  writer.size());
}

bool HAPBinder::handleConfirm(const HAPMac& from,
                              const HAPFrame& frame) noexcept {
  if (state_ != State::Accepting || from != pending_) {
    return false;
  }

  HAPReader reader(frame.payload, frame.payloadSize);
  HAPBindConfirm confirm;
  confirm.decode(reader);

  HAPChild child;
  child.index = pendingIndex_;
  child.mac = pending_;
  child.deviceType = pendingType_;
  child.capabilities = pendingCapabilities_;
  child.descriptorRev = confirm.descriptorRev != 0 ? confirm.descriptorRev
                                                   : pendingRev_;

  // Kept from the announcement, because nothing above this node will ever hear
  // one - it travels in ChildAttached instead, and is what lets the root
  // measure a silence.
  child.reportIntervalSec = pendingInterval_;

  // The key goes with the child, and has to outlive this run. The child has
  // already stored its own copy; a master that kept none would come back from a
  // reboot holding this address as a PLAIN peer while the child went on
  // transmitting encrypted - deaf in both directions, for ever, because a child
  // has one parent for life and will never announce again.
  std::memcpy(child.linkKey, linkKey_, HAP_KEY_LEN);

  // The old entry for a device that is coming back goes NOW, and not a moment
  // earlier - see handleAnnounce(). addChild() refuses a MAC it already holds,
  // so without this a re-adoption would fail at the last step and take the
  // working child with it.
  const HAPChild* stale = router_.childByMac(child.mac);
  if (stale != nullptr) {
    router_.removeChild(stale->index);
  }

  if (!router_.addChild(child)) {
    HWarning("could not record child %u", pendingIndex_);
    abandonAccept();
    return true;
  }

  confirm_.stop();
  window_.stop();
  state_ = State::Idle;

  HInfo("bound %s as child %u", child.mac.toString().c_str(), child.index);

  if (onChildBound_.is_valid()) {
    onChildBound_(child);
  }

  return true;
}

void HAPBinder::switchToEncrypted() noexcept {
  if (keySwitched_ || pending_.isZero()) {
    return;
  }

  // Re-adding a peer replaces it, which is how a link changes key without ever
  // ceasing to be a peer - drop it and re-add it instead, and a frame in flight
  // between the two has nowhere to arrive.
  transport_.addPeer(pending_, linkKey_);
  keySwitched_ = true;
}

void HAPBinder::notifySent(const HAPMac& to, bool delivered) noexcept {
  if (state_ != State::Accepting || to != pending_) {
    return;
  }

  if (!delivered) {
    // Nobody acknowledged the acceptance. Switching the key now would only make
    // the retry unreadable too; let the confirmation timeout take it.
    HWarning("acceptance to %s was not acknowledged", to.toString().c_str());
    return;
  }

  // The exact moment: the accept is on the air, in the clear, and everything
  // after it can be encrypted.
  switchToEncrypted();
}

void HAPBinder::abandonAccept() noexcept {
  if (!pending_.isZero()) {
    // Everything given out is taken back. A peer left behind would occupy one
    // of six encrypted slots for a device that never finished binding.
    transport_.removePeer(pending_);
    pending_ = HAPMac::zero();
  }

  pendingIndex_ = 0;
  keySwitched_ = false;
  confirm_.stop();
}

// ---------------------------------------------------------------------------
// Slave
// ---------------------------------------------------------------------------

bool HAPBinder::announce() noexcept {
  if (router_.hasParent()) {
    // One parent, for life. An accidental rebind would silently detach a
    // subtree, so only a factory reset can free this node.
    HWarning("already bound to %s", router_.parentMac().toString().c_str());
    return false;
  }

  state_ = State::Announcing;
  window_.start(HAP_BIND_WINDOW_MS);
  announce_.start(HAP_ANNOUNCE_PERIOD_MS);
  dwell_.start(HAP_SWEEP_DWELL_MS);

  homeChannel_ = transport_.channel();
  sweepIndex_ = 0;

  // An unbound node has no idea which channel its future parent is on, so it
  // starts on its own and works through the rest as the dwell expires.
  sendAnnouncement();

  HInfo("announcing on channel %u", transport_.channel());
  return true;
}

void HAPBinder::stopAnnouncing() noexcept {
  if (state_ != State::Announcing) {
    return;
  }

  if (transport_.channel() != homeChannel_ && homeChannel_ != 0) {
    transport_.setChannel(homeChannel_);
  }

  window_.stop();
  announce_.stop();
  dwell_.stop();
  state_ = State::Idle;
}

bool HAPBinder::isAnnouncing() const noexcept {
  return state_ == State::Announcing;
}

uint8_t HAPBinder::sweepChannel() const noexcept {
  return state_ == State::Announcing ? transport_.channel() : 0;
}

bool HAPBinder::sendAnnouncement() noexcept {
  HAPBindAnnounce announce;
  if (!node_.fillAnnounce(announce, 0)) {
    return false;
  }

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  announce.encode(writer);

  if (!writer.ok()) {
    return false;
  }

  HAPFrame frame;
  frame.type = static_cast<uint8_t>(HAPMessage::BindAnnounce);
  frame.flags = HAPFlags::None;  // Not upstream: there is nothing above yet.
  frame.seq = ++seq_;
  frame.payload = payload;
  frame.payloadSize = static_cast<uint8_t>(writer.size());

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = frame.encode(buffer, sizeof(buffer));

  return size > 0 && transport_.broadcast(buffer, size);
}

bool HAPBinder::handleAccept(const HAPMac& from, const HAPFrame& frame) noexcept {
  if (state_ != State::Announcing) {
    return false;
  }

  HAPReader reader(frame.payload, frame.payloadSize);
  HAPBindAccept accept;

  if (!accept.decode(reader)) {
    HWarning("unreadable acceptance from %s", from.toString().c_str());
    return true;
  }

  if (accept.childIndex < HAPPath::kMinHop || accept.childIndex > HAP_MAX_CHILDREN) {
    HWarning("refused: %s offered index %u", from.toString().c_str(),
             accept.childIndex);
    return true;
  }

  // Whatever channel the sweep had reached, the network's channel is the one
  // the master just named - stay there.
  if (accept.channel != 0 && accept.channel != transport_.channel()) {
    transport_.setChannel(accept.channel);
  }

  std::memcpy(linkKey_, accept.linkKey, sizeof(linkKey_));

  if (!transport_.addPeer(from, linkKey_)) {
    HWarning("could not add %s as an encrypted peer", from.toString().c_str());
    return true;
  }

  router_.setParent(from, accept.childIndex);

  // Encrypted, with the key that just arrived - so that it decrypts at all is
  // the proof the key survived the trip.
  if (!sendConfirm()) {
    HWarning("could not confirm to %s", from.toString().c_str());
    router_.clearParent();
    transport_.removePeer(from);
    return true;
  }

  window_.stop();
  announce_.stop();
  dwell_.stop();
  state_ = State::Bound;

  HInfo("bound to %s '%s' as child %u on channel %u", from.toString().c_str(),
        accept.masterName.c_str(), accept.childIndex, transport_.channel());

  if (onBound_.is_valid()) {
    onBound_(from, accept.childIndex, linkKey_, transport_.channel());
  }

  return true;
}

bool HAPBinder::sendConfirm() noexcept {
  HAPBindConfirm confirm;
  confirm.descriptorRev = node_.descriptorRev();

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  confirm.encode(writer);

  return writer.ok() && transmit(router_.parentMac(), HAPMessage::BindConfirm,
                                 HAPFlags::Upstream, payload, writer.size());
}

// ---------------------------------------------------------------------------
// Shared
// ---------------------------------------------------------------------------

bool HAPBinder::transmit(const HAPMac& to, HAPMessage type, uint8_t flags,
                         const uint8_t* payload, size_t size) noexcept {
  HAPFrame frame;
  frame.type = static_cast<uint8_t>(type);
  frame.flags = flags;
  frame.seq = ++seq_;
  frame.payload = payload;
  frame.payloadSize = static_cast<uint8_t>(size);

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t encoded = frame.encode(buffer, sizeof(buffer));

  return encoded > 0 && transport_.send(to, buffer, encoded);
}

bool HAPBinder::handle(const HAPMac& from, const HAPFrame& frame) noexcept {
  switch (frame.message()) {
    case HAPMessage::BindAnnounce:
      return handleAnnounce(from, frame);
    case HAPMessage::BindAccept:
      return handleAccept(from, frame);
    case HAPMessage::BindConfirm:
      return handleConfirm(from, frame);
    default:
      return false;
  }
}

void HAPBinder::update() noexcept {
  switch (state_) {
    case State::Listening:
      if (window_.isExpired()) {
        HInfo("window closed with nobody bound");
        stopListening();
      }
      break;

    case State::Accepting:
      // The fallback: a transport that never reports a send would otherwise
      // leave this link unencrypted forever, and the confirmation would be
      // dropped by the radio before any of this code saw it.
      if (!keySwitched_ && confirm_.elapsedMs() >= HAP_KEY_SWITCH_TIMEOUT_MS) {
        HWarning("no send report for the acceptance; switching key on timeout");
        switchToEncrypted();
      }

      if (confirm_.isExpired()) {
        HWarning("%s never confirmed", pending_.toString().c_str());
        abandonAccept();

        // The window has not necessarily closed - somebody else may still bind
        // in whatever is left of it.
        state_ = window_.isExpired() ? State::Idle : State::Listening;
      }
      break;

    case State::Announcing:
      if (window_.isExpired()) {
        HInfo("nobody answered");
        stopAnnouncing();
        break;
      }

      if (dwell_.isExpired()) {
        // Nobody on this channel. A master listens only on its own, so an
        // unbound node has to go looking.
        sweepIndex_ = (sweepIndex_ + 1) % kSweepCount;
        transport_.setChannel(kSweepChannels[sweepIndex_]);
        dwell_.reset();
        announce_.reset();
        sendAnnouncement();
        break;
      }

      if (announce_.isExpired()) {
        announce_.reset();
        sendAnnouncement();
      }
      break;

    case State::Idle:
    case State::Bound:
      break;
  }
}

HAPBinder::State HAPBinder::state() const noexcept {
  return state_;
}

void HAPBinder::onChildBound(ChildBoundHook hook) noexcept {
  onChildBound_ = hook;
}

void HAPBinder::onBound(BoundHook hook) noexcept {
  onBound_ = hook;
}

const uint8_t* HAPBinder::linkKey() const noexcept {
  return linkKey_;
}
