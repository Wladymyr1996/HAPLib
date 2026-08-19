#include <HAPMessages/HAPDataMessages.hpp>

// ---------------------------------------------------------------------------
// HAPDescribeRequest / HAPDescribeResponse
// ---------------------------------------------------------------------------

void HAPDescribeRequest::encode(HAPWriter& writer) const noexcept {
  writer.u8(fromPage);
}

bool HAPDescribeRequest::decode(HAPReader& reader) noexcept {
  fromPage = reader.u8();
  return reader.ok();
}

void HAPDescribeResponse::encode(HAPWriter& writer) const noexcept {
  writer.u16(descriptorRev);
  writer.u8(instanceCount);
  writer.u8(pageIndex);
  writer.u8(pageCount);
  writer.name(nodeName);

  for (const HAPInstanceDescriptor& instance : instances) {
    instance.encode(writer);
  }
}

bool HAPDescribeResponse::decode(HAPReader& reader) noexcept {
  descriptorRev = reader.u16();
  instanceCount = reader.u8();
  pageIndex = reader.u8();
  pageCount = reader.u8();
  nodeName = reader.name();

  instances.clear();

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
// HAPReport
// ---------------------------------------------------------------------------

void HAPReport::encode(HAPWriter& writer) const noexcept {
  writer.u16(descriptorRev);
  writer.u8(static_cast<uint8_t>(entries.size()));

  for (const HAPValueEntry& entry : entries) {
    entry.encode(writer);
  }
}

bool HAPReport::decode(HAPReader& reader) noexcept {
  descriptorRev = reader.u16();
  const uint8_t count = reader.u8();

  if (!reader.ok()) {
    return false;
  }

  // Unlike a descriptor page, the count is stated - so a frame promising more
  // entries than this build can hold is refused outright rather than parsed
  // into a silently short report.
  if (count > entries.max_size()) {
    return false;
  }

  entries.clear();

  for (uint8_t i = 0; i < count; ++i) {
    entries.push_back(HAPValueEntry::decode(reader));

    if (!reader.ok()) {
      return false;
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// HAPReadRequest
// ---------------------------------------------------------------------------

void HAPReadRequest::encode(HAPWriter& writer) const noexcept {
  writer.u8(classId);
  writer.u8(instanceId);
  writer.u8(portId);
}

bool HAPReadRequest::decode(HAPReader& reader) noexcept {
  classId = reader.u8();
  instanceId = reader.u8();
  portId = reader.u8();
  return reader.ok();
}

// ---------------------------------------------------------------------------
// HAPWriteRequest / HAPWriteResponse
// ---------------------------------------------------------------------------

HAPWriteRequest::HAPWriteRequest(uint8_t classId, uint8_t instanceId,
                                 uint8_t portId, const HValue& value) noexcept
    : classId(classId), instanceId(instanceId), portId(portId), value(value) {}

void HAPWriteRequest::setValue(const HValue& newValue) noexcept {
  HAPAssign(value, newValue);
}

void HAPWriteRequest::encode(HAPWriter& writer) const noexcept {
  writer.u8(classId);
  writer.u8(instanceId);
  writer.u8(portId);
  writer.value(value);
}

HAPWriteRequest HAPWriteRequest::decode(HAPReader& reader) noexcept {
  const uint8_t classId = reader.u8();
  const uint8_t instanceId = reader.u8();
  const uint8_t portId = reader.u8();

  return HAPWriteRequest(classId, instanceId, portId, reader.value());
}

HAPWriteResponse::HAPWriteResponse(HAPResult result, uint8_t classId,
                                   uint8_t instanceId, uint8_t portId,
                                   const HValue& value) noexcept
    : result(result),
      classId(classId),
      instanceId(instanceId),
      portId(portId),
      value(value) {}

void HAPWriteResponse::setValue(const HValue& newValue) noexcept {
  HAPAssign(value, newValue);
}

void HAPWriteResponse::encode(HAPWriter& writer) const noexcept {
  writer.u8(static_cast<uint8_t>(result));
  writer.u8(classId);
  writer.u8(instanceId);
  writer.u8(portId);
  writer.value(value);
}

HAPWriteResponse HAPWriteResponse::decode(HAPReader& reader) noexcept {
  const HAPResult result = static_cast<HAPResult>(reader.u8());
  const uint8_t classId = reader.u8();
  const uint8_t instanceId = reader.u8();
  const uint8_t portId = reader.u8();

  return HAPWriteResponse(result, classId, instanceId, portId, reader.value());
}

// ---------------------------------------------------------------------------
// HAPSetPolicyRequest / HAPSetPolicyResponse
// ---------------------------------------------------------------------------

HAPSetPolicyRequest::HAPSetPolicyRequest(uint8_t classId, uint8_t instanceId,
                                         uint8_t portId, uint16_t intervalSec,
                                         const HValue& deadband) noexcept
    : classId(classId),
      instanceId(instanceId),
      portId(portId),
      intervalSec(intervalSec),
      deadband(deadband) {}

void HAPSetPolicyRequest::setDeadband(const HValue& newDeadband) noexcept {
  HAPAssign(deadband, newDeadband);
}

void HAPSetPolicyRequest::encode(HAPWriter& writer) const noexcept {
  writer.u8(classId);
  writer.u8(instanceId);
  writer.u8(portId);
  writer.u16(intervalSec);
  writer.value(deadband);
}

HAPSetPolicyRequest HAPSetPolicyRequest::decode(HAPReader& reader) noexcept {
  const uint8_t classId = reader.u8();
  const uint8_t instanceId = reader.u8();
  const uint8_t portId = reader.u8();
  const uint16_t intervalSec = reader.u16();

  return HAPSetPolicyRequest(classId, instanceId, portId, intervalSec,
                             reader.value());
}

HAPSetPolicyResponse::HAPSetPolicyResponse(HAPResult result, uint8_t classId,
                                           uint8_t instanceId, uint8_t portId,
                                           uint16_t intervalSec,
                                           const HValue& deadband) noexcept
    : result(result),
      classId(classId),
      instanceId(instanceId),
      portId(portId),
      intervalSec(intervalSec),
      deadband(deadband) {}

void HAPSetPolicyResponse::setDeadband(const HValue& newDeadband) noexcept {
  HAPAssign(deadband, newDeadband);
}

void HAPSetPolicyResponse::encode(HAPWriter& writer) const noexcept {
  writer.u8(static_cast<uint8_t>(result));
  writer.u8(classId);
  writer.u8(instanceId);
  writer.u8(portId);
  writer.u16(intervalSec);
  writer.value(deadband);
}

HAPSetPolicyResponse HAPSetPolicyResponse::decode(HAPReader& reader) noexcept {
  const HAPResult result = static_cast<HAPResult>(reader.u8());
  const uint8_t classId = reader.u8();
  const uint8_t instanceId = reader.u8();
  const uint8_t portId = reader.u8();
  const uint16_t intervalSec = reader.u16();

  return HAPSetPolicyResponse(result, classId, instanceId, portId, intervalSec,
                              reader.value());
}

// ---------------------------------------------------------------------------
// HAPSetNameRequest / HAPSetNameResponse
// ---------------------------------------------------------------------------

void HAPSetNameRequest::encode(HAPWriter& writer) const noexcept {
  writer.u8(static_cast<uint8_t>(target));
  writer.u8(classId);
  writer.u8(instanceId);
  writer.name(name);
}

bool HAPSetNameRequest::decode(HAPReader& reader) noexcept {
  target = static_cast<Target>(reader.u8());
  classId = reader.u8();
  instanceId = reader.u8();
  name = reader.name();

  return reader.ok();
}

void HAPSetNameResponse::encode(HAPWriter& writer) const noexcept {
  writer.u8(static_cast<uint8_t>(result));
  writer.u16(descriptorRev);
}

bool HAPSetNameResponse::decode(HAPReader& reader) noexcept {
  result = static_cast<HAPResult>(reader.u8());
  descriptorRev = reader.u16();

  return reader.ok();
}
