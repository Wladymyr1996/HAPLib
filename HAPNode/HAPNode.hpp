#pragma once

#include <HAPInstance/HAPInstance.hpp>
#include <HAPMessages/HAPMessages.hpp>

#include <etl/vector.h>

/**
 * @brief This device, as the protocol sees it: a name, some instances, and a
 *        number that says whether anything about them has changed.
 *
 * Everything a node can be asked about is answered here - describe yourself,
 * what are you reading, be this, call that something else - and nothing here
 * knows about radios, parents or frames. A master's question arrives as a
 * message structure and leaves as one.
 *
 * ## The class set is fixed at start-up; names are not
 * addInstance() is a start-up call. Nothing at runtime may add or remove a
 * class, because what a device can do is a property of its hardware and its
 * firmware - a thermometer has a thermometer. What a user actually edits is the
 * label, and setName() is free at any time.
 *
 * ## descriptorRev
 * A CRC-16 over the whole descriptor, names included, recomputed whenever
 * anything in it moves. It rides in every report and every pong, so a master
 * that cached a descriptor can tell whether it is still current without asking -
 * and asks again when it is not. That single number covers a rename, a firmware
 * update that added a class, and a device swapped for a different one in the
 * same slot.
 *
 * ## Paging
 * A descriptor may not fit in one frame, and it is HERE that the question is
 * answered rather than in the messages, because which instances go on which page
 * is a fact about a node. fillDescribe() takes a page number and puts as many
 * instances on it as the frame will hold.
 */
class HAPNode {
 public:
  /** @brief Sets what this device is, before any instance is added. */
  void begin(HAPDeviceType deviceType, uint8_t capabilities,
             const HAPName& name) noexcept;

  HAPDeviceType deviceType() const noexcept;
  uint8_t capabilities() const noexcept;
  bool isBatteryPowered() const noexcept;

  const HAPName& name() const noexcept;
  void setName(const HAPName& newName) noexcept;

  /** @brief How often this node intends to report; 0 means only on change. */
  uint16_t reportIntervalSec() const noexcept;
  void setReportIntervalSec(uint16_t seconds) noexcept;

  /**
   * @brief Adds a class instance. A start-up call - see the class note.
   * @return The instance, or nullptr when the node is full or the class is one
   *         this build does not know.
   */
  HAPInstance* addInstance(HAPClassId classId, const HAPName& name) noexcept;

  size_t instanceCount() const noexcept;

  /** @return nullptr when this node has no such instance. */
  HAPInstance* instance(uint8_t classId, uint8_t instanceId) noexcept;
  const HAPInstance* instance(uint8_t classId, uint8_t instanceId) const noexcept;

  /** @brief For iteration; `position` is not an instance id. */
  HAPInstance* instanceAt(size_t position) noexcept;
  const HAPInstance* instanceAt(size_t position) const noexcept;

  /** @brief The CRC-16 of everything a master would cache about this node. */
  uint16_t descriptorRev() const noexcept;

  /** @brief How many frames a full descriptor takes. Never 0. */
  uint8_t pageCount() const noexcept;

  /**
   * @brief Fills one page of a Describe response.
   * @return false when `page` is past the end.
   */
  bool fillDescribe(HAPDescribeResponse& response, uint8_t page) const noexcept;

  /** @brief The same pagination, for the announcement a bind starts with. */
  bool fillAnnounce(HAPBindAnnounce& announce, uint8_t page) const noexcept;

  /** @brief Every out port that has a value, for an unsolicited report. */
  void fillReport(HAPReport& report) const noexcept;

  /**
   * @brief Answers a read, wildcards included.
   * @return Ok, or NoSuchClass / NoSuchPort when a specific request names
   *         something that is not here. A wildcard that matches nothing is Ok
   *         with an empty report - "I have none of those" is an answer.
   */
  HAPResult read(const HAPReadRequest& request, HAPReport& report) const noexcept;

  /** @brief Applies a write and builds the response, which reports what was taken. */
  HAPWriteResponse write(const HAPWriteRequest& request) noexcept;

  /** @brief Renames the node or one instance, and returns the NEW revision. */
  HAPSetNameResponse rename(const HAPSetNameRequest& request) noexcept;

 private:
  /** Instances that fit on `page`, and how many pages there are in total. */
  struct Page {
    size_t first = 0;
    size_t count = 0;
    uint8_t total = 1;
  };

  Page pageAt(uint8_t page, size_t fixedSize) const noexcept;

  HAPDeviceType deviceType_ = HAPDeviceType::Sensor;
  uint8_t capabilities_ = HAPCaps::None;
  uint16_t reportIntervalSec_ = 0;
  HAPName name_;

  etl::vector<HAPInstance, HAP_MAX_INSTANCES> instances_;
};
