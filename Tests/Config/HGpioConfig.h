#pragma once

/**
 * @file HGpioConfig.h
 * @brief A pin table for the host test build.
 *
 * HGpioManager is compiled into HCoreLib whether or not a test touches a pin,
 * and it cannot invent a board - so a table has to exist. One input is enough to
 * satisfy it; nothing in HAPLib's tests reads a pin.
 */

#define HGPIO_FIXED_PINS(PIN)                 \
  /*  name,  gpio,  dir,    pull,  invert */  \
  PIN("btn",    2,  Input,  Up,    true)

#define HGPIO_CONFIGURABLE_PINS(PIN)
