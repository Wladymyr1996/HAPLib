#include <HAPMessages/HAPBindMessages.hpp>

// ---------------------------------------------------------------------------
// HAPBindAnnounce
// ---------------------------------------------------------------------------

void HAPBindAnnounce::encode(HAPWriter& writer) const noexcept {
  writer.u8(static_cast<uint8_t>(deviceType));
  writer.u8(capabilities);
  writer.u16(reportIntervalSec);
  writer.u16(descriptorRev);
  writer.u8(instanceCount);
  writer.u8(pageIndex);
  writer.u8(pageCount);
  writer.name(nodeName);

  for (const HAPInstanceDescriptor& instance : instances) {
    instance.encode(writer);
  }
}

bool HAPBindAnnounce::decode(HAPReader& reader) noexcept {
  deviceType = static_cast<HAPDeviceType>(reader.u8());
  capabilities = reader.u8();
  reportIntervalSec = reader.u16();
  descriptorRev = reader.u16();
  instanceCount = reader.u8();
  pageIndex = reader.u8();
  pageCount = reader.u8();
  nodeName = reader.name();

  instances.clear();

  // The descriptors run to the end of the payload: how many there are is a
  // property of the frame's length, not a field, which is what lets a page hold
  // as many as happen to fit.
  while (reader.ok() && reader.remaining() > 0) {
    if (instances.full()) {
      return false;
    }

    HAPInstanceDescriptor instance;
    if (!instance.decode(reader)) {
      return false;
    }

    instances.push_back(instance);
  }

  return reader.ok();
}

// ---------------------------------------------------------------------------
// HAPBindAccept
// ---------------------------------------------------------------------------

void HAPBindAccept::encode(HAPWriter& writer) const noexcept {
  writer.u8(childIndex);
  writer.u8(channel);
  writer.bytes(linkKey, sizeof(linkKey));
  writer.name(masterName);
}

bool HAPBindAccept::decode(HAPReader& reader) noexcept {
  childIndex = reader.u8();
  channel = reader.u8();

  const uint8_t* key = reader.bytes(sizeof(linkKey));
  if (key == nullptr) {
    return false;
  }

  for (size_t i = 0; i < sizeof(linkKey); ++i) {
    linkKey[i] = key[i];
  }

  masterName = reader.name();

  return reader.ok();
}

// ---------------------------------------------------------------------------
// HAPBindConfirm
// ---------------------------------------------------------------------------

void HAPBindConfirm::encode(HAPWriter& writer) const noexcept {
  writer.u16(descriptorRev);
}

bool HAPBindConfirm::decode(HAPReader& reader) noexcept {
  descriptorRev = reader.u16();
  return reader.ok();
}

// ---------------------------------------------------------------------------
// HAPChildAttached
// ---------------------------------------------------------------------------

void HAPChildAttached::encode(HAPWriter& writer) const noexcept {
  writer.u8(childIndex);
  writer.u8(static_cast<uint8_t>(deviceType));
  writer.u8(capabilities);
  writer.u16(reportIntervalSec);
  writer.u16(descriptorRev);
  writer.bytes(mac, sizeof(mac));
}

bool HAPChildAttached::decode(HAPReader& reader) noexcept {
  childIndex = reader.u8();
  deviceType = static_cast<HAPDeviceType>(reader.u8());
  capabilities = reader.u8();
  reportIntervalSec = reader.u16();
  descriptorRev = reader.u16();

  const uint8_t* address = reader.bytes(sizeof(mac));
  if (address == nullptr) {
    return false;
  }

  for (size_t i = 0; i < sizeof(mac); ++i) {
    mac[i] = address[i];
  }

  return reader.ok();
}
