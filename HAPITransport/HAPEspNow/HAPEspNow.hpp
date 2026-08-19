#pragma once

#include <HAPITransport/HAPITransport.hpp>

#if IS_MCU

#include <etl/mutex.h>
#include <etl/vector.h>

/** Frames held between the radio's callback and the next update(). */
#ifndef HAP_ESPNOW_RX_QUEUE
#define HAP_ESPNOW_RX_QUEUE 8
#endif

/**
 * @file HAPEspNow.hpp
 * @brief The real radio: ESP-NOW on an ESP32.
 *
 * Target only - a host build has HAPLoopback instead, and the two are held to
 * the same promises on purpose, so that code which passes in simulation has a
 * chance of working here.
 *
 * ## Nothing happens in the radio's callback
 * ESP-NOW delivers on the Wi-Fi task. Blocking there stalls the radio, and
 * calling back into HAP from there would run the whole protocol on somebody
 * else's stack with none of HAP's own invariants held. So the callback copies
 * into a queue and returns, and update() - called from the task that owns this
 * transport, on HCoreLib's tick - hands frames to the receiver.
 *
 * That is why this class has an update() the interface does not: the simulated
 * transport has nothing to drain.
 *
 * ## One per device
 * ESP-NOW registers plain C callbacks with no user pointer, so there is exactly
 * one instance and the trampolines find it through a file-static pointer.
 * Constructing a second is a programming error and is refused.
 *
 * ## What it does NOT do
 * No retries, no acknowledgement tracking, no queueing for sleeping peers. A
 * send here means the frame reached the radio - the protocol above decides what
 * that is worth. Keeping the transport this thin is what lets the simulated one
 * be honest.
 */
class HAPEspNow : public HAPITransport {
 public:
  HAPEspNow() noexcept;
  ~HAPEspNow() override;

  /**
   * @brief Brings up Wi-Fi in station mode and ESP-NOW on one channel.
   *
   * Tolerates a network stack somebody else already started, so a device that
   * runs a portal can hand the radio over without a fight - but the channel is
   * then whatever this sets it to, and joining a foreign access point will move
   * it again. See Docs/HowItWorks.md on why that is the gateway's problem and
   * nobody else's.
   *
   * The broadcast address is added as a peer here, because a node that forgot to
   * cannot be bound and the symptom - unicast works, binding never does - costs
   * an afternoon.
   *
   * @return false with a log line naming the failed call. NVS must already be
   *         initialised: Wi-Fi keeps its calibration there.
   */
  bool begin(uint8_t channel) override;

  uint8_t channel() const noexcept override;
  const HAPMac& localMac() const noexcept override;

  bool addPeer(const HAPMac& mac, const uint8_t* key) override;
  bool removePeer(const HAPMac& mac) override;
  bool hasPeer(const HAPMac& mac) const noexcept override;

  bool send(const HAPMac& mac, const uint8_t* data, size_t size) override;
  bool broadcast(const uint8_t* data, size_t size) override;
  void onReceive(Receiver receiver) override;
  void onSendResult(SendResult result) override;

  /**
   * @brief Installs the primary master key, before any encrypted peer is added.
   *
   * ESP-NOW encrypts each link's key with this one. Adding an encrypted peer
   * without it appears to work and then delivers nothing.
   *
   * @param pmk HAP_KEY_LEN bytes.
   */
  bool setPrimaryKey(const uint8_t* pmk) noexcept;

  /**
   * @brief Hands every queued frame to the receiver. Call it every tick.
   * @return How many were delivered.
   */
  size_t update() noexcept;

  /** @brief Moves the radio to another channel. Every peer must follow. */
  bool setChannel(uint8_t channel) override;

  /**
   * @brief Signal strength of the frame currently being delivered, in dBm.
   *
   * Only meaningful inside the receiver. Worth logging on every received frame:
   * a protocol that works at ten centimetres and fails at two metres is an
   * antenna problem wearing a protocol problem's clothes.
   */
  int8_t lastRssi() const noexcept;

  /** @brief Frames the radio reported as not delivered. */
  uint32_t sendFailures() const noexcept;

  /**
   * @brief Frames dropped because update() was not called often enough.
   *
   * Never zero on a healthy node. Anything else means the task owning this
   * transport is being starved, or a burst arrived faster than one tick.
   */
  uint32_t receiveOverflows() const noexcept;

  /** @brief Called by the radio's callbacks. Not for anybody else. */
  void handleReceive(const uint8_t* mac, const uint8_t* data, int size,
                     int8_t rssi) noexcept;
  void handleSend(const uint8_t* mac, bool delivered) noexcept;

 private:
  struct Entry {
    HAPMac from;
    int8_t rssi = 0;
    uint8_t size = 0;
    uint8_t data[HAP_MAX_FRAME_SIZE] = {};
  };

  bool addEspNowPeer(const HAPMac& mac, const uint8_t* key) noexcept;

  HAPMac mac_;
  Receiver receiver_;
  SendResult sendResult_;
  uint8_t channel_ = 0;
  bool started_ = false;

  /** Mirrors the radio's peer table so hasPeer() costs nothing. */
  etl::vector<HAPMac, HAP_MAX_CHILDREN + 2> peers_;

  // Written by the Wi-Fi task, read by ours. Small enough that the lock is held
  // for a memcpy and nothing else.
  etl::mutex queueLock_;
  etl::vector<Entry, HAP_ESPNOW_RX_QUEUE> queue_;

  int8_t lastRssi_ = 0;
  uint32_t sendFailures_ = 0;
  uint32_t receiveOverflows_ = 0;
};

#endif  // IS_MCU
