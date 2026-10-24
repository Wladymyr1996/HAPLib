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

HAPRemoteNode* HAPModel::findMutable(const HAPNodeId& id) noexcept {
  for (HAPRemoteNode& node : nodes_) {
    if (node.id == id) {
      return &node;
    }
  }

  return nullptr;
}

HAPRemoteNode* HAPModel::observe(const HAPNodeId& id) noexcept {
  bool isNew = false;
  HAPRemoteNode* node = touch(id, isNew);

  if (node != nullptr) {
    announceIfNew(isNew, *node);
  }

  return node;
}

HAPRemoteNode* HAPModel::touch(const HAPNodeId& id, bool& isNew) noexcept {
  isNew = false;

  // A node with no id cannot be told from any other; decode() already refuses
  // a frame without a source, so this is only a guard.
  if (id.isZero()) {
    return nullptr;
  }

  HAPRemoteNode* node = findMutable(id);
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
             id.toString().c_str(), HAP_MODEL_MAX_NODES);
    return nullptr;
  }

  HAPRemoteNode fresh;
  fresh.id = id;
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
  // but its id, so a hook called at that moment saw an all-zero MAC, no
  // capabilities and an interval of zero, and then the real values were written
  // behind its back. On the bench that printed
  // `NODE 1 = 00:00:00:00:00:00 Sensor [-] every 0s` for a device whose MAC and
  // capabilities had just arrived in the very message being handled.
  //
  // It matters most for exactly the case it broke: ChildAttached is the only
  // message that carries a MAC, so it is the one a user interface needs in
  // order to say which device appeared.
  HInfo("discovered %s", node.id.toString().c_str());

  if (onDiscovered_.is_valid()) {
    onDiscovered_(node);
  }
}

HAPRemoteNode* HAPModel::noteReport(const HAPNodeId& id,
                                    const HAPReport& report) noexcept {
  bool isNew = false;
  HAPRemoteNode* node = touch(id, isNew);
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
    const HAPNodeId& id, const HAPDescribeResponse& response) noexcept {
  bool isNew = false;
  HAPRemoteNode* node = touch(id, isNew);
  if (node == nullptr) {
    return nullptr;
  }

  node->descriptorRev = response.descriptorRev;
  node->name = response.nodeName;

  // What it reports at now - the one way a master that missed the change
  // learns it. An older node's pages carry nothing, and change nothing.
  if (response.hasInterval) {
    node->reportIntervalSec = response.reportIntervalSec;
  }

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

HAPRemoteNode* HAPModel::notePong(const HAPNodeId& id,
                                  const HAPPong& pong) noexcept {
  bool isNew = false;
  HAPRemoteNode* node = touch(id, isNew);
  if (node == nullptr) {
    return nullptr;
  }

  node->descriptorRev = pong.descriptorRev;

  announceIfNew(isNew, *node);
  return node;
}

HAPRemoteNode* HAPModel::notePolicy(
    const HAPNodeId& id, const HAPSetPolicyResponse& response) noexcept {
  bool isNew = false;
  HAPRemoteNode* node = touch(id, isNew);
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
    const HAPChildAttached& attached) noexcept {
  // ChildAttached carries a bare six bytes; everything above the wire works in
  // ids that can be compared and printed. The id is the key, so a device that
  // turns up under a new parent is the same entry, not a second one.
  const HAPNodeId id = HAPNodeId::fromMac(attached.mac);

  bool isNew = false;
  HAPRemoteNode* node = touch(id, isNew);
  if (node == nullptr) {
    return nullptr;
  }

  node->announced = true;
  node->deviceType = attached.deviceType;
  node->capabilities = attached.capabilities;
  node->reportIntervalSec = attached.reportIntervalSec;
  node->descriptorRev = attached.descriptorRev;

  announceIfNew(isNew, *node);
  return node;
}

HAPRemoteNode* HAPModel::restore(const HAPNodeId& id, HAPDeviceType deviceType,
                                 uint8_t capabilities,
                                 uint16_t reportIntervalSec) noexcept {
  bool isNew = false;
  HAPRemoteNode* node = touch(id, isNew);
  if (node == nullptr) {
    return nullptr;
  }

  node->announced = true;
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

const HAPRemoteNode* HAPModel::find(const HAPNodeId& id) const noexcept {
  for (const HAPRemoteNode& node : nodes_) {
    if (node.id == id) {
      return &node;
    }
  }

  return nullptr;
}

bool HAPModel::nextDescribeNeeded(HAPNodeId& id) const noexcept {
  for (const HAPRemoteNode& node : nodes_) {
    if (node.needsDescribe()) {
      id = node.id;
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

    HWarning("%s has missed %u reports", node.id.toString().c_str(),
             HAP_OFFLINE_INTERVALS);

    if (onPresenceChanged_.is_valid()) {
      onPresenceChanged_(node);
    }
  }

  return changed;
}

bool HAPModel::forget(const HAPNodeId& id) noexcept {
  for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
    if (it->id == id) {
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
