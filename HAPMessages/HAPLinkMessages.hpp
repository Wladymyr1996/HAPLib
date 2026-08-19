#pragma once

#include <HAPMessages/HAPMessageParts.hpp>

#include <etl/vector.h>

/**
 * @file HAPLinkMessages.hpp
 * @brief Wiring one port to another. Docs/Links.md section 5.
 *
 * A link is installed at the lowest common ancestor of its two endpoints - the
 * only node that both SEES the source value and HAS a route to the destination.
 * Which is why both references inside one are downward paths: from the node
 * holding the link, everything it links is below it.
 */

/**
 * @brief One wire: a source out port, a destination in port, and a slot.
 *
 * Doubles as the SetLinkRequest payload and as one record of a ListLinksResponse,
 * because they are the same nineteen bytes and describing them twice would be a
 * way for the two to disagree.
 */
struct HAPLinkSpec {
  /** 0..HAP_MAX_LINKS-1. Installing into an occupied slot overwrites it. */
  uint8_t linkId = 0;

  /** Must name an OUT port. */
  HAPPortRef source;

  /** Must name an IN port, and one nothing else is already driving. */
  HAPPortRef destination;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;

  static constexpr size_t kEncodedSize = 1 + 2 * HAPPortRef::kEncodedSize;
};

/**
 * @brief The outcome of installing one.
 *
 * Refused with NoSuchPort, TypeMismatch (the quantity kinds differ), InputBusy
 * or NoLinkSlot. A node validates what it can - it knows its own ports exactly,
 * and its direct children's classes - and takes anything deeper on trust from
 * the configuring node, which holds the descriptors.
 */
struct HAPSetLinkResponse {
  HAPResult result = HAPResult::Ok;
  uint8_t linkId = 0;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;
};

/** @brief Removes one link, or every link on the node. */
struct HAPClearLinkRequest {
  /** Clears all of them. */
  static constexpr uint8_t kAllLinks = 0xFF;

  uint8_t linkId = kAllLinks;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;
};

/** @brief The outcome of removing one. */
struct HAPClearLinkResponse {
  HAPResult result = HAPResult::Ok;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;
};

/** @brief "What is already wired here?" - so a user interface can show it. */
struct HAPListLinksRequest {
  uint8_t fromPage = 0;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;
};

/**
 * @brief The links a node holds, paginated with HAPFlags::More.
 *
 * Twelve records fill a frame, so a full table of HAP_MAX_LINKS needs two pages.
 */
struct HAPListLinksResponse {
  /** Links held in total, across all pages. */
  uint8_t count = 0;

  uint8_t pageIndex = 0;
  uint8_t pageCount = 1;

  etl::vector<HAPLinkSpec, HAP_MAX_LINKS> links;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;

  static constexpr size_t kFixedSize = 3;
};
