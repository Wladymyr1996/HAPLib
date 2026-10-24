#include <HAPITransport/HAPThread/HAPOpenThreadEsp.hpp>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"

#if CONFIG_OPENTHREAD_ENABLED

#include <cstring>
#include <new>

#include <esp_openthread.h>
#include <esp_openthread_lock.h>
#include <freertos/FreeRTOS.h>
#include <openthread/instance.h>
#include <openthread/ip6.h>
#include <openthread/link.h>
#include <openthread/message.h>
#include <openthread/netdata.h>
#include <openthread/thread.h>
#include <openthread/udp.h>

namespace {

constexpr TickType_t kLockWait = pdMS_TO_TICKS(100);

/** The OpenThread lock for one call. It is recursive, so the OpenThread task may hold it too. */
class Lock {
 public:
  Lock() noexcept : held_(esp_openthread_lock_acquire(kLockWait)) {}
  ~Lock() {
    if (held_) {
      esp_openthread_lock_release();
    }
  }

  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;

  bool held() const noexcept { return held_; }

 private:
  bool held_;
};

/** The running instance, or null before the application started OpenThread. */
otInstance* instance() noexcept {
  return esp_openthread_get_instance();
}

HAPAddress toHap(const otIp6Address& address) noexcept {
  return HAPAddress::fromBytes(address.mFields.m8);
}

void toOt(const HAPAddress& address, otIp6Address& out) noexcept {
  std::memcpy(out.mFields.m8, address.bytes, sizeof(out.mFields.m8));
}

}  // namespace

struct HAPOpenThreadEsp::Socket {
  otUdpSocket udp;
  HAPOpenThreadEsp* owner;

  /** Runs on the OpenThread task, with the lock held. */
  static void onReceive(void* context, otMessage* message,
                        const otMessageInfo* info) {
    Socket* self = static_cast<Socket*>(context);
    HAPOpenThreadEsp& owner = *self->owner;

    // The payload starts after the UDP header, which the offset skips.
    const uint16_t offset = otMessageGetOffset(message);
    const uint16_t length = otMessageGetLength(message);
    const size_t size = length > offset ? length - offset : 0;
    const size_t read = size < sizeof(owner.rx_) ? size : sizeof(owner.rx_);

    otMessageRead(message, offset, owner.rx_, static_cast<uint16_t>(read));

    if (owner.handler_.is_valid()) {
      owner.handler_(toHap(info->mPeerAddr), owner.rx_, size);
    }
  }
};

HAPOpenThreadEsp::HAPOpenThreadEsp() noexcept = default;

HAPOpenThreadEsp::~HAPOpenThreadEsp() {
  close();
}

bool HAPOpenThreadEsp::open(uint16_t port, Handler handler) {
  if (socket_ != nullptr) {
    return true;
  }

  Lock lock;
  otInstance* ot = instance();

  if (!lock.held() || ot == nullptr) {
    return false;
  }

  Socket* socket = new (std::nothrow) Socket();

  if (socket == nullptr) {
    return false;
  }

  std::memset(&socket->udp, 0, sizeof(socket->udp));
  socket->owner = this;
  handler_ = handler;

  otSockAddr address;
  std::memset(&address, 0, sizeof(address));
  address.mPort = port;

  if (otUdpOpen(ot, &socket->udp, &Socket::onReceive, socket) != OT_ERROR_NONE) {
    delete socket;
    return false;
  }

  if (otUdpBind(ot, &socket->udp, &address, OT_NETIF_UNSPECIFIED) !=
      OT_ERROR_NONE) {
    otUdpClose(ot, &socket->udp);
    delete socket;
    return false;
  }

  socket_ = socket;
  port_ = port;
  return true;
}

void HAPOpenThreadEsp::close() {
  if (socket_ == nullptr) {
    return;
  }

  Lock lock;
  otInstance* ot = instance();

  // Without the lock the socket stays open and keeps its memory: closing it
  // unlocked would race the OpenThread task, and nothing here could report it.
  if (!lock.held() || ot == nullptr) {
    return;
  }

  otUdpClose(ot, &socket_->udp);
  delete socket_;
  socket_ = nullptr;
}

