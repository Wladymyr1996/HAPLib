#include <HAPNode/HAPNode.hpp>

#include <HAPCrc16/HAPCrc16.hpp>

void HAPNode::begin(HAPDeviceType deviceType, uint8_t capabilities,
                    const HAPName& name) noexcept {
  deviceType_ = deviceType;
  capabilities_ = capabilities;
  name_ = name;
}

HAPDeviceType HAPNode::deviceType() const noexcept {
  return deviceType_;
}

uint8_t HAPNode::capabilities() const noexcept {
  return capabilities_;
}

bool HAPNode::isBatteryPowered() const noexcept {
  return (capabilities_ & HAPCaps::BatteryPowered) != 0;
}

const HAPName& HAPNode::name() const noexcept {
  return name_;
}

void HAPNode::setName(const HAPName& newName) noexcept {
  name_ = newName;
}

uint16_t HAPNode::reportIntervalSec() const noexcept {
  return reportIntervalSec_;
}

void HAPNode::setReportIntervalSec(uint16_t seconds) noexcept {
  reportIntervalSec_ = seconds;
}

HAPInstance* HAPNode::addInstance(HAPClassId classId, const HAPName& name) noexcept {
  if (instances_.full()) {
    return nullptr;
  }

  // The instance id is the position it is added at, which makes it unique within
  // the node and stable for as long as the firmware is - both of which a master
  // caching a descriptor depends on.
  const uint8_t instanceId = static_cast<uint8_t>(instances_.size());

  HAPInstance instance;
  if (!instance.configure(static_cast<uint8_t>(classId), instanceId, name)) {
    return nullptr;
  }

  instances_.push_back(instance);
  return &instances_.back();
}

size_t HAPNode::instanceCount() const noexcept {
  return instances_.size();
}

HAPInstance* HAPNode::instance(uint8_t classId, uint8_t instanceId) noexcept {
  for (HAPInstance& candidate : instances_) {
    if (candidate.classId() == classId && candidate.instanceId() == instanceId) {
      return &candidate;
    }
  }

  return nullptr;
}

const HAPInstance* HAPNode::instance(uint8_t classId,
                                     uint8_t instanceId) const noexcept {
  for (const HAPInstance& candidate : instances_) {
    if (candidate.classId() == classId && candidate.instanceId() == instanceId) {
      return &candidate;
    }
  }

  return nullptr;
}

HAPInstance* HAPNode::instanceAt(size_t position) noexcept {
  return position < instances_.size() ? &instances_[position] : nullptr;
}

const HAPInstance* HAPNode::instanceAt(size_t position) const noexcept {
  return position < instances_.size() ? &instances_[position] : nullptr;
}

// ---------------------------------------------------------------------------
// The revision
// ---------------------------------------------------------------------------

uint16_t HAPNode::descriptorRev() const noexcept {
  uint16_t crc = kHAPCrc16Init;

  // Everything a master would cache, in a fixed order, so the same node always
  // produces the same number - on any board, after any reboot, with nothing
  // stored anywhere. The device type is in it because a slot whose device was
  // swapped for a different KIND of device must not keep the old revision.
  const uint8_t header[] = {static_cast<uint8_t>(deviceType_), capabilities_};
  crc = HAPCrc16Update(crc, header, sizeof(header));
  crc = HAPCrc16Update(crc, reinterpret_cast<const uint8_t*>(name_.data()),
                       name_.size());

  for (const HAPInstance& instance : instances_) {
    const HAPInstanceDescriptor descriptor = instance.describe();
    const uint8_t fields[] = {descriptor.classId, descriptor.instanceId,
                              descriptor.flags, descriptor.valueType};

    crc = HAPCrc16Update(crc, fields, sizeof(fields));
    crc = HAPCrc16Update(
        crc, reinterpret_cast<const uint8_t*>(descriptor.name.data()),
        descriptor.name.size());
  }

  return crc;
}

// ---------------------------------------------------------------------------
// Paging
// ---------------------------------------------------------------------------

HAPNode::Page HAPNode::pageAt(uint8_t page, size_t fixedSize) const noexcept {
  Page result;

  // The node's name repeats on every page, so a page can be parsed on its own -
  // and so it has to be paid for on every page too.
  const size_t overhead = fixedSize + 1 + name_.size();
  const size_t budget = HAP_MAX_PAYLOAD_SIZE - overhead;

  size_t position = 0;
  uint8_t pageIndex = 0;

  while (true) {
    size_t used = 0;
    size_t count = 0;

    while (position + count < instances_.size()) {
      const size_t size = instances_[position + count].describe().encodedSize();
      if (used + size > budget) {
        break;
      }

      used += size;
      ++count;
    }

    if (pageIndex == page) {
      result.first = position;
      result.count = count;
    }

    position += count;

    // A descriptor too large for even an empty page would loop forever. It
    // cannot happen with the standard classes - the widest is 36 bytes against
    // a budget over 200 - but a bug elsewhere must not hang a node.
    if (count == 0) {
      break;
    }

    ++pageIndex;

    if (position >= instances_.size()) {
      break;
    }
  }

  result.total = pageIndex == 0 ? 1 : pageIndex;
  return result;
}

uint8_t HAPNode::pageCount() const noexcept {
  return pageAt(0, HAPDescribeResponse::kFixedSize).total;
}

