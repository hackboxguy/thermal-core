# Stage 13 13b -- glob core/ + protocol/ into the ESP32-C3 firmware.
# The same portable C99 source that builds the Linux daemon links
# unchanged here.  protocol/ is needed because core/thermal_commands.h
# has _Static_assert pins against protocol/thermal_wire_opcodes.h
# (linker dead-strips unused encoders/decoders at runtime).

file(GLOB CORE_SRCS     "${PROJECT_DIR}/../../core/*.c")
file(GLOB PROTOCOL_SRCS "${PROJECT_DIR}/../../protocol/*.c")

# tools/json2static.py converts the daemon-shape JSON config into a
# const thermal_config_t.  Regenerate on every config or script edit.
# IDF's component_get_requirements pass forbids add_custom_command;
# guard with CMAKE_BUILD_EARLY_EXPANSION which is TRUE in that pass
# and unset during the real build.
set(GENERATED_DIR      "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(GENERATED_CONFIG_C "${GENERATED_DIR}/esp32_config.c")
file(MAKE_DIRECTORY "${GENERATED_DIR}")    # exists before idf_component_register
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_custom_command(
        OUTPUT  "${GENERATED_CONFIG_C}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND python3 "${PROJECT_DIR}/../../tools/json2static.py"
                "${PROJECT_DIR}/configs/esp32-c3-standalone.json"
                -o "${GENERATED_CONFIG_C}"
                --symbol G_THERMAL_CFG
        DEPENDS "${PROJECT_DIR}/configs/esp32-c3-standalone.json"
                "${PROJECT_DIR}/../../tools/json2static.py"
        COMMENT "Generating ESP32 thermal_config_t from JSON"
        VERBATIM)
    set_source_files_properties("${GENERATED_CONFIG_C}"
                                PROPERTIES GENERATED TRUE)
endif()

idf_component_register(
    SRCS
        "main.c"
        "bsp_esp32_pwm.c"
        "bsp_esp32_tach.c"
        "bsp_esp32_sensor.c"
        "esp32_callbacks.c"
        ${CORE_SRCS}
        ${PROTOCOL_SRCS}
        "${GENERATED_CONFIG_C}"
    INCLUDE_DIRS
        "."
        "${PROJECT_DIR}/../../core"
        "${PROJECT_DIR}/../../protocol"
        "${GENERATED_DIR}"
)
