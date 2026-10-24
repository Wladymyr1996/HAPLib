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

HAPRoute toNode() noexcept {
  HAPRoute route;
  route.action = HAPRouteAction::ToNode;
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

void HAPRouter::setParent(const HAPNodeId& id, uint8_t indexAtParent) noexcept {
  parentId_ = id;
  indexAtParent_ = indexAtParent;
  hasParent_ = true;
}

void HAPRouter::clearParent() noexcept {
  parentId_ = HAPNodeId::zero();
  indexAtParent_ = 0;
  hasParent_ = false;
}

bool HAPRouter::hasParent() const noexcept {
  return hasParent_;
}

bool HAPRouter::isRoot() const noexcept {
  return !hasParent_;
}

const HAPNodeId& HAPRouter::parentId() const noexcept {
  return parentId_;
}

uint8_t HAPRouter::indexAtParent() const noexcept {
  return indexAtParent_;
}

// ---------------------------------------------------------------------------
// Children
// ---------------------------------------------------------------------------

uint8_t HAPRouter::childCapacity() const noexcept {
  // The link upward costs one slot of the child table (a limit inherited from
  // ESP-NOW's peer slots; the transport no longer imposes it).
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

  if (newChild.index < kMinChildIndex || newChild.index > HAP_MAX_CHILDREN) {
    return false;
  }

  if (child(newChild.index) != nullptr) {
    return false;
  }

  // The same device must not appear twice under two indices: its reports would
  // arrive by two paths and a master would see one thing as two.
  if (childById(newChild.id) != nullptr) {
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

bool HAPRouter::setChildReportInterval(uint8_t index, uint16_t seconds) noexcept {
  for (HAPChild& candidate : children_) {
    if (candidate.index != index) {
      continue;
    }

    if (candidate.reportIntervalSec == seconds) {
      return false;
    }

    candidate.reportIntervalSec = seconds;
    return true;
  }

  return false;
}

const HAPChild* HAPRouter::childById(const HAPNodeId& id) const noexcept {
  for (const HAPChild& candidate : children_) {
    if (candidate.id == id) {
      return &candidate;
    }
  }

  return nullptr;
}

const HAPChild* HAPRouter::childAt(size_t position) const noexcept {
  return position < children_.size() ? &children_[position] : nullptr;
}

bool HAPRouter::isKnown(const HAPNodeId& id) const noexcept {
  if (hasParent_ && id == parentId_) {
    return true;
  }

  return childById(id) != nullptr;
}

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

HAPRoute HAPRouter::resolveDownstream(const HAPNodeId& dst,
                                      const HAPNodeId& self) const noexcept {
  if (dst == self) {
    return deliver();
  }

  if (dst.isZero()) {
    // "The master" is upstream by definition; there is no way down to it.
    return drop(HAPResult::BadRequest);
  }

  const HAPChild* direct = childById(dst);
  if (direct != nullptr) {
    return toChild(direct->index);
  }

  return toNode();
}

HAPRoute HAPRouter::route(const HAPFrame& frame, const HAPNodeId& from,
                          const HAPNodeId& self) const noexcept {
  const bool fromParent = hasParent_ && from == parentId_;
  const HAPChild* sender = childById(from);

  if (frame.dst == self) {
    // Trust is the network's key, not the sender's address - but a node that is
    // in no tree still answers only the binder.
    if (!hasParent_ && children_.empty()) {
      return drop(HAPResult::NotBound);
    }

    return deliver();
  }

  // Nothing inside a frame says where it was LAST sent from, so the sender's
  // address is the only trustworthy statement about it - and a stranger's frame
  // is the binder's business, not the router's.
  if (!fromParent && sender == nullptr) {
    return drop(HAPResult::NotBound);
  }

  if (frame.dst.isZero()) {
    if (sender == nullptr) {
      // The parent does not send frames for the master to its own child.
      // Something is forwarding the wrong way.
      return drop(HAPResult::BadRequest);
    }

    return hasParent_ ? toParent() : deliver();
  }

  // For some other node: nothing forwards downward any more.
  return drop(HAPResult::BadRequest);
}
