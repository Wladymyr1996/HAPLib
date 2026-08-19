#define HLOG_MODULE_NAME "HapLinks"

#include <HAPLinks/HAPLinks.hpp>

#include <HAPClasses/HAPClasses.hpp>
#include <HLog/HLog.hpp>

HAPLinks::HAPLinks(const HAPNode& node) noexcept : node_(node) {}

HAPResult HAPLinks::validateLocal(const HAPPortRef& reference,
                                  HAPPortDirection direction) const noexcept {
  if (!reference.path.isEmpty()) {
    // Somewhere below. This node knows the CLASS rules for it - checked by the
    // caller through the class table - but not whether that node actually has
    // the instance, so it takes the configuring node's word for that.
    return HAPResult::Ok;
  }

  const HAPInstance* instance =
      node_.instance(reference.classId, reference.instanceId);

  if (instance == nullptr) {
    return HAPResult::NoSuchClass;
  }

  const HAPClassSpec* spec = instance->spec();
  if (spec == nullptr || spec->find(direction, reference.portId) == nullptr) {
    return HAPResult::NoSuchPort;
  }

  return HAPResult::Ok;
}

bool HAPLinks::inputIsTaken(const HAPPortRef& destination,
                            uint8_t exceptId) const noexcept {
  for (const HAPLinkSpec& link : links_) {
    if (link.linkId != exceptId && link.destination == destination) {
      return true;
    }
  }

  return false;
}

HAPResult HAPLinks::set(const HAPLinkSpec& spec) noexcept {
  if (spec.linkId >= HAP_MAX_LINKS) {
    return HAPResult::BadRequest;
  }

  // The class-level check first, because it is true of any node carrying those
  // classes: do the ports exist, do they face the right way, and do they carry
  // the same quantity kind. That last one is the whole reason kinds exist -
  // both ends of a hygrometer-into-a-setpoint are floats in a plausible range.
  const HAPResult classes = HAPClasses::validateLink(
      spec.source.classId, spec.source.portId, spec.destination.classId,
      spec.destination.portId);

  if (classes != HAPResult::Ok) {
    return classes;
  }

  const HAPResult source = validateLocal(spec.source, HAPPortDirection::Out);
  if (source != HAPResult::Ok) {
    return source;
  }

  const HAPResult destination =
      validateLocal(spec.destination, HAPPortDirection::In);
  if (destination != HAPResult::Ok) {
    return destination;
  }

  // One driver per input. Two would be last-writer-wins, which shows up as an
  // input that flickers between two sources and no way to see why.
  if (inputIsTaken(spec.destination, spec.linkId)) {
    return HAPResult::InputBusy;
  }

  for (HAPLinkSpec& existing : links_) {
    if (existing.linkId == spec.linkId) {
      existing = spec;
      return HAPResult::Ok;
    }
  }

  if (links_.full()) {
    return HAPResult::NoLinkSlot;
  }

  links_.push_back(spec);

  HInfo("link %u: %s/%u.%u.%u -> %s/%u.%u.%u", spec.linkId,
        spec.source.path.toString().c_str(), spec.source.classId,
        spec.source.instanceId, spec.source.portId,
        spec.destination.path.toString().c_str(), spec.destination.classId,
        spec.destination.instanceId, spec.destination.portId);

  return HAPResult::Ok;
}

HAPResult HAPLinks::clear(uint8_t linkId) noexcept {
  if (linkId == HAPClearLinkRequest::kAllLinks) {
    links_.clear();
    return HAPResult::Ok;
  }

  for (auto it = links_.begin(); it != links_.end(); ++it) {
    if (it->linkId == linkId) {
      links_.erase(it);
      return HAPResult::Ok;
    }
  }

  return HAPResult::NoLinkSlot;
}

size_t HAPLinks::size() const noexcept {
  return links_.size();
}

bool HAPLinks::isFull() const noexcept {
  return links_.full();
}

const HAPLinkSpec* HAPLinks::at(size_t position) const noexcept {
  return position < links_.size() ? &links_[position] : nullptr;
}

const HAPLinkSpec* HAPLinks::find(uint8_t linkId) const noexcept {
  for (const HAPLinkSpec& link : links_) {
    if (link.linkId == linkId) {
      return &link;
    }
  }

  return nullptr;
}

uint8_t HAPLinks::pageCount() const noexcept {
  const size_t perPage =
      (HAP_MAX_PAYLOAD_SIZE - HAPListLinksResponse::kFixedSize) /
      HAPLinkSpec::kEncodedSize;

  if (links_.empty() || perPage == 0) {
    return 1;
  }

  return static_cast<uint8_t>((links_.size() + perPage - 1) / perPage);
}

bool HAPLinks::fillList(HAPListLinksResponse& response,
                        uint8_t page) const noexcept {
  const size_t perPage =
      (HAP_MAX_PAYLOAD_SIZE - HAPListLinksResponse::kFixedSize) /
      HAPLinkSpec::kEncodedSize;

  const uint8_t pages = pageCount();
  if (page >= pages || perPage == 0) {
    return false;
  }

  response.count = static_cast<uint8_t>(links_.size());
  response.pageIndex = page;
  response.pageCount = pages;
  response.links.clear();

  const size_t first = static_cast<size_t>(page) * perPage;

  for (size_t i = first; i < links_.size() && i < first + perPage; ++i) {
    response.links.push_back(links_[i]);
  }

  return true;
}

void HAPLinks::onDeliver(DeliveryHook hook) noexcept {
  onDeliver_ = hook;
}

bool HAPLinks::watches(const HAPPath& sourcePath) const noexcept {
  for (const HAPLinkSpec& link : links_) {
    if (link.source.path == sourcePath) {
      return true;
    }
  }

  return false;
}

size_t HAPLinks::deliver(const HAPPath& sourcePath,
                         const HAPValueEntry& entry) noexcept {
  size_t fired = 0;

  for (const HAPLinkSpec& link : links_) {
    // The comparison the whole addressing design was built to make possible:
    // what a climbing frame carries is already the downward path from here.
    if (link.source.path != sourcePath || link.source.classId != entry.classId ||
        link.source.instanceId != entry.instanceId ||
        link.source.portId != entry.portId) {
      continue;
    }

    ++fired;

    if (onDeliver_.is_valid()) {
      onDeliver_(link.destination, entry.value);
    }
  }

  return fired;
}

size_t HAPLinks::deliver(const HAPPath& sourcePath,
                         const HAPReport& report) noexcept {
  size_t fired = 0;

  for (const HAPValueEntry& entry : report.entries) {
    fired += deliver(sourcePath, entry);
  }

  return fired;
}
