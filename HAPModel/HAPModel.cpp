#define HLOG_MODULE_NAME "HapModel"

#include <HAPModel/HAPModel.hpp>

#include <HLog/HLog.hpp>
#include <HSystemUtils/HSystemUtils.hpp>

// ---------------------------------------------------------------------------
// HAPRemoteNode
// ---------------------------------------------------------------------------

bool HAPRemoteNode::isBatteryPowered() const noexcept {
  return (capabilities & HAPCaps::BatteryPowered) != 0;
}

bool HAPRemoteNode::needsDescribe() const noexcept {
  return !described || cachedRev != descriptorRev;
}

const HValue* HAPRemoteNode::value(uint8_t classId, uint8_t instanceId,
                                   uint8_t portId) const noexcept {
  for (const HAPValueEntry& entry : values) {
    if (entry.classId == classId && entry.instanceId == instanceId &&
        entry.portId == portId) {
      return &entry.value;
    }
  }

  return nullptr;
}

// ---------------------------------------------------------------------------
// Learning
// ---------------------------------------------------------------------------

HAPRemoteNode* HAPModel::findMutable(const HAPPath& path) noexcept {
  for (HAPRemoteNode& node : nodes_) {
    if (node.path == path) {
      return &node;
    }
  }

  return nullptr;
}

HAPRemoteNode* HAPModel::observe(const HAPPath& path) noexcept {
  bool isNew = false;
  HAPRemoteNode* node = touch(path, isNew);

  if (node != nullptr) {
    announceIfNew(isNew, *node);
  }

  return node;
}

HAPRemoteNode* HAPModel::touch(const HAPPath& path, bool& isNew) noexcept {
  isNew = false;

  HAPRemoteNode* node = findMutable(path);
  const uint32_t now = HSystemUtils::millis();

  if (node != nullptr) {
    const bool wasOffline = !node->online;

    node->lastHeardMs = now;
    node->online = true;

    if (wasOffline && onPresenceChanged_.is_valid()) {
      onPresenceChanged_(*node);
    }

    return node;
  }

  if (nodes_.full()) {
    HWarning("no room for %s - the model holds %u nodes",
             path.toString().c_str(), HAP_MODEL_MAX_NODES);
    return nullptr;
  }

  HAPRemoteNode fresh;
  fresh.path = path;
  fresh.lastHeardMs = now;
  fresh.online = true;

  nodes_.push_back(fresh);
  isNew = true;

  return &nodes_.back();
}

void HAPModel::announceIfNew(bool isNew, const HAPRemoteNode& node) noexcept {
  if (!isNew) {
    return;
  }

  // Fired HERE - after the caller has filled the record in - and not from
  // touch(), which is where it used to be. A newly created entry holds nothing
  // but its path, so a hook called at that moment saw an all-zero MAC, no
  // capabilities and an interval of zero, and then the real values were written
  // behind its back. On the bench that printed
  // `NODE 1 = 00:00:00:00:00:00 Sensor [-] every 0s` for a device whose MAC and
  // capabilities had just arrived in the very message being handled.
  //
  // It matters most for exactly the case it broke: ChildAttached is the only
  // message that carries a MAC, so it is the one a user interface needs in
  // order to say which device appeared.
  HInfo("discovered %s", node.path.toString().c_str());

  if (onDiscovered_.is_valid()) {
    onDiscovered_(node);
  }
}

HAPRemoteNode* HAPModel::noteReport(const HAPPath& path,
                                    const HAPReport& report) noexcept {
  bool isNew = false;
  HAPRemoteNode* node = touch(path, isNew);
  if (node == nullptr) {
    return nullptr;
  }

  node->descriptorRev = report.descriptorRev;

  // Replaced rather than merged: a report is the node's whole current state for
  // the ports it carries, and merging would leave a value from ten minutes ago
  // sitting next to one from now with nothing to tell them apart.
  node->values.clear();

  for (const HAPValueEntry& entry : report.entries) {
    if (node->values.full()) {
      break;
    }

    node->values.push_back(entry);
  }

  announceIfNew(isNew, *node);
  return node;
}

HAPRemoteNode* HAPModel::noteDescribe(
    const HAPPath& path, const HAPDescribeResponse& response) noexcept {
  bool isNew = false;
  HAPRemoteNode* node = touch(path, isNew);
  if (node == nullptr) {
    return nullptr;
  }

  node->descriptorRev = response.descriptorRev;
  node->name = response.nodeName;

  if (response.pageIndex == 0) {
    node->instances.clear();
    node->described = false;
  }

  for (const HAPInstanceDescriptor& instance : response.instances) {
    if (node->instances.full()) {
      break;
    }

    node->instances.push_back(instance);
  }

  // Only a complete descriptor counts. Marking a half-arrived one as current
  // would stop the model asking for the rest of it.
  if (response.pageIndex + 1 >= response.pageCount) {
    node->described = true;
    node->cachedRev = response.descriptorRev;
  }

  announceIfNew(isNew, *node);
  return node;
}

HAPRemoteNode* HAPModel::notePong(const HAPPath& path,
                                  const HAPPong& pong) noexcept {
  bool isNew = false;
  HAPRemoteNode* node = touch(path, isNew);
  if (node == nullptr) {
    return nullptr;
  }

  node->descriptorRev = pong.descriptorRev;

  announceIfNew(isNew, *node);
  return node;
}

