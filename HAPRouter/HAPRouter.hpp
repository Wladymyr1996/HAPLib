#pragma once

#include <HAPFrame/HAPFrame.hpp>
#include <HAPNodeId/HAPNodeId.hpp>

#include <etl/vector.h>

/** @brief What a node knows about one of its own children. */
struct HAPChild {
  /** 1..HAP_MAX_CHILDREN. Its routing identity, and only meaningful here. */
  uint8_t index = 0;

  /** Its permanent identity, which outlives any index it is given. */
  HAPNodeId id;

  HAPDeviceType deviceType = HAPDeviceType::Sensor;

  /** HAPCaps bits. */
  uint8_t capabilities = HAPCaps::None;

  /** Last known; a change means the cached descriptor is stale. */
  uint16_t descriptorRev = 0;

  /** What it said in its announcement; 0 means it reports only on change. */
  uint16_t reportIntervalSec = 0;

  /**
   * The key this link is encrypted with. Zero until a bind supplies one.
   *
   * A master keeps this across a reboot, and a child stores its own copy with
   * its bind. Nothing reads it after load today; it is kept because the stored
   * format carries it.
   */
  uint8_t linkKey[HAP_KEY_LEN] = {};

  /** @brief True when a key is known. All-zero means none was ever stored. */
  bool hasLinkKey() const noexcept;

  /** @brief Asleep almost always - send to it and nothing happens. Queue instead. */
  bool isBatteryPowered() const noexcept;
};

/** @brief What to do with a frame. */
enum class HAPRouteAction : uint8_t {
  Deliver,  ///< For this node: hand it up to the stack.
  ToChild,  ///< Send to a direct child; see HAPRoute::childIndex.
  ToParent, ///< Forward up.
  ToNode,   ///< Send straight to the destination id; it is not a direct child.
  Drop      ///< Went nowhere; see HAPRoute::reason.
};

/** @brief The decision, and enough about it to answer or log. */
struct HAPRoute {
  HAPRouteAction action = HAPRouteAction::Drop;

  /** Which child, when the action is ToChild - or which one was missing. */
  uint8_t childIndex = 0;

  /** Why, when the action is Drop. Ok otherwise. */
  HAPResult reason = HAPResult::Ok;

  bool isDrop() const noexcept { return action == HAPRouteAction::Drop; }
};

/**
 * @brief Who this node's parent is, who its children are, and where a frame goes.
 *
 * The whole of HAP's addressing, and nothing else: no radio, no timers, no
 * state machine. Given a frame and the address it arrived from, it says
 * `deliver`, `forward there`, or `drop, because`. What to DO about that - send
 * it, queue it for a sleeping child - belongs to the layer above, which is what
 * keeps this one exhaustively testable.
 *
 * ## The rules
 * A frame addressed to this node is delivered, whoever sent it. A frame for the
 * master (destination zero) climbs: from a child it goes to the parent, and a
 * node with no parent is the root, where it has arrived. The source id is the
 * originator's and is never touched on the way, so a reply needs no return path
 * - it is addressed to that id.
 *
 * Nothing forwards DOWN the tree any more: a frame for another node is sent
 * straight to it (see resolveDownstream()). Only upstream traffic is relayed,
 * and it climbs a tree built by binding, which cannot loop.
 *
 * ## What it refuses
 * - a frame from an address that is neither the parent nor a child, unless it
 *   is addressed to this node, because only the sender's address says where it
 *   came from. Bind traffic never reaches here: an unbound stranger is the
 *   binder's business;
 * - a frame for the master arriving from the parent, which is a node forwarding
 *   the wrong way;
 * - a frame for some other node, which this node has no business relaying.
 */
class HAPRouter {
 public:
  /** The lowest child index; 0 means "none". */
  static constexpr uint8_t kMinChildIndex = 1;

  /** @brief Records the parent, and the index this node was given there. */
  void setParent(const HAPNodeId& id, uint8_t indexAtParent) noexcept;

  /** @brief Forgets it - a factory reset, and the only way to change parent. */
  void clearParent() noexcept;

  bool hasParent() const noexcept;

  /** @brief True when nothing is above this node: it is the root. */
  bool isRoot() const noexcept;

  const HAPNodeId& parentId() const noexcept;

  /** @brief What this node is called by its parent. 0 when unbound. */
  uint8_t indexAtParent() const noexcept;

  /**
   * @brief How many children THIS node may have, which is not always the same.
   *
   * The budget is inherited from ESP-NOW's peer limit: the link going up
   * costs one slot of the child table. The transport no longer imposes it, so
   * it is a configured limit that stays until the child table is redesigned:
   *
   * | Node | Parent link | Children |
   * | ---- | ----------- | -------- |
   * | root | none | **6** |
   * | anything bound | 1 slot | **5** |
   */
  uint8_t childCapacity() const noexcept;

  /**
   * @brief The lowest index not in use, or 0 when the node is full.
   *
   * Lowest rather than next: a child removed and replaced takes the slot back,
   * so a two-child network stays 1 and 2 however often it is rebuilt.
   */
  uint8_t freeChildIndex() const noexcept;

  /** @return false when full, when the index is illegal, or when it is taken. */
  bool addChild(const HAPChild& child) noexcept;

  bool removeChild(uint8_t index) noexcept;
  void clearChildren() noexcept;

  size_t childCount() const noexcept;
  bool isFull() const noexcept;

  /** @return nullptr when there is no such child. */
  const HAPChild* child(uint8_t index) const noexcept;
  const HAPChild* childById(const HAPNodeId& id) const noexcept;

  /** @brief For iteration; `position` is not an index. */
  const HAPChild* childAt(size_t position) const noexcept;

  /**
   * @brief Adopts the interval a child now says it reports at.
   *
   * The announcement is not the last word: a battery node whose owner changes
   * its wake period says so later, and this table is what re-seeds the model
   * after a reboot - so a stale entry here would bring the old interval back.
   * @return true when the child exists and the value actually changed.
   */
  bool setChildReportInterval(uint8_t index, uint16_t seconds) noexcept;

  /** @brief True when the address is the parent or a known child. */
  bool isKnown(const HAPNodeId& id) const noexcept;

  /**
   * @brief Decides what happens to a received frame. The frame is not changed:
   *        what is forwarded is what arrived.
   *
   * @param from The sender's address.
   * @param self This node's own id.
   */
  HAPRoute route(const HAPFrame& frame, const HAPNodeId& from,
                 const HAPNodeId& self) const noexcept;

  /**
   * @brief Where a frame this node is ORIGINATING goes.
   *
   * A direct child is reached through the child table, which is what lets a
   * sleeping one be queued for; any other node is sent to directly.
   */
  HAPRoute resolveDownstream(const HAPNodeId& dst,
                             const HAPNodeId& self) const noexcept;

 private:
  etl::vector<HAPChild, HAP_MAX_CHILDREN> children_;

  HAPNodeId parentId_;
  uint8_t indexAtParent_ = 0;
  bool hasParent_ = false;
};
