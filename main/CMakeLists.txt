set(ui_srcs "")
if(CONFIG_EXAMPLE_BT_UI_ENABLE)
    list(APPEND ui_srcs "bt_ui/bt_ui.c"
         "bt_ui/images/cis_stream_icon.c"
         "bt_ui/images/bis_stream_icon.c"
         "bt_ui/images/speakers_dark_dark_small.c"
         "bt_ui/images/speakers_dark_light_small.c"
         "bt_ui/images/speakers_light_dark_small.c"
         "bt_ui/fonts/lv_font_notosanssc_regular_28.c")
endif()

set(reqs "bt" "nvs_flash" "console" "fatfs" "esp_driver_sdmmc" "esp_driver_sdspi" "esp_driver_i2s" "soc")

# ---------------------------------------------------------------------------
# Versionsnummer aus version.txt (Projektwurzel) als Header erzeugen.
#
# Warum eine eigene Datei und nicht esp_app_get_description(): dieses Projekt
# liegt in keinem Git-Repository, deshalb steht dort "n/a". Ohne sichtbare
# Version laesst sich am seriellen Log nicht erkennen, welcher Stand gerade
# laeuft - genau daran sind wir schon einmal haengen geblieben: auf dem Chip
# lief noch der alte Build, waehrend die Quellen schon weiter waren.
#
# version.h wird in DIESEN Ordner geschrieben (nicht in build/), denn
# INCLUDE_DIRS "." enthaelt genau diesen Ordner. Im Build-Verzeichnis lag die
# Datei zwar, war aber nicht auffindbar ("fatal error: version.h: No such
# file"). version.txt bleibt die einzige Quelle, die von Hand geaendert wird;
# version.h traegt einen Hinweis darauf.
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

# I2S_ONLY_TEST=1 legt nur die I2S-Pipeline an und laesst den Datei-Zweig weg.
# Damit laesst sich pruefen, ob die I2S-Kette allein startet (der Fehler
# ESP_GMF_ERR_NOT_READY beim Job-Registrieren trat nur mit beiden Zweigen auf).
option(I2S_ONLY_TEST "Nur die I2S-Pipeline anlegen (Diagnose)" OFF)

# ---------------------------------------------------------------------------
# Messschalter fuer den I2S-Zweig.
#
#   0 = aus          (nur Datei-Zweig -> MP3 allein messbar)
#   1 = GMF          (aud_rate_cvt + aud_bit_cvt zusammen)
#   2 = linear       (aud_lin_resample, Q16.16-Interpolation, Vorgabe)
#   3 = shift+rate   (aud_bit_cvt aus GMF -> aud_rate_cvt aus GMF)
#   4 = wie 3        (eigener Shifter war als Messfall 4 gedacht und ist an
#                     einem GMF-Absturz gescheitert - siehe stream_proc.c)
#
# Gemessen auf Hardware: der eigene lineare Umsetzer braucht auf Kern 1 nur 7 %,
# die GMF-Kette in Fall 3 17 % und in Fall 1 19 %. Der Shift VOR der
# Ratenwandlung spart also nur ~2 Punkte - die GMF-Ratenwandlung wird vom Filter
# bestimmt, nicht von der Wortbreite.
#
# Umschalten ueber idf.py:  -DI2S_MODE=0/1/2/3
# Wichtig: der CMake-Cache muss beim Wechsel geloescht werden, sonst bleibt der
# alte Wert stehen. Siehe .tmp\mess_bauen.ps1.
# ---------------------------------------------------------------------------
set(I2S_MODE "2" CACHE STRING "I2S-Zweig: 0=aus, 1=GMF, 2=linear, 3=Shift(GMF)+GMF-Rate, 4=wie 3")

if(NOT I2S_MODE MATCHES "^[01234]$")
    message(FATAL_ERROR "I2S_MODE muss 0, 1, 2, 3 oder 4 sein (ist: ${I2S_MODE})")
endif()
message(STATUS "I2S-Modus: ${I2S_MODE} (0=aus, 1=GMF, 2=linear, 3=Shift(GMF)+GMF-Rate, 4=wie 3)")

# ---------------------------------------------------------------------------
# POOL_SMALL=1: kleinerer GMF-Pool fuer Module OHNE PSRAM.
#
# Auf so einem Modul stehen nur ~234 KiB interner RAM zur Verfuegung, und der
# vollstaendige Pool-Aufbau scheitert:
#
#   E ESP_GMF_POOL: esp_gmf_pool.c:71 (esp_gmf_pool_init): Memory exhausted
#   ESP_ERROR_CHECK failed ... expression: esp_gmf_pool_init(&pool)
#
# POOL_SMALL laesst weg, was dieses Projekt nie benutzt:
#   - aud_aec   (Echo-Unterdrueckung, gehoert zu einem Mikrofonpfad)
#   - aud_asrc  (wurde durch aud_rate_cvt -> aud_bit_cvt ersetzt)
#   - io_codec_dev und die Pipelines bt2codec/codec2bt (Board-Manager ist raus)
#
# Umschalten:  idf.py -DPOOL_SMALL=1 build   (bzw. .tmp\mess_bauen.ps1 -Small)
# ---------------------------------------------------------------------------
option(POOL_SMALL "Kleinerer GMF-Pool (fuer Module ohne PSRAM)" OFF)
message(STATUS "Pool klein: ${POOL_SMALL}")

idf_component_register(SRCS "main.c" "pool_reg.c" "cmd_reg.c" "stream_proc.c" "sd_card.c" "i2s_input.c"
                            "linear_resample.c"
                    ${ui_srcs}
                    INCLUDE_DIRS "." "bt_ui"
                    REQUIRES ${reqs}
                    )

if(I2S_ONLY_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE I2S_ONLY_TEST=1)
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE I2S_MODE=${I2S_MODE})

if(POOL_SMALL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE POOL_SMALL=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE POOL_SMALL=0)
endif()
