#pragma once

/**
 * @file HCoreLibConfig.h
 * @brief HCoreLib's tuning header, for the Thread rig image only.
 *
 * HCoreLib demands this file and fails the build without one. The rig keeps its
 * own rather than borrowing an application's: it runs no product code.
 */

/** Matched to the host tests (Tests/Config/HCoreLibConfig.h). */
#define HVALUE_MAX_STRING_LEN 64
