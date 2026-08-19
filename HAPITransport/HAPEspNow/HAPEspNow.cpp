#define HLOG_MODULE_NAME "HapNow"

#include <HAPITransport/HAPEspNow/HAPEspNow.hpp>

#if IS_MCU

#include <HLog/HLog.hpp>

#include <cstring>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"

namespace {

/**
 * ESP-NOW registers plain C callbacks with no user pointer, so the trampolines
 * below have to find their way back here. One radio, one instance.
 */
HAPEspNow* gInstance = nullptr;

void recvTrampoline(const esp_now_recv_info_t* info, const uint8_t* data,
                    int size) {
  if (gInstance == nullptr || info == nullptr) {
    return;
  }

  // rx_ctrl is the only place the signal strength is available, and it is gone
  // the moment this returns.
  const int8_t rssi = info->rx_ctrl != nullptr
                          ? static_cast<int8_t>(info->rx_ctrl->rssi)
                          : 0;

  gInstance->handleReceive(info->src_addr, data, size, rssi);
}

void sendTrampoline(const esp_now_send_info_t* info,
                    esp_now_send_status_t status) {
  if (gInstance == nullptr) {
    return;
  }

  gInstance->handleSend(info != nullptr ? info->des_addr : nullptr,
                        status == ESP_NOW_SEND_SUCCESS);
}

/** True for the errors that mean "somebody already did this", which is fine. */
bool alreadyDone(esp_err_t error) noexcept {
  return error == ESP_OK || error == ESP_ERR_INVALID_STATE;
}

}  // namespace

HAPEspNow::HAPEspNow() noexcept {
  if (gInstance == nullptr) {
    gInstance = this;
    return;
  }

  // Refused rather than silently taking over: two of these would each receive
  // half the frames, which is a fault nobody would diagnose from a log.
  HCritical("A second HAPEspNow was constructed; this one will never receive");
}

HAPEspNow::~HAPEspNow() {
  if (gInstance != this) {
    return;
  }

  if (started_) {
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
  }

  gInstance = nullptr;
}

bool HAPEspNow::begin(uint8_t channel) {
  if (started_) {
    return setChannel(channel);
  }

  if (gInstance != this) {
    return false;
  }

  // Each of these may already have been done by an application that runs a
  // portal, and each says so in its own way. What must NOT be tolerated is a
  // real failure, so the return values are checked rather than discarded.
  esp_err_t error = esp_netif_init();
  if (!alreadyDone(error)) {
    HCritical("esp_netif_init failed: %s", esp_err_to_name(error));
    return false;
  }

  error = esp_event_loop_create_default();
  if (!alreadyDone(error)) {
    HCritical("esp_event_loop_create_default failed: %s", esp_err_to_name(error));
    return false;
  }

  const wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  error = esp_wifi_init(&config);
  if (!alreadyDone(error)) {
    // Almost always a missing nvs_flash_init(): Wi-Fi keeps its calibration
    // data there and will not start without it.
    HCritical("esp_wifi_init failed: %s - is NVS initialised?",
              esp_err_to_name(error));
    return false;
  }

  // RAM rather than flash: this node stores no access point credentials, and a
  // write to flash on every start is a wear cost for nothing.
  esp_wifi_set_storage(WIFI_STORAGE_RAM);

  error = esp_wifi_set_mode(WIFI_MODE_STA);
  if (!alreadyDone(error)) {
    HCritical("esp_wifi_set_mode failed: %s", esp_err_to_name(error));
    return false;
  }

  error = esp_wifi_start();
  if (!alreadyDone(error)) {
    HCritical("esp_wifi_start failed: %s", esp_err_to_name(error));
    return false;
  }

  // Order matters: the channel can only be set once the radio is running, and
  // esp_now_init() must come after esp_wifi_start() or it fails outright.
  error = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  if (error != ESP_OK) {
    HCritical("esp_wifi_set_channel(%u) failed: %s", channel,
              esp_err_to_name(error));
    return false;
  }

  error = esp_now_init();
  if (error != ESP_OK) {
    HCritical("esp_now_init failed: %s", esp_err_to_name(error));
    return false;
  }

  esp_now_register_recv_cb(recvTrampoline);
  esp_now_register_send_cb(sendTrampoline);

  uint8_t address[HAP_MAC_LEN] = {};
  error = esp_wifi_get_mac(WIFI_IF_STA, address);
  if (error != ESP_OK) {
    HCritical("esp_wifi_get_mac failed: %s", esp_err_to_name(error));
    return false;
  }

  mac_ = HAPMac::fromBytes(address);
  channel_ = channel;
  started_ = true;

  // Without this a node can unicast perfectly and never bind, because binding
  // is the one thing that must reach a stranger.
  if (!addEspNowPeer(HAPMac::broadcast(), nullptr)) {
    HCritical("could not add the broadcast peer - binding will never work");
    return false;
  }

  HInfo("up on channel %u as %s", channel, mac_.toString().c_str());
  return true;
}

uint8_t HAPEspNow::channel() const noexcept {
  return channel_;
}

const HAPMac& HAPEspNow::localMac() const noexcept {
  return mac_;
}

bool HAPEspNow::setChannel(uint8_t channel) {
  const esp_err_t error = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  if (error != ESP_OK) {
    HWarning("esp_wifi_set_channel(%u) failed: %s", channel,
             esp_err_to_name(error));
    return false;
  }

  channel_ = channel;
  return true;
}

bool HAPEspNow::setPrimaryKey(const uint8_t* pmk) noexcept {
  if (pmk == nullptr) {
    return false;
  }

  const esp_err_t error = esp_now_set_pmk(pmk);
  if (error != ESP_OK) {
    HCritical("esp_now_set_pmk failed: %s", esp_err_to_name(error));
    return false;
  }

  return true;
}

