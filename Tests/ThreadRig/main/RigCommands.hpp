#pragma once

/**
 * @file RigCommands.hpp
 * @brief The `hap` console command: info, send, sendhex, mcast, burst, stall.
 */

namespace rig {

/** Registers `hap` with the console. Call after the REPL exists, before it starts. */
void registerCommands();

}  // namespace rig
