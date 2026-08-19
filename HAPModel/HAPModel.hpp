#pragma once

#include <HAPMac/HAPMac.hpp>
#include <HAPMessages/HAPMessages.hpp>
#include <HAPPath/HAPPath.hpp>

#include <etl/delegate.h>
#include <etl/vector.h>

/**
 * Nodes a model may hold.
 *
 * NOT free. Each one carries a name, up to HAP_MAX_INSTANCES descriptors and up
 * to HAP_MAX_REPORT_ENTRIES values - close to a kilobyte with the default
 * limits, so eight of them is most of a small heapless budget. A gateway with
 * RAM to spare should raise this knowing that; a controller should not hold a
 * model at all.
 */
#ifndef HAP_MODEL_MAX_NODES
#define HAP_MODEL_MAX_NODES 8
#endif

/** @brief One node somewhere below this one, as far as this one knows. */
struct HAPRemoteNode {
  /** Downward from the node holding the model. Its identity here. */
  HAPPath path;

  /** Its permanent identity, once ChildAttached has said so. Zero until then. */
  HAPMac mac;

  HAPDeviceType deviceType = HAPDeviceType::Sensor;
  uint8_t capabilities = HAPCaps::None;

  /** As last heard, in any report or pong. */
  uint16_t descriptorRev = 0;

  /** The revision the descriptor below actually belongs to. */
  uint16_t cachedRev = 0;

  /** False until a complete Describe has arrived. */
  bool described = false;

  HAPName name;
  etl::vector<HAPInstanceDescriptor, HAP_MAX_INSTANCES> instances;

  /** What it last reported. Null entries are readings it does not have. */
  etl::vector<HAPValueEntry, HAP_MAX_REPORT_ENTRIES> values;

  /** How often it says it intends to speak. 0 means only on change. */
  uint16_t reportIntervalSec = 0;

  uint32_t lastHeardMs = 0;
  bool online = true;

  bool isBatteryPowered() const noexcept;

  /** @brief True when what is cached does not belong to the current revision. */
  bool needsDescribe() const noexcept;

  /** @return nullptr when this node has no such port in its last report. */
  const HValue* value(uint8_t classId, uint8_t instanceId,
                      uint8_t portId) const noexcept;
};

/**
 * @brief What the root knows about the network beneath it.
 *
 * Only the root needs one. Every node keeps its direct children in its router
 * because it cannot route without them; a controller in the middle keeps
 * nothing else, so it cannot hold a stale picture of a subtree. This class is
 * the exception, and it exists because somebody has to have the whole picture
 * for a user interface to show.
 *
 * ## Discovery is a side effect of reporting
 * Nothing announces the shape of the network. A report arriving with
 * `src = 1.2` says there is a node at 1.2, and the revision it carries says
 * whether what is cached about it is still true. So the model is built by
 * listening, and `Describe` is pulled only for what it turns out not to know.
 *
 * That single mechanism covers a renamed instance, a firmware update that added
 * a class, and a device swapped for a different one in the same slot - and it
 * heals itself after any outage, because a gateway switched off for a week
 * finds every revision unfamiliar on the first report from each node.
 *
 * ## Silence is not the same as absence
 * A node that declared a report interval is marked offline after three of them.
 * A node that reports only on change never is: it has promised nothing, and a
 * Ping is the way to ask. Marking it offline for being quiet would be inventing
 * a fault.
 */
class HAPModel {
 public:
  /**
   * @brief Records that a node exists and was heard from just now.
   * @return The entry, or nullptr when the model is full.
   */
  HAPRemoteNode* observe(const HAPPath& path) noexcept;

  /** @brief Applies a report: values, revision, and proof of life. */
  HAPRemoteNode* noteReport(const HAPPath& path, const HAPReport& report) noexcept;

  /**
   * @brief Applies one page of a Describe.
   *
   * Page 0 replaces what was cached; later pages add to it. Only when the last
   * page arrives is the descriptor marked as belonging to its revision - a
   * half-arrived descriptor is not a descriptor.
   */
  HAPRemoteNode* noteDescribe(const HAPPath& path,
                              const HAPDescribeResponse& response) noexcept;

  /** @brief Applies a pong: revision and proof of life, without values. */
  HAPRemoteNode* notePong(const HAPPath& path, const HAPPong& pong) noexcept;

