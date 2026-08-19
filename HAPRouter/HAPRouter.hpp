#pragma once

#include <HAPFrame/HAPFrame.hpp>
#include <HAPMac/HAPMac.hpp>

#include <etl/vector.h>

/** @brief What a node knows about one of its own children. */
struct HAPChild {
  /** 1..HAP_MAX_CHILDREN. Its routing identity, and only meaningful here. */
  uint8_t index = 0;

  /** Its permanent identity, which outlives any index it is given. */
  HAPMac mac;

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
   * A master has to keep this, and keep it across a reboot. Every link in HAP is
   * encrypted, and a child stores its own copy with its bind - so a master that
   * forgot the key would come back holding its children as PLAIN peers while
   * they went on transmitting encrypted, and neither side could hear the other
   * again. There is no recovery from that: a child has one parent for life and
   * will never re-announce, so nothing would ever re-establish the key.
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
  void setParent(const HAPMac& mac, uint8_t indexAtParent) noexcept;

  /** @brief Forgets it - a factory reset, and the only way to change parent. */
  void clearParent() noexcept;

  bool hasParent() const noexcept;

  /** @brief True when nothing is above this node: it is the root. */
  bool isRoot() const noexcept;

  const HAPMac& parentMac() const noexcept;

  /** @brief What this node is called by its parent. 0 when unbound. */
  uint8_t indexAtParent() const noexcept;

  /**
   * @brief How many children THIS node may have, which is not always the same.
   *
   * ESP-NOW holds six ENCRYPTED peers, and every link in HAP is encrypted -
   * including the one going up. So a node's budget is:
   *
   * | Node | Parent link | Children |
   * | ---- | ----------- | -------- |
   * | root | none | **6** |
   * | anything bound | 1 slot | **5** |
   *
   * Getting this wrong does not fail where you would look for it. The sixth
   * child of a bound node binds *almost* correctly - the acceptance goes out,
   * because an unencrypted peer still fits - and then the radio refuses to make
   * that peer encrypted, the child's confirmation is dropped below the protocol,
   * and the log says only that nobody confirmed.
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
  const HAPChild* childByMac(const HAPMac& mac) const noexcept;

  /** @brief For iteration; `position` is not an index. */
  const HAPChild* childAt(size_t position) const noexcept;

  /** @brief True when the address is the parent or a known child. */
  bool isKnown(const HAPMac& mac) const noexcept;

  /**
   * @brief Decides what happens to a received frame, and rewrites its paths.
   *
   * @param frame Mutated in place: the destination is shifted for a forward
   *        downstream, the source is prepended for a forward upstream. What
   *        comes back is ready to encode and send as it stands.
   * @param from The sender's address.
   */
  HAPRoute route(HAPFrame& frame, const HAPMac& from) const noexcept;

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

  HAPMac parentMac_;
  uint8_t indexAtParent_ = 0;
  bool hasParent_ = false;
};
