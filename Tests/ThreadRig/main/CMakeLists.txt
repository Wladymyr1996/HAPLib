idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       # HAPLib is the transport under test; HCoreLib comes with
                       # it. The rest is bring-up: nvs and the event loop for
                       # OpenThread's platform, vfs for its eventfd, console and
                       # the USB driver for the `ot` and `hap` commands.
                       REQUIRES HCoreLib HAPLib openthread nvs_flash esp_netif
                                esp_event vfs console esp_timer
                                esp_driver_usb_serial_jtag)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