bool HAPOpenThreadEsp::isAttached() {
  Lock lock;
  otInstance* ot = instance();

  if (!lock.held() || ot == nullptr) {
    return false;
  }

  switch (otThreadGetDeviceRole(ot)) {
    case OT_DEVICE_ROLE_CHILD:
    case OT_DEVICE_ROLE_ROUTER:
    case OT_DEVICE_ROLE_LEADER:
      return true;
    default:
      return false;
  }
}

bool HAPOpenThreadEsp::meshLocalEid(HAPAddress& out) {
  Lock lock;
  otInstance* ot = instance();

  if (!lock.held() || ot == nullptr) {
    return false;
  }

  const otIp6Address* eid = otThreadGetMeshLocalEid(ot);

  if (eid == nullptr || otIp6IsAddressUnspecified(eid)) {
    return false;
  }

  out = toHap(*eid);
  return true;
}

HAPNodeId HAPOpenThreadEsp::factoryId() {
  Lock lock;
  otInstance* ot = instance();

  if (!lock.held() || ot == nullptr) {
    return HAPNodeId::zero();
  }

  otExtAddress eui64;
  otLinkGetFactoryAssignedIeeeEui64(ot, &eui64);
  return HAPNodeId::fromBytes(eui64.m8);
}

bool HAPOpenThreadEsp::serviceAloc(uint32_t enterprise, const uint8_t* data,
                                   uint8_t size, HAPAddress& out) {
  Lock lock;
  otInstance* ot = instance();

  if (!lock.held() || ot == nullptr) {
    return false;
  }

  otNetworkDataIterator iterator = OT_NETWORK_DATA_ITERATOR_INIT;
  otServiceConfig service;

  while (otNetDataGetNextService(ot, &iterator, &service) == OT_ERROR_NONE) {
    if (service.mEnterpriseNumber != enterprise ||
        service.mServiceDataLength != size ||
        std::memcmp(service.mServiceData, data, size) != 0) {
      continue;
    }

    otIp6Address aloc;

    // All entries of one service share an ALOC, so the first match is as good
    // as any.
    if (otThreadGetServiceAloc(ot, service.mServiceId, &aloc) != OT_ERROR_NONE) {
      return false;
    }

    out = toHap(aloc);
    return true;
  }

  return false;
}

bool HAPOpenThreadEsp::isLocalAddress(const HAPAddress& address) {
  Lock lock;
  otInstance* ot = instance();

  if (!lock.held() || ot == nullptr) {
    return false;
  }

  otIp6Address ip6;
  toOt(address, ip6);
  return otIp6HasUnicastAddress(ot, &ip6);
}

bool HAPOpenThreadEsp::sendTo(const HAPAddress& to, const HAPAddress& from,
                              const uint8_t* data, size_t size) {
  Lock lock;
  otInstance* ot = instance();

  if (!lock.held() || ot == nullptr || socket_ == nullptr) {
    return false;
  }

  otMessage* message = otUdpNewMessage(ot, nullptr);

  if (message == nullptr) {
    return false;
  }

  if (otMessageAppend(message, data, static_cast<uint16_t>(size)) !=
      OT_ERROR_NONE) {
    otMessageFree(message);
    return false;
  }

  otMessageInfo info;
  std::memset(&info, 0, sizeof(info));
  toOt(to, info.mPeerAddr);
  info.mPeerPort = port_;

  // Pinning the source is what keeps `from` the sender's ML-EID when the
  // destination is an ALOC, where the stack would pick the RLOC.
  toOt(from, info.mSockAddr);
  info.mSockPort = port_;
  info.mMulticastLoop = false;

  if (otUdpSend(ot, &socket_->udp, message, &info) != OT_ERROR_NONE) {
    otMessageFree(message);
    return false;
  }

  return true;
}

#endif  // CONFIG_OPENTHREAD_ENABLED
#endif  // ESP_PLATFORM
