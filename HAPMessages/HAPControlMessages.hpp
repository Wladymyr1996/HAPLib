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

