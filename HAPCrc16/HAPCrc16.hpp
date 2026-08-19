#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @brief CRC-16/CCITT-FALSE: polynomial 0x1021, initial value 0xFFFF.
 *
 * Used for one thing - `descriptorRev`, the number a node carries in every
 * report and every pong so a master can tell whether what it has cached is
 * still what the node is.
 *
 * ## Why a checksum rather than a counter
 * A counter needs somewhere durable to live, survives a factory reset only if
 * somebody remembered to exclude it, and lets two devices disagree about what
 * "revision 4" meant. A checksum of the content cannot: the same descriptor
 * always produces the same number, on any node, after any reboot, with nothing
 * stored anywhere.
 *
 * It is a checksum and not a hash. Two different descriptors can collide, and if
 * one ever does the master keeps a stale cache until something else changes -
 * an annoyance, not a hazard, and cheap at sixteen bits in a frame that has to
 * fit in 250.
 */
uint16_t HAPCrc16(const uint8_t* data, size_t size) noexcept;

/** @brief Continues a running CRC over another block. */
uint16_t HAPCrc16Update(uint16_t crc, const uint8_t* data, size_t size) noexcept;

/** The value HAPCrc16Update must start from. */
constexpr uint16_t kHAPCrc16Init = 0xFFFF;
