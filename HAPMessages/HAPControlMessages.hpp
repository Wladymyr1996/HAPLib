#pragma once

#include <HAPMessages/HAPMessageParts.hpp>

/**
 * @file HAPControlMessages.hpp
 * @brief Liveness, acknowledgement and refusal. Docs/Protocol.md section 4.
 *
 * Ping and DescribeRequest aside, these are the messages that carry no news -
 * which makes them the ones a network is diagnosed with.
 */

/** @brief "Is this link alive?" No payload at all. */
struct HAPPing {
  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;
};

/**
 * @brief "Yes", plus the revision.
 *
 * The revision rides here as well as on every report, so a master can check a
 * mains-powered node it has not heard values from recently without asking it to
 * measure anything.
 */
struct HAPPong {
  uint16_t descriptorRev = 0;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;
};

/**
 * @brief End-to-end acknowledgement, for frames that have no natural response.
 *
 * A WriteRequest gets a WriteResponse and needs none of these. This is for the
 * ones where "it arrived" is the entire answer.
 */
struct HAPAck {
  /** The sequence number being acknowledged. */
  uint16_t seq = 0;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;
};

/** @brief "Arrived, and refused." */
struct HAPNack {
  uint16_t seq = 0;
  HAPResult reason = HAPResult::BadRequest;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;
};

/**
 * @brief "I could not pass this on, and here is which hop failed."
 *
 * The difference between a timeout and a diagnosis: a master learns WHICH link
 * in the chain is down rather than only that something did not come back. Sent
 * toward the originator, whose route is the failed frame's own source path.
 */
struct HAPRouteError {
  /** Of the frame that could not be forwarded. */
  uint16_t seq = 0;

  /** The child index that could not be reached, relative to the sender. */
  uint8_t failedHop = 0;

  HAPResult reason = HAPResult::ChildUnreachable;

  void encode(HAPWriter& writer) const noexcept;
  bool decode(HAPReader& reader) noexcept;
};
