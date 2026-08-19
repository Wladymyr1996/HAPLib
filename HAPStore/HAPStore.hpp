#pragma once

#include <HAPModel/HAPModel.hpp>
#include <HAPNode/HAPNode.hpp>
#include <HAPRouter/HAPRouter.hpp>

/**
 * @brief What a node must remember: who its parent is, who its children are,
 *        and what everything is called.
 *
 * A bind is meant to survive a reboot, a firmware update and - for a battery
 * node - several thousand deep sleeps. Nothing else in HAPLib touches storage.
 *
 * ## Four files, because they change for four different reasons
 *
 * | Module | Holds | Written when |
 * | ------ | ----- | ------------ |
 * | `hapbind` | the link upward: parent, index, key, channel | a bind, or a reset |
 * | `hapkids` | the children table, keys included | a child binds or leaves |
 * | `hapnames` | the node's name and its instances' | a user renames something |
 * | `hapmodel` | what a master was told about the tree below | a node appears, moves or goes |
 *
 * One file would mean rewriting the whole thing to record a rename, and
 * HConfig::write() replaces a file wholesale - so a partial save would silently
 * drop whatever it was not given.
 *
 * ## Values are packed
 * A child is one string - `01246f28aabbcc01009c4a` - rather than five entries.
 * Not to save flash: to keep the entry array that builds a write off the stack
 * of a 4 KB task. Six children as five entries each is 3 KB of HConfigEntry;
 * as one each it is under a kilobyte.
 *
 * Its **link key** is the exception, and gets an entry of its own (`k0`..`k5`).
 * A 16-byte key is 32 hex characters, which is exactly what
 * HVALUE_MAX_STRING_LEN is already required to hold; appending it to the record
 * would have needed 58 and broken any application sized to the documented
 * minimum. Both halves must be kept: a master that restored its children without
 * their keys would hold them as unencrypted peers while they transmitted
 * encrypted, and neither end would ever hear the other again.
 *
 * ## The RTC mirror holds exactly what a wake needs to transmit
 * Only the link upward is mirrored into retained memory, and deliberately not
 * the children or the names: a battery node wakes, measures, reports to its
 * parent and sleeps, and mounting a filesystem to look up the parent's address
 * would cost more than the measurement did. A master is mains-powered and can
 * afford the file.
 *
 * The mirror is lost on a power cut, which is correct - that is exactly when the
 * file should be believed instead.
 */

/** @brief The link upward. Everything a slave needs to reach its parent. */
struct HAPBindState {
  bool bound = false;
  HAPMac parent;

  /** What the parent calls this node. 1..HAP_MAX_CHILDREN. */
  uint8_t indexAtParent = 0;

  /** The network's channel, learned from BindAccept. */
  uint8_t channel = 0;

  uint8_t linkKey[HAP_KEY_LEN] = {};
};

class HAPStore {
 public:
  /**
   * @brief Registers the retained block. Call once, before anything else.
   *
   * HConfig::init() and a mounted filesystem are the application's business and
   * must already have happened - HAPLib does not own either.
   */
  static void init() noexcept;

  /**
   * @brief Restores the link upward: retained memory first, then the file.
   * @return false when this node has never been bound.
   */
  static bool loadBind(HAPBindState& state) noexcept;

  /** @brief Records it in both places. */
  static bool saveBind(const HAPBindState& state) noexcept;

  /**
   * @brief Forgets the parent - a factory reset, and the only way to rebind.
   *
   * Clears the retained copy as well as the file, or the next wake would
   * restore the parent this just deleted.
   */
  static void clearBind() noexcept;

  /**
   * @brief Whether the last loadBind() came from retained memory.
   *
   * True on a timer wake, false after a power cut - which is the difference
   * between a cheap wake and one that had to read flash, and worth logging on a
   * battery node.
   */
  static bool restoredFromRtc() noexcept;

  /** @brief Fills the router's child table. @return how many were restored. */
  static size_t loadChildren(HAPRouter& router) noexcept;

  static bool saveChildren(const HAPRouter& router) noexcept;

  /**
   * @brief Applies stored names to a node whose instances already exist.
   *
   * Names are user data laid over a class set the firmware decides, so this runs
   * AFTER the application has added its instances. An instance with no stored
   * name keeps whatever the firmware gave it.
   */
  static void loadNames(HAPNode& node) noexcept;

  static bool saveNames(const HAPNode& node) noexcept;

  /**
   * @brief The identity of every node this one has been TOLD about.
   *
   * The root's own memory of the network, and the answer to a problem three
   * boards found: `ChildAttached` is the only message carrying a child's MAC,
   * capabilities and interval, it is sent exactly once, and nothing can ask for
   * it again. A master that restarts therefore rebuilds its model out of
   * reports - which carry none of those - and every node below its own children
   * becomes an entry that can neither be recognised when the device moves nor
   * ever be marked offline.
   *
   * Sixteen bytes a node: the path, the MAC, the capabilities, the device type
   * and the interval. That is 32 hexadecimal characters, which is exactly what
   * HVALUE_MAX_STRING_LEN is already required to hold - see the static_assert
   * in HAPStore.cpp, which the link key set.
   *
   * **Only what somebody said, never what was inferred.** Names, instances and
   * values are a cache and are not written: a master that has been away should
   * pull the descriptor again rather than show what may have changed while it
   * was gone.
   *
   * ## Written rarely, on purpose
   * Identity changes when a node is discovered, moves, or is forgotten - and at
   * no other time. Saving on every report would be a flash write every ten
   * seconds for a value that had not changed.
   */
  static bool saveModel(const HAPModel& model) noexcept;

  /** @brief Fills the model back in. @return how many nodes were restored. */
  static size_t loadModel(HAPModel& model) noexcept;

  /** @brief Removes every file and the retained copy. */
  static bool clearAll() noexcept;
};
