#pragma once

#include <HAPMessages/HAPMessageParts.hpp>

#include <etl/vector.h>

/**
 * @file HAPDataMessages.hpp
 * @brief Reading, writing, describing and naming. Docs/Protocol.md section 4.
 *
 * Every message here names a port as well as an instance. Port 0 is an
 * instance's primary value - what a single-valued class like Thermometer
 * reports, and the only port it has - so a device that never heard of ports
 * writes 0 everywhere and is correct.
 */

/** @brief "Tell me what you are." */
struct HAPDescribeRequest {
  /** 0 for the whole descriptor; a later page to resume an interrupted read. */
  uint8_t fromPage = 0;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;
};

/**
 * @brief A node's name and instances, paginated.
 *
 * HAPFlags::More is set on every frame but the last. As with BindAnnounce,
 * choosing what goes on a page belongs to HAPNode, not to this structure.
 */
struct HAPDescribeResponse {
  uint16_t descriptorRev = 0;
  uint8_t instanceCount = 0;
  uint8_t pageIndex = 0;
  uint8_t pageCount = 1;
  HAPName nodeName;

  etl::vector<HAPInstanceDescriptor, HAP_MAX_INSTANCES> instances;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;

  /** Bytes before the descriptors: five fixed plus the node's name. */
  static constexpr size_t kFixedSize = 5;
};

/**
 * @brief Values, unsolicited or asked for - Report and ReadResponse both.
 *
 * The two are the same payload deliberately: a master parsing an answer and a
 * master parsing an announcement should not need two code paths for the same
 * information.
 *
 * The revision rides along in every one of these, which is what lets a master
 * notice a node it has stale knowledge of without asking anybody anything.
 */
struct HAPReport {
  uint16_t descriptorRev = 0;

  etl::vector<HAPValueEntry, HAP_MAX_REPORT_ENTRIES> entries;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;
};

/** @brief "What is it reading?" */
struct HAPReadRequest {
  /** Wildcard: every class on the node. */
  static constexpr uint8_t kAnyClass = 0xFF;
  /** Wildcard: every instance of the named class. */
  static constexpr uint8_t kAnyInstance = 0xFF;
  /** Wildcard: every out port of the named instance. */
  static constexpr uint8_t kAnyPort = 0xFF;

  uint8_t classId = kAnyClass;
  uint8_t instanceId = kAnyInstance;
  uint8_t portId = kAnyPort;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;
};

/** @brief "Be this." Only an in port may be written. */
struct HAPWriteRequest {
  HAPWriteRequest() noexcept = default;
  HAPWriteRequest(uint8_t classId, uint8_t instanceId, uint8_t portId,
                  const HValue& value) noexcept;

  uint8_t classId = 0;
  uint8_t instanceId = 0;
  uint8_t portId = 0;
  HValue value;

  /** See HAPValueEntry::setValue - plain assignment would coerce. */
  void setValue(const HValue& newValue) noexcept;

  void encode(HAPWriter& writer) const noexcept;

  /** @brief Reads a request. Check reader.ok() afterwards. */
  static HAPWriteRequest decode(HAPReader& reader) noexcept;
};

/**
 * @brief What was actually taken, which may not be what was asked.
 *
 * A dimmer told 1.5 answers 1.0; a master that assumed otherwise would show a
 * value the device is not at.
 */
struct HAPWriteResponse {
  HAPWriteResponse() noexcept = default;
  HAPWriteResponse(HAPResult result, uint8_t classId, uint8_t instanceId,
                   uint8_t portId, const HValue& value) noexcept;

  HAPResult result = HAPResult::Ok;
  uint8_t classId = 0;
  uint8_t instanceId = 0;
  uint8_t portId = 0;
  HValue value;

  void setValue(const HValue& newValue) noexcept;

  void encode(HAPWriter& writer) const noexcept;
  static HAPWriteResponse decode(HAPReader& reader) noexcept;
};

/**
 * @brief How often an out port should speak, and how much it must move first.
 *
 * A node reports when EITHER the interval elapses OR the value has moved by more
 * than the deadband. A sleeping node can only honour the second when it wakes,
 * so its wake period is a floor on both whatever this says.
 */
struct HAPSetPolicyRequest {
  HAPSetPolicyRequest() noexcept = default;
  HAPSetPolicyRequest(uint8_t classId, uint8_t instanceId, uint8_t portId,
                      uint16_t intervalSec, const HValue& deadband) noexcept;

  uint8_t classId = 0;
  uint8_t instanceId = 0;
  uint8_t portId = 0;

  /** Seconds; 0 means report only on change. */
  uint16_t intervalSec = 0;

  /** Null for none. */
  HValue deadband;

  void setDeadband(const HValue& newDeadband) noexcept;

  void encode(HAPWriter& writer) const noexcept;
  static HAPSetPolicyRequest decode(HAPReader& reader) noexcept;
};

/** @brief The policy in force afterwards, which a node may have clamped. */
struct HAPSetPolicyResponse {
  HAPSetPolicyResponse() noexcept = default;
  HAPSetPolicyResponse(HAPResult result, uint8_t classId, uint8_t instanceId,
                       uint8_t portId, uint16_t intervalSec,
                       const HValue& deadband) noexcept;

  HAPResult result = HAPResult::Ok;
  uint8_t classId = 0;
  uint8_t instanceId = 0;
  uint8_t portId = 0;
  uint16_t intervalSec = 0;
  HValue deadband;

  void setDeadband(const HValue& newDeadband) noexcept;

  void encode(HAPWriter& writer) const noexcept;
  static HAPSetPolicyResponse decode(HAPReader& reader) noexcept;
};

/** @brief Renaming the node itself, or one of its instances. */
struct HAPSetNameRequest {
  enum class Target : uint8_t { Node = 0x00, Instance = 0x01 };

  Target target = Target::Node;

  /** Both ignored when the target is the node. */
  uint8_t classId = 0;
  uint8_t instanceId = 0;

  HAPName name;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;
};

/**
 * @brief The outcome and the NEW revision.
 *
 * Recomputed, because names are part of the descriptor - which is how a master
 * that was switched off during the rename finds out about it.
 */
struct HAPSetNameResponse {
  HAPResult result = HAPResult::Ok;
  uint16_t descriptorRev = 0;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;
};
