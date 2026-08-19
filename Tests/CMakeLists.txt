#######################################################################
# HAPLib host tests.
#
# A standalone CMake project, built with whatever compiler the machine has:
#
#   git submodule update --init --recursive
#   cmake -S HAPLib/Tests -B build-haptests
#   cmake --build build-haptests
#   ctest --test-dir build-haptests --output-on-failure
#
# It pulls HCoreLib and HAPLib in as ordinary subdirectories - the same sources
# the firmware is built from, not a reimplementation of them - and points
# HCoreLib at the test-owned Config/ rather than the application's.
#
# HCoreLib is a submodule of THIS DIRECTORY rather than of the library, and that
# placement is the whole point. HAPLib only ever DECLARES the dependency - see
# ../CMakeLists.txt, which links HCoreLib without ever adding it - so an
# application remains free to build HAPLib against whichever HCoreLib revision
# it already has, and two copies of the target can never collide in one build.
# Only this test project resolves it, because a test project is an application
# and an application is the thing entitled to pin a version.
#######################################################################

cmake_minimum_required(VERSION 3.16)
project(HAPLibTests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The submodule by default - a sibling checkout would make the suite depend on
# how a developer happened to arrange their disk. Overridable, because "which
# HCoreLib does HAPLib work against?" is a question worth being able to ask of a
# revision other than the pinned one:
#
#   cmake -S Tests -B build -DHCORELIB_DIR=/path/to/another/HCoreLib
set(HCORELIB_DIR "${CMAKE_CURRENT_LIST_DIR}/HCoreLib" CACHE PATH
    "HCoreLib source directory. Override to test against another revision.")

if(NOT EXISTS "${HCORELIB_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "No HCoreLib in ${HCORELIB_DIR}. It is a submodule - run:\n"
    "  git submodule update --init --recursive")
endif()

# Cached, because HCoreLib's CMakeLists reads it from its own directory scope.
set(HCORELIB_CONFIG_DIR "${CMAKE_CURRENT_LIST_DIR}/Config" CACHE INTERNAL
    "Directory holding the test build's HCoreLibConfig.h")

add_subdirectory("${HCORELIB_DIR}" HCoreLib)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/.." HAPLib)

add_executable(haptests
    "HAPTest.cpp"
    "HAPPathTest.cpp"
    "HAPCodecTest.cpp"
    "HAPFrameTest.cpp"
    "HAPMessagesTest.cpp"
    "HAPRouterTest.cpp"
    "HAPTreeTest.cpp"
    "HAPNodeTest.cpp"
    "HAPBinderTest.cpp"
    "HAPStoreTest.cpp"
    "HAPReportTest.cpp"
    "HAPModelTest.cpp"
    "HAPLinksTest.cpp"
    "HAPStackTest.cpp"
    "HAPSpecTest.cpp")

target_link_libraries(haptests PRIVATE HAPLib)

if(MSVC)
  # /utf-8 is not optional here: HAPSpecTest.cpp contains Cyrillic string
  # literals whose exact UTF-8 bytes are the thing under test, and MSVC would
  # otherwise read the source in the system code page and encode them as
  # something else entirely.
  target_compile_options(haptests PRIVATE /W4 /utf-8)
else()
  target_compile_options(haptests PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME haptests COMMAND haptests)
