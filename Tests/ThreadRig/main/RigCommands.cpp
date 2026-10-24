#include "RigCommands.hpp"

#include "RigThread.hpp"

#include <HAP.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_console.h"
#include "openthread/ip6.h"

namespace rig {

namespace {

// Commands run one at a time on the console task, so one buffer will do.
uint8_t gPayload[HAP_MAX_FRAME_SIZE];

int fail(const char* reason) {
  std::printf("HAPERR %s\n", reason);
  return 0;
}

/** "master", or an IPv6 address copied 1:1 into the 16 bytes of a HAPAddress. */
bool parseDestination(const char* text, HAPAddress& out) {
  if (std::strcmp(text, "master") == 0) {
    out = HAPAddress::master();
    return true;
  }
  otIp6Address ip;
  if (otIp6AddressFromString(text, &ip) != OT_ERROR_NONE) return false;
  out = HAPAddress::fromBytes(ip.mFields.m8);
  return true;
}

bool parseNumber(const char* text, unsigned long& out) {
  char* end = nullptr;
  out = std::strtoul(text, &end, 10);
  return end != text && *end == '\0';
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/** The arguments from @p first on, joined with single spaces. */
bool joinText(int argc, char** argv, int first, size_t& size) {
  size = 0;
  for (int i = first; i < argc; ++i) {
    const size_t length = std::strlen(argv[i]);
    const size_t need = length + (i > first ? 1 : 0);
    if (size + need > sizeof(gPayload)) return false;
    if (i > first) gPayload[size++] = ' ';
    std::memcpy(gPayload + size, argv[i], length);
    size += length;
  }
  return true;
}

int commandSend(int argc, char** argv) {
  if (argc < 4) return fail("usage: hap send <ipv6|master> <text>");
  HAPAddress to;
  if (!parseDestination(argv[2], to)) return fail("bad address");
  size_t size = 0;
  if (!joinText(argc, argv, 3, size)) return fail("text too long");
  send(to, gPayload, size);
  return 0;
}

int commandSendHex(int argc, char** argv) {
  if (argc != 4) return fail("usage: hap sendhex <ipv6|master> <hex>");
  HAPAddress to;
  if (!parseDestination(argv[2], to)) return fail("bad address");

  const char* hex = argv[3];
  const size_t digits = std::strlen(hex);
  if (digits % 2 != 0 || digits / 2 > sizeof(gPayload)) return fail("bad hex");
  for (size_t i = 0; i < digits / 2; ++i) {
    const int high = hexValue(hex[2 * i]);
    const int low = hexValue(hex[2 * i + 1]);
    if (high < 0 || low < 0) return fail("bad hex");
    gPayload[i] = static_cast<uint8_t>(high << 4 | low);
  }
  send(to, gPayload, digits / 2);
  return 0;
}

int commandMulticast(int argc, char** argv) {
  if (argc < 3) return fail("usage: hap mcast <text>");
  size_t size = 0;
  if (!joinText(argc, argv, 2, size)) return fail("text too long");
  multicast(gPayload, size);
  return 0;
}

int commandBurst(int argc, char** argv) {
  if (argc < 5 || argc > 6) {
    return fail("usage: hap burst <ipv6|master|mcast> <count> <size> [intervalMs]");
  }
  const bool toMulticast = std::strcmp(argv[2], "mcast") == 0;
  HAPAddress to;
  if (!toMulticast && !parseDestination(argv[2], to)) return fail("bad address");

  unsigned long count = 0;
  unsigned long size = 0;
  unsigned long interval = 0;
  if (!parseNumber(argv[3], count) || count == 0) return fail("bad count");
  if (!parseNumber(argv[4], size) || size < 6 || size > HAP_MAX_FRAME_SIZE) {
    return fail("bad size");
  }
  if (argc == 6 && !parseNumber(argv[5], interval)) return fail("bad interval");

  if (!startBurst(toMulticast, to, static_cast<uint32_t>(count),
                  static_cast<uint16_t>(size), static_cast<uint32_t>(interval))) {
    return fail("busy");
  }
  return 0;
}

int commandStall(int argc, char** argv) {
  unsigned long ms = 0;
  if (argc != 3 || !parseNumber(argv[2], ms)) return fail("usage: hap stall <ms>");
  stall(static_cast<uint32_t>(ms));
  return 0;
}

int command(int argc, char** argv) {
  if (argc < 2) return fail("usage: hap info|send|sendhex|mcast|burst|stall");
  const char* name = argv[1];
  if (std::strcmp(name, "info") == 0) {
    printInfo();
    return 0;
  }
  if (std::strcmp(name, "send") == 0) return commandSend(argc, argv);
  if (std::strcmp(name, "sendhex") == 0) return commandSendHex(argc, argv);
  if (std::strcmp(name, "mcast") == 0) return commandMulticast(argc, argv);
  if (std::strcmp(name, "burst") == 0) return commandBurst(argc, argv);
  if (std::strcmp(name, "stall") == 0) return commandStall(argc, argv);
  return fail("unknown command");
}

}  // namespace

void registerCommands() {
  esp_console_cmd_t cmd = {};
  cmd.command = "hap";
  cmd.help = "HAP rig: info | send | sendhex | mcast | burst | stall";
  cmd.func = command;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

}  // namespace rig
