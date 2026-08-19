#include <HAPMessages/HAPMessageParts.hpp>

// ---------------------------------------------------------------------------
// HAPInstanceDescriptor
// ---------------------------------------------------------------------------

void HAPInstanceDescriptor::encode(HAPWriter& writer) const noexcept {
  writer.u8(classId);
  writer.u8(instanceId);
  writer.u8(flags);
  writer.u8(valueType);
  writer.name(name);
}

bool HAPInstanceDescriptor::decode(HAPReader& reader) noexcept {
  classId = reader.u8();
  instanceId = reader.u8();
  flags = reader.u8();
  valueType = reader.u8();
  name = reader.name();

  return reader.ok();
}

size_t HAPInstanceDescriptor::encodedSize() const noexcept {
  return 4 + 1 + name.size();
}

// ---------------------------------------------------------------------------
// HAPValueEntry
// ---------------------------------------------------------------------------

HAPValueEntry::HAPValueEntry(uint8_t classId, uint8_t instanceId, uint8_t portId,
                             const HValue& value) noexcept
    : classId(classId), instanceId(instanceId), portId(portId), value(value) {}

void HAPValueEntry::setValue(const HValue& newValue) noexcept {
  // Destroy and re-construct rather than assign. HValue fixes its type at
  // construction and coerces on assignment, so `value = newValue` would keep
  // whatever type this entry already had - turning a fresh 21.5 into a Null on
  // a default-constructed entry.
  HAPAssign(value, newValue);
}

void HAPValueEntry::encode(HAPWriter& writer) const noexcept {
  writer.u8(classId);
  writer.u8(instanceId);
  writer.u8(portId);
  writer.value(value);
}

HAPValueEntry HAPValueEntry::decode(HAPReader& reader) noexcept {
  const uint8_t classId = reader.u8();
  const uint8_t instanceId = reader.u8();
  const uint8_t portId = reader.u8();

  // Built by the constructor, so the HValue is copy-CONSTRUCTED and keeps the
  // type it was decoded as.
  return HAPValueEntry(classId, instanceId, portId, reader.value());
}

// ---------------------------------------------------------------------------
// HAPPortRef
// ---------------------------------------------------------------------------

void HAPPortRef::encode(HAPWriter& writer) const noexcept {
  writer.u8(path.length());
  writer.bytes(path.bytes(), HAP_MAX_DEPTH);
  writer.u8(classId);
  writer.u8(instanceId);
  writer.u8(portId);
}

bool HAPPortRef::decode(HAPReader& reader) noexcept {
  const uint8_t length = reader.u8();
  const uint8_t* hops = reader.bytes(HAP_MAX_DEPTH);

  if (hops == nullptr) {
    return false;
  }

  path = HAPPath::fromBytes(hops, length);

  // fromBytes yields an empty path for anything illegal, so a non-zero length
  // that came back empty means the reference addresses nothing real.
  if (path.length() != length) {
    return false;
  }

  classId = reader.u8();
  instanceId = reader.u8();
  portId = reader.u8();

  return reader.ok();
}

bool HAPPortRef::operator==(const HAPPortRef& other) const noexcept {
  return path == other.path && classId == other.classId &&
         instanceId == other.instanceId && portId == other.portId;
}

bool HAPPortRef::operator!=(const HAPPortRef& other) const noexcept {
  return !(*this == other);
}
