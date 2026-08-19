#include <HAPCrc16/HAPCrc16.hpp>

uint16_t HAPCrc16Update(uint16_t crc, const uint8_t* data, size_t size) noexcept {
  if (data == nullptr) {
    return crc;
  }

  // Bitwise rather than table-driven: a 512-byte table is a poor trade for
  // something that runs over a descriptor of a couple of hundred bytes, a few
  // times a minute at most.
  for (size_t i = 0; i < size; ++i) {
    crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(data[i]) << 8));

    for (uint8_t bit = 0; bit < 8; ++bit) {
      if ((crc & 0x8000) != 0) {
        crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
      } else {
        crc = static_cast<uint16_t>(crc << 1);
      }
    }
  }

  return crc;
}

uint16_t HAPCrc16(const uint8_t* data, size_t size) noexcept {
  return HAPCrc16Update(kHAPCrc16Init, data, size);
}
