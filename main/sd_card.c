/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "sd_card.h"

#define SD_MOUNT_POINT         "/sdcard"
/*
 * Gleichzeitig offene Dateien im FAT-VFS (esp_vfs_fat_mount_config_t.max_files).
 * Das I2C-Protokoll der Vampire haelt bis zu SD_FS_MAX_FILE_HANDLES (4) Dateien
 * und SD_FS_MAX_DIR_HANDLES (2) Verzeichnisse offen, und opendir() belegt im
 * FatFs-VFS ebenfalls einen Platz. Die frueheren 5 waren dafuer zu knapp; 8
 * laesst Luft und kostet nur wenige hundert Byte.
 */
#define SD_MAX_FILE_HANDLES    8
#define SD_MAX_FREQ_KHZ        4000
#define SD_MOUNT_ATTEMPTS      3
#define SD_RETRY_DELAY_MS      200

/*
 * Pinbelegung des Kartenslots (1-Bit-SDMMC).
 *
 * Der ESP32 SDMMC-Host hat KEINE GPIO-Matrix: fuer Slot 1 stehen die Pins fest
 * (components/soc/esp32/include/soc/sdmmc_pins.h):
 *
 *     CLK = GPIO14   CMD = GPIO15   D0 = GPIO2
 *
 * Die Defines sind deshalb Dokumentation, nicht Konfiguration - slot.clk/cmd/d0
 * koennen die Leitungen nicht verschieben. Sie bleiben, damit Schaltplan und
 * Code sich Zeile fuer Zeile vergleichen lassen.
 *
 * GPIO2 ist ausserdem ein Strapping-Pin, der fuer den Download-Modus LOW sein
 * muss, und der DAT0-Pull-up haelt ihn HIGH: deshalb muss das Modul zum Flashen
 * aus der Platine.
 */
#define SD_PIN_CLK   GPIO_NUM_14
#define SD_PIN_CMD   GPIO_NUM_15
#define SD_PIN_D0    GPIO_NUM_2

/*
 * Card Detect des microSD-Halters: der Schalter zieht CD auf GND, wenn eine
 * Karte steckt - LOW heisst also "vorhanden". GPIO34 ist input-only und hat
 * keinen internen Pull-up; fuer ein sicheres "nicht vorhanden" braucht es einen
 * externen 10k nach 3,3 V.
 *
 * GPIO34 kann ausserdem nicht als Ausgang dienen (input-only) - er ist deshalb
 * nur fuer Card Detect verwendbar, nie als Chip Select.
 */
#define SD_PIN_CD    GPIO_NUM_34

/*
 * DAT3 des Kartenslots (Kartenkontakt 2), hier GPIO13.
 *
 * Die Karte entscheidet ueber diese Leitung, in welchem Modus sie arbeitet:
 * sieht sie DAT3/CS beim CMD0 LOW, schaltet sie in den SPI-Modus und haelt
 * danach DAT0 als Busy-Signal auf LOW. Im 1-Bit-SDMMC-Modus wird DAT3 sonst
 * nicht gebraucht, und IDF konfiguriert D1..D3 erst ab 4 Bit Breite
 * (esp_driver_sdmmc/src/sd_host_sdmmc.c:1376-1385, dort steht ausdruecklich
 * "Force D3 high to make slave enter SD mode") - deshalb ziehen wir es selbst
 * vor dem Mountversuch auf HIGH (sd_dat3_high()).
 */
#define SD_PIN_DAT3  GPIO_NUM_13

static const char *TAG = "SD_CARD";

static sdmmc_card_t *s_card;
static bool          s_mounted;
static bool          s_cd_ready;

