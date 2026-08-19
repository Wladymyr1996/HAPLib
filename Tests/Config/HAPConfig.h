#pragma once

/**
 * @file HAPConfig.h
 * @brief HAPLib's optional tuning header, for the host test build.
 *
 * Unlike HLib's, this file is optional - HAPLib runs on its defaults, and
 * HAP.h picks it up only if it exists. Its being here is also the proof that
 * mechanism works.
 *
 * Only CADENCES are shortened. Nothing below changes a rule, a limit or a byte
 * on the wire: a test that ran against a different protocol would be testing
 * something no device does.
 *
 * The reason is that a handshake is full of "and then wait": between
 * announcements, on each channel of a sweep, for a confirmation that never
 * comes. At the real intervals a dozen bind tests would take half a minute of
 * genuine waiting, and a suite nobody runs because it is slow is a suite that
 * stops catching things.
 */

/** 500 ms in the field. Between one announcement and the next. */
#define HAP_ANNOUNCE_PERIOD_MS 20

/** 1500 ms in the field. How long a sweep waits on one channel. */
#define HAP_SWEEP_DWELL_MS 60

/** 2000 ms in the field. How long a master waits to be confirmed. */
#define HAP_CONFIRM_TIMEOUT_MS 100

/** 200 ms in the field. The fallback when a transport reports no send. */
#define HAP_KEY_SWITCH_TIMEOUT_MS 50

// HAP_BIND_WINDOW_MS is deliberately left at its real 60 s. No test waits for
// it - a window closing is checked by driving the state directly - and having
// one number here that matches the device keeps it obvious that these are
// cadences rather than a different protocol.
