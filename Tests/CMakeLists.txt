#######################################################################
# HAPLib host tests.
#
# A standalone CMake project, built with whatever compiler the machine has:
#
#   cmake -S HAPLib/Tests -B build-haptests
#   cmake --build build-haptests
#   ctest --test-dir build-haptests --output-on-failure
#
# It pulls HLib and HAPLib in as ordinary subdirectories - the same sources the
# firmware is built from, not a reimplementation of them - and points HLib at
# the test-owned Config/ rather than the application's.
#######################################################################

cmake_minimum_required(VERSION 3.16)
project(HAPLibTests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Cached, because HLib's CMakeLists reads it from its own directory scope.
set(HLIB_CONFIG_DIR "${CMAKE_CURRENT_LIST_DIR}/Config" CACHE INTERNAL
    "Directory holding the test build's HLibConfig.h")

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../HLib" HLib)
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
