# Stage 13 13b -- glob core/ + protocol/ into the ESP32-C3 firmware.
# The same portable C99 source that builds the Linux daemon links
# unchanged here.  protocol/ is needed because core/thermal_commands.h
# has _Static_assert pins against protocol/thermal_wire_opcodes.h
# (linker dead-strips unused encoders/decoders at runtime).
#
# Stage 13 13c -- adds the THERMALCORE_REPLAY_STANDALONE option
# (default OFF).  Invoke as
#   idf.py -DTHERMALCORE_REPLAY_STANDALONE=ON build
# to compile the fixture-driven REPLAY firmware.  The option
# defines THERMALCORE_MODE_REPLAY_STANDALONE on the component's
# compile line; main.c + esp32_callbacks.c branch on it.  The
# replay_fixture.c source is linked unconditionally; LTO dead-
# strips it under STANDALONE since nothing references G_REPLAY_TICKS.

option(THERMALCORE_REPLAY_STANDALONE
       "Build firmware in REPLAY_STANDALONE mode (fixture-driven, no hardware)"
       OFF)

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
        "replay_fixture.c"
        ${CORE_SRCS}
        ${PROTOCOL_SRCS}
        "${GENERATED_CONFIG_C}"
    INCLUDE_DIRS
        "."
        "${PROJECT_DIR}/../../core"
        "${PROJECT_DIR}/../../protocol"
        "${GENERATED_DIR}"
)

# REPLAY-mode compile flag.  Component-scoped so it only
# affects this firmware's sources (not the IDF runtime).
if(THERMALCORE_REPLAY_STANDALONE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        THERMALCORE_MODE_REPLAY_STANDALONE)
endif()
