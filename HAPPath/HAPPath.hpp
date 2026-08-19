#pragma once

#include <HAP.h>

#include <cstdint>

/**
 * @brief An address in the tree: the hops from where you stand to where you mean.
 *
 * There are no global node addresses in HAP. A node knows its parent's MAC and
 * the indices it handed to its own children, and nothing else - so "the bedroom
 * thermometer" is `1.2` seen from the gateway, `2` seen from the controller
 * above it, and empty seen from itself. This class is that relative address.
 *
 * ## The two operations that make routing work
 *
 * | Direction | Operation | Meaning |
 * | --------- | --------- | ------- |
 * | downstream | shift() | eat the first hop; what is left addresses the rest |
 * | upstream | prepend() | write in the child a frame arrived from |
 *
 * prepend() is the interesting one. Because every node prepends the hop a frame
 * came in on, a climbing frame's source path is - at every node it passes -
 * EXACTLY the downward path from that node back to the origin. The frame draws
 * its own return route, no node has to know the shape of the tree, and matching
 * a report against a link (see Docs/Links.md) is a byte comparison.
 *
 * ## Hop values
 * A hop is 1..254. Zero is padding, and 0xFF is reserved, so neither can be a
 * child index - which is what lets an all-zero path mean "empty" unambiguously
 * and keeps a malformed frame from routing anywhere at all.
 *
 * Trivially copyable, six bytes, no heap.
 */
class HAPPath {
 public:
  /** Lowest legal child index. Zero is padding, so counting starts at one. */
  static constexpr uint8_t kMinHop = 1;

  /** Highest legal child index. 0xFF is reserved for future use. */
  static constexpr uint8_t kMaxHop = 254;

  /** @brief Creates an empty path - "me" downstream, "the root" upstream. */
  HAPPath() noexcept;

  /**
   * @brief Builds a path from wire bytes.
   * @param hops Up to HAP_MAX_DEPTH hop indices; may be nullptr when length is 0.
   * @param length How many of them are meaningful.
   * @return An empty path if the length is impossible or any hop is illegal.
   *         There is deliberately no partial success: half an address routes
   *         somewhere, and somewhere is worse than nowhere.
   */
  static HAPPath fromBytes(const uint8_t* hops, uint8_t length) noexcept;

  /** @brief How many hops this address holds. */
  uint8_t length() const noexcept;

  /** @brief True when the address names the node holding it. */
  bool isEmpty() const noexcept;

  /** @brief True when no further hop can be prepended or appended. */
  bool isFull() const noexcept;

  /** @brief The hop at `index`, or 0 when there is none. */
  uint8_t hop(uint8_t index) const noexcept;

  /** @brief The next hop to forward to, or 0 when the address is empty. */
  uint8_t first() const noexcept;

  /**
   * @brief Removes the first hop - the downstream forwarding step.
   * @return false when the path was already empty, in which case the frame was
   *         for this node and should never have been forwarded.
   */
  bool shift() noexcept;

  /**
   * @brief Writes a hop at the FRONT - the upstream forwarding step.
   * @param hop The child index the frame arrived from, 1..254.
   * @return false when the path is full or the hop is illegal. A full path on
   *         the way up means the frame has travelled further than the tree is
   *         allowed to be deep, which is the loop guard: drop it.
   */
  bool prepend(uint8_t hop) noexcept;

  /**
   * @brief Writes a hop at the END, extending the address one level deeper.
   *
   * How a master names a grandchild it has just heard about: the path a frame
   * arrived by, plus the child index inside it (see ChildAttached).
   *
   * @return false when the path is full or the hop is illegal.
   */
  bool append(uint8_t hop) noexcept;

  /** @brief Makes the path empty again. */
  void clear() noexcept;

  /**
   * @brief The wire form: HAP_MAX_DEPTH bytes, unused tail zeroed.
   *
   * The zeroing is not cosmetic. Frames are compared and hashed as bytes, so a
   * path that left rubbish past its length would encode differently depending on
   * where it had been.
   */
  const uint8_t* bytes() const noexcept;

  /** @brief True when both paths have the same hops in the same order. */
  bool operator==(const HAPPath& other) const noexcept;
  bool operator!=(const HAPPath& other) const noexcept;

  /** @brief Dotted form for logs: "1.2", or "." when empty. */
  etl::string<HAP_PATH_TEXT_LEN> toString() const noexcept;

 private:
  static bool isLegalHop(uint8_t hop) noexcept;

  uint8_t hops_[HAP_MAX_DEPTH];
  uint8_t length_;
};
