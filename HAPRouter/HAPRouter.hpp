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
  ToChild,  ///< Forward down; see HAPRoute::childIndex.
  ToParent, ///< Forward up.
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
 * it, queue it for a sleeping child, answer with a RouteError - belongs to the
 * layer above, which is what keeps this one exhaustively testable.
 *
 * ## The two rules
 * Downstream, eat the first hop and forward to that child; an empty destination
 * means the frame has arrived. Upstream, prepend the index of the child it came
 * from and pass it to the parent; no parent means this node is the root and the
 * frame has arrived.
 *
 * The prepend is the interesting half. Because every node does it, a climbing
 * frame's source path is, at each node it passes, exactly the downward path from
 * THAT node back to the origin - so a reply routes straight back down and
 * matching a report against a link is a byte comparison.
 *
 * ## What it refuses
 * - a frame from an address that is neither the parent nor a child, because
 *   nothing inside a frame says where it came from and only the sender's
 *   address does. Bind traffic never reaches here: an unbound stranger is the
 *   binder's business;
 * - a downstream frame arriving from a child, which is a node forwarding the
 *   wrong way;
 * - an upstream frame whose source path is already five hops long, which has
 *   travelled further than the tree may be deep. That is the loop guard.
 */
class HAPRouter {
 public:
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
   * @brief Decides what happens to a received frame, and rewrites its paths.
   *
   * @param frame Mutated in place: the destination is shifted for a forward
   *        downstream, the source is prepended for a forward upstream. What
   *        comes back is ready to encode and send as it stands.
   * @param from The sender's address.
   */
  HAPRoute route(HAPFrame& frame, const HAPNodeId& from) const noexcept;

  /**
   * @brief The downstream step alone, for a frame this node is ORIGINATING.
   *
   * A root addressing 1.4 resolves hop 1 itself, and what it emits carries only
   * the remainder - so its own frames take the same path through the same code
   * as anything it forwards.
   */
  HAPRoute resolveDownstream(HAPFrame& frame) const noexcept;

 private:
  etl::vector<HAPChild, HAP_MAX_CHILDREN> children_;

  HAPNodeId parentId_;
  uint8_t indexAtParent_ = 0;
  bool hasParent_ = false;
};