HAPRemoteNode* HAPModel::notePolicy(
    const HAPPath& path, const HAPSetPolicyResponse& response) noexcept {
  bool isNew = false;
  HAPRemoteNode* node = touch(path, isNew);
  if (node == nullptr) {
    return nullptr;
  }

  // Only when the node agreed. A refusal says nothing about what it will do, and
  // adopting the interval from one would be believing a number nobody promised.
  if (response.result == HAPResult::Ok) {
    node->reportIntervalSec = response.intervalSec;
  }

  announceIfNew(isNew, *node);
  return node;
}

HAPRemoteNode* HAPModel::noteChildAttached(
    const HAPPath& parentPath, const HAPChildAttached& attached) noexcept {
  HAPPath path = parentPath;

  if (!path.append(attached.childIndex)) {
    HWarning("%s + child %u is deeper than the tree may be",
             parentPath.toString().c_str(), attached.childIndex);
    return nullptr;
  }

  // ChildAttached carries a bare six bytes; everything above the wire works in
  // addresses that can be compared and printed.
  const HAPMac mac = HAPMac::fromBytes(attached.mac);

  // The same device at a different path is one node re-bound elsewhere, not a
  // second node. Keeping both would show one thing twice, and a link pointing
  // at the old address would keep failing silently.
  const HAPRemoteNode* previous = findByMac(mac);
  if (previous != nullptr && previous->path != path) {
    HInfo("%s moved from %s to %s", mac.toString().c_str(),
          previous->path.toString().c_str(), path.toString().c_str());
    forget(previous->path);
  }

  bool isNew = false;
  HAPRemoteNode* node = touch(path, isNew);
  if (node == nullptr) {
    return nullptr;
  }

  node->mac = mac;
  node->deviceType = attached.deviceType;
  node->capabilities = attached.capabilities;
  node->reportIntervalSec = attached.reportIntervalSec;
  node->descriptorRev = attached.descriptorRev;

  announceIfNew(isNew, *node);
  return node;
}

HAPRemoteNode* HAPModel::restore(const HAPPath& path, const HAPMac& mac,
                                 HAPDeviceType deviceType, uint8_t capabilities,
                                 uint16_t reportIntervalSec) noexcept {
  bool isNew = false;
  HAPRemoteNode* node = touch(path, isNew);
  if (node == nullptr) {
    return nullptr;
  }

  node->mac = mac;
  node->deviceType = deviceType;
  node->capabilities = capabilities;
  node->reportIntervalSec = reportIntervalSec;

  // `described` is left false and `descriptorRev` at zero on purpose: the names
  // and instances are a cache, and a master that has been away should ask for
  // them again rather than show what may have changed while it was gone. The
  // first report will carry the current revision and settle it.
  announceIfNew(isNew, *node);
  return node;
}

// ---------------------------------------------------------------------------
// Querying
// ---------------------------------------------------------------------------

size_t HAPModel::size() const noexcept {
  return nodes_.size();
}

bool HAPModel::isFull() const noexcept {
  return nodes_.full();
}

const HAPRemoteNode* HAPModel::at(size_t position) const noexcept {
  return position < nodes_.size() ? &nodes_[position] : nullptr;
}

const HAPRemoteNode* HAPModel::find(const HAPPath& path) const noexcept {
  for (const HAPRemoteNode& node : nodes_) {
    if (node.path == path) {
      return &node;
    }
  }

  return nullptr;
}

const HAPRemoteNode* HAPModel::findByMac(const HAPMac& mac) const noexcept {
  if (mac.isZero()) {
    // Every node starts with a zero address, so matching on one would return
    // whichever has not been described yet.
    return nullptr;
  }

  for (const HAPRemoteNode& node : nodes_) {
    if (node.mac == mac) {
      return &node;
    }
  }

  return nullptr;
}

bool HAPModel::nextDescribeNeeded(HAPPath& path) const noexcept {
  for (const HAPRemoteNode& node : nodes_) {
    if (node.needsDescribe()) {
      path = node.path;
      return true;
    }
  }

  return false;
}

size_t HAPModel::describeBacklog() const noexcept {
  size_t waiting = 0;

  for (const HAPRemoteNode& node : nodes_) {
    if (node.needsDescribe()) {
      ++waiting;
    }
  }

  return waiting;
}

size_t HAPModel::sweepOffline() noexcept {
  const uint32_t now = HSystemUtils::millis();
  size_t changed = 0;

  for (HAPRemoteNode& node : nodes_) {
    if (!node.online) {
      continue;
    }

    // A node reporting only on change has promised nothing, so its silence
    // means nothing. Calling it offline would be inventing a fault; a Ping is
    // how to ask.
    if (node.reportIntervalSec == 0) {
      continue;
    }

    const uint32_t allowed = static_cast<uint32_t>(node.reportIntervalSec) *
                             1000u * HAP_OFFLINE_INTERVALS;

    if (now - node.lastHeardMs < allowed) {
      continue;
    }

    node.online = false;
    ++changed;

    HWarning("%s has missed %u reports", node.path.toString().c_str(),
             HAP_OFFLINE_INTERVALS);

    if (onPresenceChanged_.is_valid()) {
      onPresenceChanged_(node);
    }
  }

  return changed;
}

bool HAPModel::forget(const HAPPath& path) noexcept {
  for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
    if (it->path == path) {
      nodes_.erase(it);
      return true;
    }
  }

  return false;
}

void HAPModel::clear() noexcept {
  nodes_.clear();
}

void HAPModel::onDiscovered(DiscoveredHook hook) noexcept {
  onDiscovered_ = hook;
}

void HAPModel::onPresenceChanged(PresenceHook hook) noexcept {
  onPresenceChanged_ = hook;
}
