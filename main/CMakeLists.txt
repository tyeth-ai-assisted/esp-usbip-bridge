set(req_components esp_event esp_netif esp_timer lwip nvs_flash usb mdns esp_driver_i2c
                   harness_io harness_scpi harness_dut harness_io_expander)

set(srcs
    "main.c"
    "device_naming.c"
    "discovery_service.c"
    "network_init.c"
    "usb_backend.c"
    "log_server.c"
    "usbip_server.c"
    "http_server.c"
    "led_strip_encoder.c"
    "neopixel.c"
    "hub_control.c"
    "analog_mux.c"
    "controller_api.c"
)

if(CONFIG_USBIP_VIRTUAL_LOGIC_ANALYZER OR CONFIG_USBIP_VIRTUAL_HARNESS)
    list(APPEND srcs
        "virtual_device.c"
        "virtual_cdc.c"
    )
endif()

if(CONFIG_USBIP_VIRTUAL_LOGIC_ANALYZER)
    list(APPEND req_components sump sump_io logic_capture perfetto_trace)
    list(APPEND srcs
        "virtual_perfetto_logic.c"
    )
endif()

if(CONFIG_USBIP_VIRTUAL_HARNESS)
    list(APPEND req_components harness_io_expander harness_gpio harness_bus)
    list(APPEND srcs
        "virtual_harness.c"
        "scpi_controller.c"
    )
endif()

if(IDF_TARGET STREQUAL "esp32p4" OR IDF_TARGET STREQUAL "esp32s31")
    list(APPEND req_components esp_eth)
elseif(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND req_components esp_wifi esp_driver_gpio)
endif()

# Always compiled; neopixel_init() becomes a no-op when gpio < 0.
list(APPEND req_components esp_driver_rmt)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${req_components}
    EMBED_TXTFILES "web_app.js"
)

# Syntax-check the embedded browser JS at build time when node is available.
# This is best-effort: hosts without node simply skip the check so CI and
# local builds stay portable.  Run `node --check main/web_app.js` manually
# to validate when node is absent.
find_program(NODE_JS node)
if(NODE_JS)
    add_custom_target(main_jscheck
        COMMAND "${NODE_JS}" --check "${CMAKE_CURRENT_SOURCE_DIR}/web_app.js"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web_app.js"
        COMMENT "Syntax-checking web_app.js"
        VERBATIM)
    add_dependencies(${COMPONENT_LIB} main_jscheck)
endif()