bool HAPNode::fillDescribe(HAPDescribeResponse& response,
                           uint8_t page) const noexcept {
  const Page slice = pageAt(page, HAPDescribeResponse::kFixedSize);

  if (page >= slice.total) {
    return false;
  }

  response.descriptorRev = descriptorRev();
  response.instanceCount = static_cast<uint8_t>(instances_.size());
  response.pageIndex = page;
  response.pageCount = slice.total;
  response.nodeName = name_;
  response.instances.clear();

  for (size_t i = 0; i < slice.count; ++i) {
    response.instances.push_back(instances_[slice.first + i].describe());
  }

  return true;
}

bool HAPNode::fillAnnounce(HAPBindAnnounce& announce, uint8_t page) const noexcept {
  const Page slice = pageAt(page, HAPBindAnnounce::kFixedSize);

  if (page >= slice.total) {
    return false;
  }

  announce.deviceType = deviceType_;
  announce.capabilities = capabilities_;
  announce.reportIntervalSec = reportIntervalSec_;
  announce.descriptorRev = descriptorRev();
  announce.instanceCount = static_cast<uint8_t>(instances_.size());
  announce.pageIndex = page;
  announce.pageCount = slice.total;
  announce.nodeName = name_;
  announce.instances.clear();

  for (size_t i = 0; i < slice.count; ++i) {
    announce.instances.push_back(instances_[slice.first + i].describe());
  }

  return true;
}

// ---------------------------------------------------------------------------
// Answering
// ---------------------------------------------------------------------------

void HAPNode::fillReport(HAPReport& report) const noexcept {
  report.descriptorRev = descriptorRev();
  report.entries.clear();

  for (const HAPInstance& instance : instances_) {
    const HAPClassSpec* classSpec = instance.spec();
    if (classSpec == nullptr) {
      continue;
    }

    for (uint8_t i = 0; i < classSpec->portCount; ++i) {
      const HAPPortSpec& portSpec = classSpec->ports[i];
      if (portSpec.direction != HAPPortDirection::Out) {
        continue;
      }

      if (report.entries.full()) {
        return;
      }

      report.entries.push_back(HAPValueEntry(instance.classId(),
                                             instance.instanceId(),
                                             portSpec.portId,
                                             instance.read(portSpec.portId)));
    }
  }
}

HAPResult HAPNode::read(const HAPReadRequest& request,
                        HAPReport& report) const noexcept {
  report.descriptorRev = descriptorRev();
  report.entries.clear();

  const bool anyClass = request.classId == HAPReadRequest::kAnyClass;
  const bool anyInstance = request.instanceId == HAPReadRequest::kAnyInstance;
  const bool anyPort = request.portId == HAPReadRequest::kAnyPort;

  bool matchedInstance = false;

  for (const HAPInstance& instance : instances_) {
    if (!anyClass && instance.classId() != request.classId) {
      continue;
    }

    if (!anyInstance && instance.instanceId() != request.instanceId) {
      continue;
    }

    matchedInstance = true;

    const HAPClassSpec* classSpec = instance.spec();
    if (classSpec == nullptr) {
      continue;
    }

    bool matchedPort = false;

    for (uint8_t i = 0; i < classSpec->portCount; ++i) {
      const HAPPortSpec& portSpec = classSpec->ports[i];

      if (portSpec.direction != HAPPortDirection::Out) {
        continue;
      }

      if (!anyPort && portSpec.portId != request.portId) {
        continue;
      }

      matchedPort = true;

      if (report.entries.full()) {
        return HAPResult::Ok;
      }

      report.entries.push_back(HAPValueEntry(instance.classId(),
                                             instance.instanceId(),
                                             portSpec.portId,
                                             instance.read(portSpec.portId)));
    }

    if (!matchedPort && !anyPort) {
      return HAPResult::NoSuchPort;
    }
  }

  // A wildcard matching nothing is an answer - "I have none of those" - but a
  // request naming one specific instance that is not here is a mistake worth
  // reporting.
  if (!matchedInstance && !(anyClass && anyInstance)) {
    return HAPResult::NoSuchClass;
  }

  return HAPResult::Ok;
}

HAPWriteResponse HAPNode::write(const HAPWriteRequest& request) noexcept {
  HAPInstance* target = instance(request.classId, request.instanceId);

  if (target == nullptr) {
    return HAPWriteResponse(HAPResult::NoSuchClass, request.classId,
                            request.instanceId, request.portId, HValue());
  }

  const HAPResult result = target->write(request.portId, request.value);

  // The response carries what was actually TAKEN, read back from the port
  // rather than echoed from the request - so an instance that clamped or
  // ignored a value says so, and a master never shows a state the device is
  // not in.
  return HAPWriteResponse(result, request.classId, request.instanceId,
                          request.portId, target->input(request.portId));
}

HAPSetNameResponse HAPNode::rename(const HAPSetNameRequest& request) noexcept {
  HAPSetNameResponse response;

  if (request.target == HAPSetNameRequest::Target::Node) {
    setName(request.name);
    response.result = HAPResult::Ok;
  } else {
    HAPInstance* target = instance(request.classId, request.instanceId);

    if (target == nullptr) {
      response.result = HAPResult::NoSuchClass;
    } else {
      target->setName(request.name);
      response.result = HAPResult::Ok;
    }
  }

  // Recomputed after the change, whether or not it succeeded: the number is the
  // truth about what this node now is, and a master compares it against what it
  // cached rather than trusting the result code.
  response.descriptorRev = descriptorRev();
  return response;
}
