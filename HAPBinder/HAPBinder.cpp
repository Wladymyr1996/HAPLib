#define HLOG_MODULE_NAME "HapBind"

#include <HAPBinder/HAPBinder.hpp>

#include <HAPMessages/HAPBindMessages.hpp>
#include <HAPRandom/HAPRandom.hpp>
#include <HLog/HLog.hpp>

#include <cstring>

HAPBinder::HAPBinder(HAPITransport& transport, HAPRouter& router,
                     const HAPNode& node) noexcept
    : transport_(transport),
      router_(router),
      node_(node),
      window_(HAP_BIND_WINDOW_MS),
      announce_(HAP_ANNOUNCE_PERIOD_MS),
      confirm_(HAP_CONFIRM_TIMEOUT_MS) {}

// ---------------------------------------------------------------------------
// Master
// ---------------------------------------------------------------------------

bool HAPBinder::listen() noexcept {
  if (router_.isFull()) {
    // A limit of the router's child table, not a decision made here.
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

bool HAPBinder::handleAnnounce(const HAPAddress& from,
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
  // children with it.
  //
  // The old index is kept rather than a fresh one taken. Links and
  // anything a master has cached name a child by its index, and a device that
  // came back as 1.3 instead of 1.2 would break every one of them silently -
  // which is the same failure B8 exists to prevent.
  const HAPChild* returning = router_.childById(from.nodeId());
  uint8_t index = 0;

  if (returning != nullptr) {
    // The stale entry is NOT removed here. If this handshake fails the child
    // must still be there afterwards. handleConfirm() replaces it, once there is
    // something to replace it with.
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

  pending_ = from;
  pendingIndex_ = index;
  pendingType_ = announce.deviceType;
  pendingCapabilities_ = announce.capabilities;
  pendingRev_ = announce.descriptorRev;
  pendingInterval_ = announce.reportIntervalSec;

  state_ = State::Accepting;
  confirm_.start(HAP_CONFIRM_TIMEOUT_MS);

  if (!sendAccept(from, index)) {
    // Never left. Give back the index, and go on listening - the window has
    // not closed and somebody else may still bind in what is left.
    abandonAccept();
    state_ = window_.isExpired() ? State::Idle : State::Listening;
    return true;
  }

  HInfo("accepted %s '%s' as child %u", from.toString().c_str(),
        announce.nodeName.c_str(), index);
  return true;
}

bool HAPBinder::sendAccept(const HAPAddress& to, uint8_t childIndex) noexcept {
  HAPRandom::fill(linkKey_, sizeof(linkKey_));

  HAPBindAccept accept;
  accept.childIndex = childIndex;
  accept.channel = 0;  // Still on the wire, but nothing reads it any more.
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

bool HAPBinder::handleConfirm(const HAPAddress& from,
                              const HAPFrame& frame) noexcept {
  if (state_ != State::Accepting || from != pending_) {
    return false;
  }

  HAPReader reader(frame.payload, frame.payloadSize);
  HAPBindConfirm confirm;
  confirm.decode(reader);

  HAPChild child;
  child.index = pendingIndex_;
  child.id = pending_.nodeId();
  child.deviceType = pendingType_;
  child.capabilities = pendingCapabilities_;
  child.descriptorRev = confirm.descriptorRev != 0 ? confirm.descriptorRev
                                                   : pendingRev_;

  // Kept from the announcement, because nothing above this node will ever hear
  // one - it travels in ChildAttached instead, and is what lets the root
  // measure a silence.
  child.reportIntervalSec = pendingInterval_;

  // The key goes with the child, and is persisted with it. No transport uses it
  // yet; the stored format still carries it.
  std::memcpy(child.linkKey, linkKey_, HAP_KEY_LEN);

  // The old entry for a device that is coming back goes NOW, and not a moment
  // earlier - see handleAnnounce(). addChild() refuses an id it already holds,
  // so without this a re-adoption would fail at the last step and take the
  // working child with it.
  const HAPChild* stale = router_.childById(child.id);
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

  HInfo("bound %s as child %u", child.id.toString().c_str(), child.index);

  if (onChildBound_.is_valid()) {
    onChildBound_(child);
  }

  return true;
}

void HAPBinder::abandonAccept() noexcept {
  if (!pending_.isZero()) {
    pending_ = HAPAddress::zero();
  }

  pendingIndex_ = 0;
  confirm_.stop();
}

// ---------------------------------------------------------------------------
// Slave
// ---------------------------------------------------------------------------

bool HAPBinder::announce() noexcept {
  if (router_.hasParent()) {
    // One parent, for life. An accidental rebind would silently detach a
    // subtree, so only a factory reset can free this node.
    HWarning("already bound to %s", router_.parentId().toString().c_str());
    return false;
  }

  state_ = State::Announcing;
  window_.start(HAP_BIND_WINDOW_MS);
  announce_.start(HAP_ANNOUNCE_PERIOD_MS);

  sendAnnouncement();

  HInfo("announcing");
  return true;
}

void HAPBinder::stopAnnouncing() noexcept {
  if (state_ != State::Announcing) {
    return;
  }

  window_.stop();
  announce_.stop();
  state_ = State::Idle;
}

bool HAPBinder::isAnnouncing() const noexcept {
  return state_ == State::Announcing;
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
  frame.flags = HAPFlags::None;
  frame.seq = ++seq_;
  frame.src = transport_.localId();  // dst stays zero: nobody is addressed yet.
  frame.payload = payload;
  frame.payloadSize = static_cast<uint8_t>(writer.size());

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t size = frame.encode(buffer, sizeof(buffer));

  // A node that has never joined a network can multicast here. On Thread it
  // cannot, until the enrol handshake replaces this announcement.
  return size > 0 && transport_.multicast(buffer, size);
}

bool HAPBinder::handleAccept(const HAPAddress& from, const HAPFrame& frame) noexcept {
  if (state_ != State::Announcing) {
    return false;
  }

  HAPReader reader(frame.payload, frame.payloadSize);
  HAPBindAccept accept;

  if (!accept.decode(reader)) {
    HWarning("unreadable acceptance from %s", from.toString().c_str());
    return true;
  }

  if (accept.childIndex < HAPRouter::kMinChildIndex || accept.childIndex > HAP_MAX_CHILDREN) {
    HWarning("refused: %s offered index %u", from.toString().c_str(),
             accept.childIndex);
    return true;
  }

  std::memcpy(linkKey_, accept.linkKey, sizeof(linkKey_));

  router_.setParent(from.nodeId(), accept.childIndex);

  if (!sendConfirm()) {
    HWarning("could not confirm to %s", from.toString().c_str());
    router_.clearParent();
    return true;
  }

  window_.stop();
  announce_.stop();
  state_ = State::Bound;

  HInfo("bound to %s '%s' as child %u", from.toString().c_str(),
        accept.masterName.c_str(), accept.childIndex);

  if (onBound_.is_valid()) {
    onBound_(from, accept.childIndex, linkKey_, 0);
  }

  return true;
}

bool HAPBinder::sendConfirm() noexcept {
  HAPBindConfirm confirm;
  confirm.descriptorRev = node_.descriptorRev();

  uint8_t payload[HAP_MAX_PAYLOAD_SIZE];
  HAPWriter writer(payload, sizeof(payload));
  confirm.encode(writer);

  return writer.ok() &&
         transmit(HAPAddress::fromNodeId(router_.parentId()),
                  HAPMessage::BindConfirm, HAPFlags::None, payload,
                  writer.size());
}

// ---------------------------------------------------------------------------
// Shared
// ---------------------------------------------------------------------------

bool HAPBinder::transmit(const HAPAddress& to, HAPMessage type, uint8_t flags,
                         const uint8_t* payload, size_t size) noexcept {
  HAPFrame frame;
  frame.type = static_cast<uint8_t>(type);
  frame.flags = flags;
  frame.seq = ++seq_;
  frame.dst = to.nodeId();
  frame.src = transport_.localId();
  frame.payload = payload;
  frame.payloadSize = static_cast<uint8_t>(size);

  uint8_t buffer[HAP_MAX_FRAME_SIZE];
  const size_t encoded = frame.encode(buffer, sizeof(buffer));

  return encoded > 0 && transport_.send(to, buffer, encoded);
}

bool HAPBinder::handle(const HAPAddress& from, const HAPFrame& frame) noexcept {
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
