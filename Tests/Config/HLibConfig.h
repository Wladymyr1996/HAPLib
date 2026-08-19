#pragma once

/**
 * @file HLibConfig.h
 * @brief HLib's tuning header, for the host test build only.
 *
 * HLib demands this file and fails the build without one. The tests supply
 * their own rather than borrowing the application's, because a library test
 * that reaches into App/ has quietly made the library depend on an application
 * - which is the one thing HLib is not allowed to do.
 *
 * Only what the tests actually need is here; every other limit keeps the
 * default declared beside the module that consumes it.
 */

/**
 * Matched to the device (App/Config/HLibConfig.h) on purpose.
 *
 * HAPReader truncates an over-long String value to this, so a test that ran
 * with a different limit would be testing different behaviour from the
 * firmware's.
 */
#define HVALUE_MAX_STRING_LEN 64