static void sd_cd_pin_init(void)
{
    if (s_cd_ready) {
        return;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << SD_PIN_CD,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,      /* GPIO34 has no internal pull-up */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    s_cd_ready = true;
}

bool sd_card_is_present(void)
{
    sd_cd_pin_init();
    return (gpio_get_level(SD_PIN_CD) == 0);
}

/*
 * STAND 0.9.46 (gemessen, siehe docs/MESSRIEHE.md Abschnitt 12):
 *
 * Der weiter unten beschriebene Flash-Takt war nur EIN Faktor. Mit Flash
 * 40 MHz (im Image und zur Laufzeit verifiziert) mountet der WROVER-KIT
 * dieselbe Karte mit DEMSELBEN Build in 103 ms:
 *
 *   I (1125) SD_CARD: mounted at /sdcard (SDMMC 1 Bit)
 *
 * Der WROOM faellt weiter aus - und zwar schon im ersten CIU-Clock-Update,
 * bevor ein Bit zur Karte geht:
 *
 *   E sd_host_slot_clock_update_command(993): sd_host_start_command returned 0x107
 *
 * Das passiert mit Karte, ohne Karte, mit abgeklemmtem SD-Breakout, bei 4 MHz
 * und bei 20 MHz SD-Takt. Damit sind Karte, Halter, Verkabelung, SD-Takt und
 * Flash-Takt als Ursache widerlegt; die Firmware ebenfalls (der WROVER laeuft).
 * Offen ist ein modul-/platinenspezifischer Rest; Pruefpunkte und die
 * Registerdiagnose ("sdreg") stehen in Abschnitt 12a-12f der Messreihe.
 *
 * Weg 1: SDMMC (1 Bit) - der schnelle, native Weg.
 *
 * FEHLERBILD, das uns lange beschaeftigt hat:
 *
 *   I (2063) SD_HOST: src_freq_hz: 160000000
 *   E (3063) SD_HOST: sd_host_start_command returned 0x107
 *   E (3063) SD_HOST: sd_host_slot_set_card_clk(568): ... failed to disable clk
 *
 * 0x107 ist ESP_ERR_TIMEOUT: schon das Clock-Update-Kommando der CIU laeuft in
 * den 5-s-Timeout, also VOR jeder Datenuebertragung.
 *
 * URSACHE (am 02.10. gemessen, nicht geraten): der FLASH-TAKT.
 *
 *   Flash 80 MHz -> SDMMC faellt mit 0x107 aus
 *   Flash 40 MHz -> SD mountet in 100 ms
 *
 * Der SDMMC-Takt kommt aus derselben PLL wie der Flash-Takt. Mit PSRAM laeuft
 * der Flash zwangsweise mit 40 MHz (Flash und PSRAM teilen sich den SPI-Takt),
 * ohne PSRAM zieht ESP-IDF 80 MHz - deshalb sah es aus, als ob WROVER-Module
 * gingen und WROOM-Module nicht:
 *
 *   WROVER + Build MIT PSRAM  (40 MHz) -> SD laeuft
 *   WROVER + Build OHNE PSRAM (80 MHz) -> SD faellt aus   <- der Beweis
 *   WROOM  + Build OHNE PSRAM (80 MHz) -> SD faellt aus
 *
 * Der Flash-Takt steht deshalb in sdkconfig.defaults.esp32.psram auf 40 MHz
 * (CONFIG_ESPTOOLPY_FLASHFREQ_40M). Das kostet etwas Flash-Durchsatz, ist aber
 * die Voraussetzung dafuer, dass die SD auf JEDEM Modul laeuft.
 *
 * Falsch war meine frueher hier stehende Deutung "der SDMMC-Block des
 * Ersatzmoduls ist defekt": dagegen sprach schon, dass das alte Modul mit
 * demselben Board und derselben Karte mountete - es lief nur mit einem
 * PSRAM-Build. Nach dem Flashen eines No-PSRAM-Builds auf DASSELBE Modul
 * scheiterte die SD dort genauso.
 */
/*
 * DAT3 (Kartenkontakt 2, bei uns GPIO13) MUSS waehrend der Initialisierung HIGH
 * sein. Ueber diese Leitung entscheidet die Karte, in welchem Modus sie
 * arbeitet: sieht sie CS/DAT3 beim CMD0 LOW, schaltet sie in den SPI-Modus und
 * haelt danach DAT0 (dort DO) als Busy-Signal auf LOW.
 *
 * Genau das ist am 04.10. gemessen worden: der SDMMC-Host verweigert jedes
 * Kommando, solange die Karte DAT0 auf LOW haelt - im Registerverlauf
 * (docs/MESSRIEHE.md, Abschnitt 12k) steht direkt vor dem Timeout
 * `status=0x00000306`, also data_busy = 1, und `start_command = 1`.
 *
 * Unser Aufbau nutzt im 1-Bit-Modus nur CLK/CMD/D0; GPIO13 bleibt frei, und
 * IDF konfiguriert D1..D3 erst ab 4 Bit Breite
 * (esp_driver_sdmmc/src/sd_host_sdmmc.c:1376-1385, dort steht ausdruecklich
 * "Force D3 high to make slave enter SD mode"). Also machen wir es hier selbst.
 */
static void sd_dat3_high(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << SD_PIN_DAT3,    /* GPIO13 = DAT3 am Kartenslot */
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(SD_PIN_DAT3, 1);
}

/*
 * DAT1/DAT2/DAT3 (GPIO4/12/13) nach dem Boot auf HIGH ziehen.
 *
 * WOZU: Laut ESP-IDF-Doku "SD Pullup Requirements" muessen auch im 1-Bit-Modus
 * ALLE Datenleitungen hochgezogen sein (CMD und D0-D3, je 50k). GPIO12 ist aber
 * gleichzeitig MTDI und legt beim Reset die Flash-Spannung VDD_SDIO fest:
 *
 *   WROOM-32 (3,3 V Flash): GPIO12 muss beim Reset LOW sein (interner Pulldown)
 *   WROVER   (1,8 V Flash): GPIO12 muss beim Reset HIGH sein (interner Pullup)
 *
 * Die Doku nennt das ausdruecklich "incompatible with SD card operation". Der
 * interne Pull-up wirkt erst NACH dem Boot und stoert das Strapping deshalb
 * nicht - das ist der in der Doku fuer den ESP32 als Host empfohlene Weg:
 *
 *   "In the case using ESP32 host only, external pullup can be omitted and an
 *    internal pullup can be enabled using a gpio_pullup_en(GPIO_NUM_12); call.
 *    Most SD cards work fine when an internal pullup on GPIO12 line is enabled."
 *
 * gpio_set_pull_mode(..., GPIO_PULLUP_ONLY) schaltet den internen Pulldown mit
 * ab - sonst kaempfen beide internen Widerstaende gegeneinander.
 *
 * Grenze: der interne Pull-up ist schwach (ca. 45k). Zieht die Platine DAT2
 * stark herunter, kommt er nicht an - dann muss DAT2 getrennt werden (im
 * 1-Bit-Modus wird es nicht gebraucht).
 */
static void sd_dat_pullups(void)
{
    gpio_set_pull_mode(GPIO_NUM_4,  GPIO_PULLUP_ONLY);   /* DAT1 */
    gpio_set_pull_mode(GPIO_NUM_12, GPIO_PULLUP_ONLY);   /* DAT2 = MTDI */
    gpio_set_pull_mode(GPIO_NUM_13, GPIO_PULLUP_ONLY);   /* DAT3 */
}

static esp_err_t sd_mount_try_sdmmc(void)
{
    /* DAT3/CS HIGH, damit die Karte im SD-Modus bleibt und nicht auf SPI
     * umschaltet, und die Datenleitungen hochziehen (siehe oben). */
    sd_dat3_high();
    sd_dat_pullups();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SD_MAX_FREQ_KHZ;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk   = SD_PIN_CLK;
    slot.cmd   = SD_PIN_CMD;
    slot.d0    = SD_PIN_D0;
    slot.width = 1;
    /* Mit verdrahtetem CD verweigert der Treiber jedes Kommando mit
     * ESP_ERR_NOT_FOUND, solange der Pin HIGH liest - eine fehlende Karte
     * scheitert damit sofort statt im Timeout. */
    slot.cd    = SD_PIN_CD;
    slot.wp    = SDMMC_SLOT_NO_WP;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = SD_MAX_FILE_HANDLES,
        .allocation_unit_size = 16 * 1024,
    };

    s_card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
    }
    return err;
}

