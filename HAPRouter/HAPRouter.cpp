#include <HAPRouter/HAPRouter.hpp>

namespace {

HAPRoute deliver() noexcept {
  HAPRoute route;
  route.action = HAPRouteAction::Deliver;
  return route;
}

HAPRoute toParent() noexcept {
  HAPRoute route;
  route.action = HAPRouteAction::ToParent;
  return route;
}

HAPRoute toChild(uint8_t index) noexcept {
  HAPRoute route;
  route.action = HAPRouteAction::ToChild;
  route.childIndex = index;
  return route;
}

HAPRoute drop(HAPResult reason, uint8_t childIndex = 0) noexcept {
  HAPRoute route;
  route.action = HAPRouteAction::Drop;
  route.reason = reason;
  route.childIndex = childIndex;
  return route;
}

}  // namespace

bool HAPChild::isBatteryPowered() const noexcept {
  return (capabilities & HAPCaps::BatteryPowered) != 0;
}

bool HAPChild::hasLinkKey() const noexcept {
  // All-zero means none. A generated key could in principle be all zeroes; the
  // odds are one in 2^128, which is a better trade than carrying a flag that
  // every caller has to remember to set.
  for (uint8_t byte : linkKey) {
    if (byte != 0) {
      return true;
    }
  }

  return false;
}

// ---------------------------------------------------------------------------
// Parent
// ---------------------------------------------------------------------------

void HAPRouter::setParent(const HAPMac& mac, uint8_t indexAtParent) noexcept {
  parentMac_ = mac;
  indexAtParent_ = indexAtParent;
  hasParent_ = true;
}

void HAPRouter::clearParent() noexcept {
  parentMac_ = HAPMac::zero();
  indexAtParent_ = 0;
  hasParent_ = false;
}

bool HAPRouter::hasParent() const noexcept {
  return hasParent_;
}

bool HAPRouter::isRoot() const noexcept {
  return !hasParent_;
}

const HAPMac& HAPRouter::parentMac() const noexcept {
  return parentMac_;
}

uint8_t HAPRouter::indexAtParent() const noexcept {
  return indexAtParent_;
}

// ---------------------------------------------------------------------------
// Children
// ---------------------------------------------------------------------------

uint8_t HAPRouter::childCapacity() const noexcept {
  // The link upward is encrypted too, and comes out of the same six slots the
  // radio has for encrypted peers.
  return hasParent_ ? HAP_MAX_CHILDREN - 1 : HAP_MAX_CHILDREN;
}

uint8_t HAPRouter::freeChildIndex() const noexcept {
  if (children_.size() >= childCapacity()) {
    return 0;
  }

  for (uint8_t index = 1; index <= HAP_MAX_CHILDREN; ++index) {
    if (child(index) == nullptr) {
      return index;
    }
  }

  return 0;
}

bool HAPRouter::addChild(const HAPChild& newChild) noexcept {
  if (isFull()) {
    return false;
  }

  if (newChild.index < HAPPath::kMinHop || newChild.index > HAP_MAX_CHILDREN) {
    return false;
  }

  if (child(newChild.index) != nullptr) {
    return false;
  }

  // The same device must not appear twice under two indices: its reports would
  // arrive by two paths and a master would see one thing as two.
  if (childByMac(newChild.mac) != nullptr) {
    return false;
  }

  children_.push_back(newChild);
  return true;
}

bool HAPRouter::removeChild(uint8_t index) noexcept {
  for (auto it = children_.begin(); it != children_.end(); ++it) {
    if (it->index == index) {
      children_.erase(it);
      return true;
    }
  }

  return false;
}

void HAPRouter::clearChildren() noexcept {
  children_.clear();
}

size_t HAPRouter::childCount() const noexcept {
  return children_.size();
}

bool HAPRouter::isFull() const noexcept {
  return children_.size() >= childCapacity();
}

const HAPChild* HAPRouter::child(uint8_t index) const noexcept {
  for (const HAPChild& candidate : children_) {
    if (candidate.index == index) {
      return &candidate;
    }
  }

  return nullptr;
}

const HAPChild* HAPRouter::childByMac(const HAPMac& mac) const noexcept {
  for (const HAPChild& candidate : children_) {
    if (candidate.mac == mac) {
      return &candidate;
    }
  }

  return nullptr;
}

const HAPChild* HAPRouter::childAt(size_t position) const noexcept {
  return position < children_.size() ? &children_[position] : nullptr;
}

bool HAPRouter::isKnown(const HAPMac& mac) const noexcept {
  if (hasParent_ && mac == parentMac_) {
    return true;
  }

  return childByMac(mac) != nullptr;
}

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

HAPRoute HAPRouter::resolveDownstream(HAPFrame& frame) const noexcept {
  if (frame.dest.isEmpty()) {
    return deliver();
  }

  const uint8_t index = frame.dest.first();
  if (child(index) == nullptr) {
    // The frame is not dropped silently: the caller owes the originator a
    // RouteError naming this hop, which is the difference between a diagnosis
    // and a timeout.
    return drop(HAPResult::NoSuchChild, index);
  }

  frame.dest.shift();
  return toChild(index);
}

HAPRoute HAPRouter::route(HAPFrame& frame, const HAPMac& from) const noexcept {
  const bool fromParent = hasParent_ && from == parentMac_;
  const HAPChild* sender = childByMac(from);

  // Nothing inside a frame says where it came from, so the sender's address is
  // the only trustworthy statement about it - and a stranger's frame is the
  // binder's business, not the router's.
  if (!fromParent && sender == nullptr) {
    return drop(HAPResult::NotBound);
  }

  if (frame.isUpstream()) {
    if (sender == nullptr) {
      // The parent does not send upstream frames to its own child. Something is
      // forwarding the wrong way.
      return drop(HAPResult::BadRequest);
    }

    if (!frame.src.prepend(sender->index)) {
      // Five hops already: this has travelled further than the tree may be
      // deep, so it is circulating rather than climbing.
      return drop(HAPResult::BadRequest, sender->index);
    }

    return hasParent_ ? toParent() : deliver();
  }

  if (!fromParent) {
    return drop(HAPResult::BadRequest);
  }

  return resolveDownstream(frame);
}
