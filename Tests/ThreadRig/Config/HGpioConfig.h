#pragma once

/**
 * @file HGpioConfig.h
 * @brief An empty pin table for the Thread rig image.
 *
 * HGpioManager is compiled into HCoreLib whether or not anything uses it, and
 * it needs a table to exist. The rig drives no pin, so both lists are empty.
 */

#define HGPIO_FIXED_PINS(PIN)

#define HGPIO_CONFIGURABLE_PINS(PIN)