esp_err_t sd_card_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    sd_cd_pin_init();

    bool present = sd_card_is_present();
    ESP_LOGI(TAG, "Card detect (GPIO%d): %s", SD_PIN_CD,
             present ? "card inserted" : "no card / pin floating");

    /*
     * Nur noch ein Weg: 1-Bit-SDMMC.
     *
     * Der frueher hier stehende SPI-Rueckfall auf denselben Leitungen ist in
     * 0.9.60 entfernt worden. Er war reine Absicherung - gebraucht wurde er nie:
     * die eigentlichen Ursachen des SD-Ausfalls waren der Flash-Takt (80 statt
     * 40 MHz) und der fehlende DAT0-Pull-up, beide behoben. Ein zweiter
     * Mountweg haette nur einen zweiten Fehlerpfad bedeutet, den niemand
     * wartet.
     */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= SD_MOUNT_ATTEMPTS; attempt++) {
        ESP_LOGI(TAG, "SDMMC-Versuch %d/%d (max %d kHz)", attempt, SD_MOUNT_ATTEMPTS, SD_MAX_FREQ_KHZ);
        err = sd_mount_try_sdmmc();
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "SDMMC-Versuch %d fehlgeschlagen: %s", attempt, esp_err_to_name(err));
        if (attempt < SD_MOUNT_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(SD_RETRY_DELAY_MS));
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s (Karte richtig gesteckt? Kontakte? Flash-Takt 40 MHz?)",
                 esp_err_to_name(err));
        s_mounted = false;
        return err;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "mounted at %s (SDMMC 1 Bit)", SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

esp_err_t sd_card_unmount(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }
    esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;
    return err;
}

bool sd_card_is_mounted(void)
{
    return s_mounted;
}

const char *sd_card_transport(void)
{
    return s_mounted ? "SDMMC 1 Bit" : "nicht verbunden";
}

uint16_t sd_card_sector_size(void)
{
    if (!s_mounted || s_card == NULL) {
        return 0;
    }
    return (uint16_t)s_card->csd.sector_size;
}
