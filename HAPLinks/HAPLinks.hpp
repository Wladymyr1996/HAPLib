#pragma once

#include <HAPMessages/HAPLinkMessages.hpp>
#include <HAPNode/HAPNode.hpp>

#include <etl/delegate.h>
#include <etl/vector.h>

/**
 * @brief The wires this node holds: one port's output feeding another's input.
 *
 * A link lives at the **lowest common ancestor** of its two endpoints - the only
 * node that both sees the source value and has a route to the destination. Which
 * is why every path in one is downward: from here, everything a link touches is
 * at or below this node.
 *
 * ## Matching is a byte comparison
 * Because each node prepends the hop a frame arrived from, a climbing report's
 * source path is - at this node - exactly the downward path from here to
 * whatever produced it. That is the same thing a link stores, so deciding
 * whether a report feeds a link is a comparison rather than a calculation.
 *
 * ## A wire is a wire
 * Nothing here scales, thresholds or transforms. "Temperature above 22 turns the
 * lamp off" is a *class* - a Thermostat, with an input, a setpoint and an output
 * - not link syntax. The moment a wire can carry an expression, every node needs
 * an interpreter, and logic that could have been firmware and testable becomes
 * configuration that is neither.
 *
 * ## Delivery is a tap, not a diversion
 * A report that feeds a link still climbs. The gateway sees every reading
 * whether or not something downstream is wired to it.
 */
class HAPLinks {
 public:
  /**
   * @param node This node's own model, so a link touching it can be checked
   *        properly rather than taken on trust.
   */
  explicit HAPLinks(const HAPNode& node) noexcept;

  /**
   * @brief Installs a link, or replaces whatever occupied its slot.
   *
   * Validates what it can. The class table says whether the two ports exist,
   * face the right way and carry the same quantity kind - that much is true of
   * any node with those classes. Whether a particular node *has* that instance
   * is only knowable here for this node's own ports; deeper endpoints are taken
   * on trust from the configuring node, which holds the descriptors.
   *
   * @return Ok, or NoSuchClass, NoSuchPort, NotWritable, TypeMismatch,
   *         InputBusy or NoLinkSlot.
   */
  HAPResult set(const HAPLinkSpec& spec) noexcept;

  /** @brief Removes one link, or every link when given kAllLinks. */
  HAPResult clear(uint8_t linkId) noexcept;

  size_t size() const noexcept;
  bool isFull() const noexcept;

  /** @brief For iteration; `position` is not a link id. */
  const HAPLinkSpec* at(size_t position) const noexcept;

  const HAPLinkSpec* find(uint8_t linkId) const noexcept;

  /** @brief Fills one page of a ListLinks response. @return false past the end. */
  bool fillList(HAPListLinksResponse& response, uint8_t page) const noexcept;

  /** @brief How many frames a full listing takes. Never 0. */
  uint8_t pageCount() const noexcept;

  /**
   * @brief Where a value is to be delivered.
   *
   * An empty destination path means this node itself - the case that never
   * touches the radio, and the reason a control loop belongs as low in the tree
   * as it will go.
   */
  using DeliveryHook = etl::delegate<void(const HAPPortRef& destination,
                                          const HValue& value)>;
  void onDeliver(DeliveryHook hook) noexcept;

  /**
   * @brief Feeds one value to whatever is wired to it.
   * @param sourcePath Downward from this node; empty for its own instances.
   * @return How many links fired.
   */
  size_t deliver(const HAPPath& sourcePath, const HAPValueEntry& entry) noexcept;

  /** @brief The same, for every entry of a report. */
  size_t deliver(const HAPPath& sourcePath, const HAPReport& report) noexcept;

  /** @brief True when any link takes its source from that path. */
  bool watches(const HAPPath& sourcePath) const noexcept;

 private:
  /** True when something else already drives that input. */
  bool inputIsTaken(const HAPPortRef& destination, uint8_t exceptId) const noexcept;

  /** Checks an endpoint that lives on THIS node; deeper ones cannot be. */
  HAPResult validateLocal(const HAPPortRef& reference,
                          HAPPortDirection direction) const noexcept;

  const HAPNode& node_;
  etl::vector<HAPLinkSpec, HAP_MAX_LINKS> links_;
  DeliveryHook onDeliver_;
};
