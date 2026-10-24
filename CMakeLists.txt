#######################################################################
# HAPLib - the HatynkaAirProtocol, built on HCoreLib.
#
# Like HCoreLib it builds either as an ESP-IDF component or as a plain static
# library for host tests, and for the same reason: almost all of this library
# is logic - addressing, framing, routing, binding, links - that a desktop can
# exercise far more thoroughly than two boards on a bench can.
#
# The radio lives behind HAPITransport with exactly one backend compiled in:
# an in-process simulated bus on the host, and nothing on target until the
# Thread backend arrives. That seam is what lets a whole tree of nodes be built
# inside one unit test.
#
# See Docs/ImplementingStatus.md for what is built so far.
#######################################################################

set(HAPLIB_SRCS
    "HAP.cpp"
    "HAPAddress/HAPAddress.cpp"
    "HAPBinder/HAPBinder.cpp"
    "HAPClasses/HAPClasses.cpp"
    "HAPCodec/HAPCodec.cpp"
    "HAPCrc16/HAPCrc16.cpp"
    "HAPFrame/HAPFrame.cpp"
    "HAPInstance/HAPInstance.cpp"
    "HAPITransport/HAPITransport.cpp"
    "HAPLinks/HAPLinks.cpp"
    "HAPListenWindow/HAPListenWindow.cpp"
    "HAPModel/HAPModel.cpp"
    "HAPNode/HAPNode.cpp"
    "HAPNodeId/HAPNodeId.cpp"
    "HAPQueue/HAPQueue.cpp"
    "HAPRandom/HAPRandom.cpp"
    "HAPReporter/HAPReporter.cpp"
    "HAPRouter/HAPRouter.cpp"
    "HAPStack/HAPStack.cpp"
    "HAPStore/HAPStore.cpp"
    "HAPMessages/HAPBindMessages.cpp"
    "HAPMessages/HAPControlMessages.cpp"
    "HAPMessages/HAPDataMessages.cpp"
    "HAPMessages/HAPLinkMessages.cpp"
    "HAPMessages/HAPMessageParts.cpp"
    "HAPMessages/HAPServiceMessages.cpp"
    "HAPPath/HAPPath.cpp")

# "." serves <HAP.h> and the <HAPFrame/HAPFrame.hpp> spelling every module uses.
set(HAPLIB_INCLUDE_DIRS ".")

# Exactly one transport backend is compiled in, the way HCoreLib chooses a
# filesystem: on the host a bus made of memory that a whole tree of simulated
# nodes attaches to, and on target none yet - the Thread backend adds itself
# here. The interface is virtual - unlike HCoreLib's platform seams - because a
# test has several transports alive at once.
if(NOT ESP_PLATFORM)
  list(APPEND HAPLIB_SRCS "HAPITransport/HAPLoopback/HAPLoopback.cpp")
endif()

if(ESP_PLATFORM)
  idf_component_register(SRCS ${HAPLIB_SRCS}
                         INCLUDE_DIRS ${HAPLIB_INCLUDE_DIRS}
                         REQUIRES HCoreLib)

  target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
else()
  cmake_minimum_required(VERSION 3.16)

  add_library(HAPLib STATIC ${HAPLIB_SRCS})
  add_library(HAPLib::HAPLib ALIAS HAPLib)

  target_include_directories(HAPLib PUBLIC ${HAPLIB_INCLUDE_DIRS})
  target_compile_features(HAPLib PUBLIC cxx_std_17)

  # PUBLIC: HAP.h includes <HCoreLib.h>, and HAPCodec puts HValue in its
  # interface, so anything including a HAPLib header needs HCoreLib's headers
  # too.
  target_link_libraries(HAPLib PUBLIC HCoreLib)
endif()
