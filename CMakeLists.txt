#######################################################################
# HAPLib - the HatynkaAirProtocol, built on HLib.
#
# Like HLib it builds either as an ESP-IDF component or as a plain static
# library for host tests, and for the same reason: almost all of this library
# is logic - addressing, framing, routing, binding, links - that a desktop can
# exercise far more thoroughly than two boards on a bench can.
#
# The radio lives behind HAPITransport with exactly one backend compiled in:
# ESP-NOW on target, an in-process simulated bus on the host. That seam is what
# lets a whole tree of nodes be built inside one unit test.
#
# See Docs/ImplementingStatus.md for what is built so far.
#######################################################################

set(HAPLIB_SRCS
    "HAP.cpp"
    "HAPBinder/HAPBinder.cpp"
    "HAPClasses/HAPClasses.cpp"
    "HAPCodec/HAPCodec.cpp"
    "HAPCrc16/HAPCrc16.cpp"
    "HAPFrame/HAPFrame.cpp"
    "HAPInstance/HAPInstance.cpp"
    "HAPITransport/HAPITransport.cpp"
    "HAPLinks/HAPLinks.cpp"
    "HAPListenWindow/HAPListenWindow.cpp"
    "HAPMac/HAPMac.cpp"
    "HAPModel/HAPModel.cpp"
    "HAPNode/HAPNode.cpp"
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
    "HAPPath/HAPPath.cpp")

# "." serves <HAP.h> and the <HAPFrame/HAPFrame.hpp> spelling every module uses.
set(HAPLIB_INCLUDE_DIRS ".")

# Exactly one transport backend is compiled in, the way HLib chooses a
# filesystem: ESP-NOW on target, and on the host a bus made of memory that a
# whole tree of simulated nodes attaches to. The interface is virtual - unlike
# HLib's platform seams - because a test has several transports alive at once.
if(ESP_PLATFORM)
  list(APPEND HAPLIB_SRCS "HAPITransport/HAPEspNow/HAPEspNow.cpp")
else()
  list(APPEND HAPLIB_SRCS "HAPITransport/HAPLoopback/HAPLoopback.cpp")
endif()

if(ESP_PLATFORM)
  # esp_wifi is PRIVATE: HAPEspNow.hpp deliberately declares no ESP type, so the
  # radio's headers stop at this component's own translation unit and nothing
  # including a HAPLib header inherits them.
  idf_component_register(SRCS ${HAPLIB_SRCS}
                         INCLUDE_DIRS ${HAPLIB_INCLUDE_DIRS}
                         REQUIRES HLib
                         PRIV_REQUIRES esp_wifi esp_netif esp_event)

  target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
else()
  cmake_minimum_required(VERSION 3.16)

  add_library(HAPLib STATIC ${HAPLIB_SRCS})
  add_library(HAPLib::HAPLib ALIAS HAPLib)

  target_include_directories(HAPLib PUBLIC ${HAPLIB_INCLUDE_DIRS})
  target_compile_features(HAPLib PUBLIC cxx_std_17)

  # PUBLIC: HAP.h includes <HLib.h>, and HAPCodec puts HValue in its interface,
  # so anything including a HAPLib header needs HLib's headers too.
  target_link_libraries(HAPLib PUBLIC HLib)
endif()
