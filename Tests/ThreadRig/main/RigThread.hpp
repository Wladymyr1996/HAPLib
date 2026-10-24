#pragma once

/**
 * @file RigThread.hpp
 * @brief The rig's HAPThread: the transport under test, its tick task, and
 *        everything the bench prints about it.
 *
 * Every line the runner greps for is produced here or in RigCommands.cpp; the
 * README lists them. One printf per line, so tasks cannot interleave inside
 * one.
 */

#include <HAPAddress/HAPAddress.hpp>

#include <cstddef>
#include <cstdint>

namespace rig {

/**
 * Installs the receiver, calls begin() and starts the tick task. Call it after
 * OpenThread is running, because begin() fails until then.
 * @return False if begin() failed; HAPERR is printed and nothing runs.
 */
bool startThread();

/** HAPINFO: identity, address, attachment, drops and free heap. */
void printInfo();

/** Pauses update() for @p ms, so frames pile up in the queue. */
void stall(uint32_t ms);

/** The HAPTX line for a send. */
void send(const HAPAddress& to, const uint8_t* data, size_t size);

/** The HAPTX line for a multicast. */
void multicast(const uint8_t* data, size_t size);

/**
 * Starts the burst worker: @p count frames of @p size bytes, each carrying its
 * sequence number, one per @p intervalMs.
 * @return False while a burst is still running.
 */
bool startBurst(bool toMulticast, const HAPAddress& to, uint32_t count,
                uint16_t size, uint32_t intervalMs);

}  // namespace rig
