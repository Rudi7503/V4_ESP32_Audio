set(reqs "bt" "nvs_flash" "console" "fatfs" "esp_driver_sdmmc" "esp_driver_sdspi" "esp_driver_i2s" "soc")

# ---------------------------------------------------------------------------
# version.txt (Projektwurzel) -> version.h
#
# Die Version steht im seriellen Log, damit am Board erkennbar ist, welcher
# Stand laeuft. version.txt ist die einzige Quelle, die von Hand geaendert wird.
# ---------------------------------------------------------------------------
set(_ver_file "${CMAKE_CURRENT_LIST_DIR}/../version.txt")
if(NOT EXISTS "${_ver_file}")
    message(FATAL_ERROR "version.txt fehlt: ${_ver_file}")
endif()

file(READ "${_ver_file}" _ver_raw)
string(STRIP "${_ver_raw}" _ver)
file(WRITE "${CMAKE_CURRENT_LIST_DIR}/version.h"
     "/* Erzeugt aus version.txt - nicht von Hand aendern. */\n"
     "#pragma once\n"
     "#define APP_VERSION_STRING \"${_ver}\"\n")
message(STATUS "App-Version: ${_ver}")

idf_component_register(SRCS "main.c" "pool_reg.c" "cmd_reg.c" "stream_proc.c" "sd_card.c" "i2s_input.c"
                            "linear_resample.c"
                    INCLUDE_DIRS "."
                    REQUIRES ${reqs}
                    )

# Der GMF-Pool bleibt klein (ohne aud_aec/aud_asrc): auf einem Modul ohne PSRAM
# ist der vollstaendige Aufbau nicht moeglich.
target_compile_definitions(${COMPONENT_LIB} PRIVATE POOL_SMALL=1)