  /**
   * @brief Applies a policy answer: what this node will now ACTUALLY do.
   *
   * The interval a master measures a silence against has to be the one the node
   * agreed to, not the one it announced when it bound. Otherwise tightening a
   * node's reporting is a way to have it declared offline for obeying - which is
   * what the bench found: a leaf told to report every 3600 s was marked offline
   * thirty seconds later, having done exactly as it was asked.
   *
   * A node speaks when ANY of its out ports is due, so the number here is only
   * as good as the last answer. That errs toward waiting too long before calling
   * a node offline, which is the right way to be wrong: a late diagnosis is
   * slower, an early one is a lie about a working node.
   */
  HAPRemoteNode* notePolicy(const HAPPath& path,
                            const HAPSetPolicyResponse& response) noexcept;

  /**
   * @brief Applies a ChildAttached, whose path is its parent's plus the index.
   *
   * This is the one message that carries a MAC, and therefore the only way to
   * notice that a device has MOVED: the same address turning up at a different
   * path is a node re-bound elsewhere, not a second device. Its old entry is
   * dropped, because leaving both would show one thing twice.
   */
  HAPRemoteNode* noteChildAttached(const HAPPath& parentPath,
                                   const HAPChildAttached& attached) noexcept;

  /**
   * @brief Puts back a node this device was told about before it restarted.
   *
   * The identity half of an entry - address, MAC, capabilities, interval -
   * arrives in exactly one message, `ChildAttached`, and only ever once. A
   * report carries none of it. So a master that restarts and waits to be told
   * again is never told: it rebuilds its picture out of reports, and every node
   * below the first hop becomes an entry with no MAC, which cannot be
   * recognised if the device moves, and no interval, which means it can never
   * be called offline either. It sits there until the next restart.
   *
   * This is how that is avoided: the master keeps what it was told and puts it
   * back itself. Deliberately NOT restored are the name, the instances and the
   * values - those are a cache, `described` stays false, and the descriptor is
   * pulled again. What comes back is only what somebody said, never what was
   * inferred.
   */
  HAPRemoteNode* restore(const HAPPath& path, const HAPMac& mac,
                         HAPDeviceType deviceType, uint8_t capabilities,
                         uint16_t reportIntervalSec) noexcept;

  size_t size() const noexcept;
  bool isFull() const noexcept;

  const HAPRemoteNode* at(size_t position) const noexcept;
  const HAPRemoteNode* find(const HAPPath& path) const noexcept;
  const HAPRemoteNode* findByMac(const HAPMac& mac) const noexcept;

  /**
   * @brief The first node whose cached descriptor is out of date.
   * @return false when everything known is current.
   */
  bool nextDescribeNeeded(HAPPath& path) const noexcept;

  /** @brief How many nodes are waiting to be described. */
  size_t describeBacklog() const noexcept;

  /**
   * @brief Marks nodes that have missed HAP_OFFLINE_INTERVALS of their own.
   * @return How many changed state, so a caller only acts when something did.
   */
  size_t sweepOffline() noexcept;

  /** @brief Removes a node and everything cached about it. */
  bool forget(const HAPPath& path) noexcept;

  void clear() noexcept;

  /**
   * @brief A path the model had never seen before.
   *
   * Called once the message that revealed the node has been applied, so the
   * record handed over is populated rather than blank. That ordering is the
   * whole value of the hook: `ChildAttached` is the only message carrying a MAC,
   * and a hook fired before it was written saw an all-zero address for the
   * device it was announcing.
   */
  using DiscoveredHook = etl::delegate<void(const HAPRemoteNode&)>;
  void onDiscovered(DiscoveredHook hook) noexcept;

  /** @brief A node crossing between online and offline, either way. */
  using PresenceHook = etl::delegate<void(const HAPRemoteNode&)>;
  void onPresenceChanged(PresenceHook hook) noexcept;

 private:
  HAPRemoteNode* findMutable(const HAPPath& path) noexcept;

  /**
   * @brief observe() without the announcement: records the node, says if it is new.
   *
   * Every note* method fills the record in after this returns, so they announce
   * for themselves once they are done - see announceIfNew().
   */
  HAPRemoteNode* touch(const HAPPath& path, bool& isNew) noexcept;

  /** @brief Fires onDiscovered for a record that is now complete. */
  void announceIfNew(bool isNew, const HAPRemoteNode& node) noexcept;

  etl::vector<HAPRemoteNode, HAP_MODEL_MAX_NODES> nodes_;

  DiscoveredHook onDiscovered_;
  PresenceHook onPresenceChanged_;
};
