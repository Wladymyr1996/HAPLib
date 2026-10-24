#######################################################################
# HAPThreadRig - the Thread bench rig. A TEST image, not a product.
#
# One firmware for every board of the bench: it brings OpenThread up on a
# hardcoded rig network, takes the OpenThread CLI (`ot <line>`) on the USB
# console, and links HAPLib so the transport under test runs on the real radio.
#
#   unset MSYSTEM                                  # Git Bash only
#   . C:/esp/v5.5.3/esp-idf/export.sh
#   idf.py -C Tests/ThreadRig -B <build dir> \
#       -DHCORELIB_DIR=<path to HCoreLib> set-target esp32c6 build
#######################################################################

cmake_minimum_required(VERSION 3.16)

# HCoreLib is a submodule of the host tests (Tests/HCoreLib) and that is the
# default here too, so the rig and the suite build against the same revision.
# Overridable for a checkout that keeps it elsewhere:
#
#   idf.py ... -DHCORELIB_DIR=/path/to/HCoreLib
set(HCORELIB_DIR "${CMAKE_CURRENT_LIST_DIR}/../HCoreLib" CACHE PATH
    "HCoreLib source directory. Override to build against another checkout.")

if(NOT EXISTS "${HCORELIB_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "No HCoreLib in ${HCORELIB_DIR}. It is a submodule - run:\n"
    "  git submodule update --init --recursive\n"
    "or point the build at another checkout with -DHCORELIB_DIR=<path>.")
endif()

# IDF names a component after its directory, and main/ asks for "HAPLib" by that
# name - so a clone under another folder name would fail with an error that does
# not say why.
get_filename_component(HAPLIB_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(HAPLIB_DIR_NAME "${HAPLIB_DIR}" NAME)
if(NOT HAPLIB_DIR_NAME STREQUAL "HAPLib")
  message(FATAL_ERROR
    "The repository folder is called '${HAPLIB_DIR_NAME}', but it is an IDF "
    "component and IDF names a component after its folder. Rename it to HAPLib.")
endif()

# Cached, because IDF registers each component in its own scope: an ordinary
# variable set here would not survive the trip into HCoreLib's CMakeLists.
set(HCORELIB_CONFIG_DIR "${CMAKE_CURRENT_LIST_DIR}/Config" CACHE INTERNAL
    "Directory holding the rig's HCoreLibConfig.h")

set(EXTRA_COMPONENT_DIRS "${HAPLIB_DIR}" "${HCORELIB_DIR}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(HAPThreadRig)