bool HAPEspNow::addEspNowPeer(const HAPMac& mac, const uint8_t* key) noexcept {
  esp_now_peer_info_t peer = {};
  std::memcpy(peer.peer_addr, mac.bytes, HAP_MAC_LEN);

  // Zero means "whatever channel the radio is on", which is what keeps a peer
  // table valid across a channel change.
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = key != nullptr;

  if (key != nullptr) {
    std::memcpy(peer.lmk, key, HAP_KEY_LEN);
  }

  const bool known = esp_now_is_peer_exist(mac.bytes);
  const esp_err_t error =
      known ? esp_now_mod_peer(&peer) : esp_now_add_peer(&peer);

  if (error != ESP_OK) {
    HWarning("%s peer %s failed: %s", known ? "modifying" : "adding",
             mac.toString().c_str(), esp_err_to_name(error));
    return false;
  }

  if (!known && !peers_.full()) {
    peers_.push_back(mac);
  }

  return true;
}

bool HAPEspNow::addPeer(const HAPMac& mac, const uint8_t* key) {
  if (!started_) {
    return false;
  }

  // The ceiling that decides how many children a node may have. Worth its own
  // line in a log, because the symptom otherwise is "the seventh device binds
  // and is never heard from".
  if (key != nullptr) {
    esp_now_peer_num_t count = {};
    if (esp_now_get_peer_num(&count) == ESP_OK &&
        count.encrypt_num >= ESP_NOW_MAX_ENCRYPT_PEER_NUM &&
        !esp_now_is_peer_exist(mac.bytes)) {
      HWarning("no room for another encrypted peer (%d in use)",
               count.encrypt_num);
      return false;
    }
  }

  return addEspNowPeer(mac, key);
}

bool HAPEspNow::removePeer(const HAPMac& mac) {
  const esp_err_t error = esp_now_del_peer(mac.bytes);

  for (auto it = peers_.begin(); it != peers_.end(); ++it) {
    if (*it == mac) {
      peers_.erase(it);
      break;
    }
  }

  return error == ESP_OK;
}

bool HAPEspNow::hasPeer(const HAPMac& mac) const noexcept {
  for (const HAPMac& peer : peers_) {
    if (peer == mac) {
      return true;
    }
  }

  return false;
}

bool HAPEspNow::send(const HAPMac& mac, const uint8_t* data, size_t size) {
  if (!started_ || data == nullptr || size == 0 || size > HAP_MAX_FRAME_SIZE) {
    return false;
  }

  const esp_err_t error = esp_now_send(mac.bytes, data, size);

  if (error != ESP_OK) {
    // ESP_ERR_ESPNOW_NOT_FOUND here means the address is not a peer. It is the
    // commonest mistake in an ESP-NOW network and deserves to be unmissable.
    HWarning("send to %s failed: %s", mac.toString().c_str(),
             esp_err_to_name(error));
    return false;
  }

  return true;
}

bool HAPEspNow::broadcast(const uint8_t* data, size_t size) {
  return send(HAPMac::broadcast(), data, size);
}

void HAPEspNow::onReceive(Receiver receiver) {
  receiver_ = receiver;
}

void HAPEspNow::handleReceive(const uint8_t* mac, const uint8_t* data, int size,
                              int8_t rssi) noexcept {
  // On the Wi-Fi task. Copy, and nothing else - no parsing, no logging, no
  // callbacks, and above all no blocking.
  if (mac == nullptr || data == nullptr || size <= 0 ||
      size > static_cast<int>(HAP_MAX_FRAME_SIZE)) {
    return;
  }

  etl::lock_guard<etl::mutex> guard(queueLock_);

  if (queue_.full()) {
    ++receiveOverflows_;
    return;
  }

  Entry entry;
  entry.from = HAPMac::fromBytes(mac);
  entry.rssi = rssi;
  entry.size = static_cast<uint8_t>(size);
  std::memcpy(entry.data, data, static_cast<size_t>(size));

  queue_.push_back(entry);
}

void HAPEspNow::onSendResult(SendResult result) {
  sendResult_ = result;
}

void HAPEspNow::handleSend(const uint8_t* mac, bool delivered) noexcept {
  if (!delivered) {
    ++sendFailures_;
  }

  // Still on the Wi-Fi task, so whoever installed this must keep it short. It
  // exists for one decision - retry, or switch a link's key - and neither needs
  // more than a few instructions.
  if (sendResult_.is_valid()) {
    sendResult_(HAPMac::fromBytes(mac), delivered);
  }
}

size_t HAPEspNow::update() noexcept {
  size_t delivered = 0;

  while (true) {
    Entry entry;

    {
      // The lock covers taking one frame off the queue and nothing more: the
      // receiver below may do real work, and holding a lock the Wi-Fi task
      // wants while it does would stall the radio.
      etl::lock_guard<etl::mutex> guard(queueLock_);

      if (queue_.empty()) {
        break;
      }

      entry = queue_.front();
      queue_.erase(queue_.begin());
    }

    lastRssi_ = entry.rssi;

    if (receiver_.is_valid()) {
      receiver_(entry.from, entry.data, entry.size);
    }

    ++delivered;
  }

  return delivered;
}

int8_t HAPEspNow::lastRssi() const noexcept {
  return lastRssi_;
}

uint32_t HAPEspNow::sendFailures() const noexcept {
  return sendFailures_;
}

uint32_t HAPEspNow::receiveOverflows() const noexcept {
  return receiveOverflows_;
}

#endif  // IS_MCU
