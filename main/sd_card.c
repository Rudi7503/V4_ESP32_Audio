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
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "sd_card.h"

#define SD_MOUNT_POINT         "/sdcard"
#define SD_MAX_FILE_HANDLES    5
#define SD_MAX_FREQ_KHZ        4000
#define SD_SPI_MAX_FREQ_KHZ    1000
#define SD_MOUNT_ATTEMPTS      3
#define SD_RETRY_DELAY_MS      200

/*
 * Pinbelegung des Kartenslots. Sie gilt fuer BEIDE Wege - es wird nichts
 * umverdrahtet, nur die Bedeutung der Leitungen aendert sich:
 *
 *   Leitung   SDMMC (1 Bit)      SPI
 *   CLK       CLK                CLK
 *   CMD       CMD (bidirekt.)    MOSI (nur zum Kart)
 *   D0        DAT0               MISO (nur vom Kart)
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
 * NICHT als CS verwenden! Der erste Versuch war genau das und ist falsch:
 * esp_driver_sdspi steuert CS per gpio_set_level und legt den Pin dabei als
 * AUSGANG an (sdspi_host.c:377-396, "Configure CS pin", .mode = GPIO_MODE_OUTPUT).
 * GPIO34 kann das auf dem ESP32 nicht - er ist input-only.
 */
#define SD_PIN_CD    GPIO_NUM_34

/*
 * Chip Select fuer den SPI-Weg.
 *
 * SPI braucht - anders als SDMMC im 1-Bit-Modus - eine CS-Leitung, und die ist
 * am Slot zusaetzlich zu verdrahten (Kartenkontakt D3/CD). Sie MUSS ein freier
 * Ausgangs-Pin sein; input-only-Pins wie 34/35 scheiden aus.
 *
 * GPIO13 ist auf diesem Testboard frei und ein normaler Ausgang.
 */
#define SD_PIN_CS    GPIO_NUM_13

static const char *TAG = "SD_CARD";

static sdmmc_card_t *s_card;
static bool          s_mounted;
static bool          s_cd_ready;
static bool          s_spi_bus;         /* SPI-Bus angelegt -> beim Unmount freigeben */
static bool          s_over_spi;        /* ueber welchen Weg haengt die Karte? */

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
 *
 * Achtung: der SPI-Weg benutzt GPIO13 als CS - dort darf der Pin nicht fest
 * auf HIGH liegen, deshalb wird er nur vor dem SDMMC-Versuch gesetzt.
 */
static void sd_dat3_high(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << SD_PIN_CS,      /* GPIO13 = DAT3 am Kartenslot */
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(SD_PIN_CS, 1);
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

/*
 * Weg 2: SPI auf denselben Leitungen.
 *
 * WOZU: falls SDMMC einmal nicht geht, kann die Karte ueber SPI trotzdem
 * laufen - langsamer, aber voll funktionsfaehig. Der Versuch trennt ausserdem
 * zwei Faelle sauber:
 *   - SPI geht   -> SDMMC-Weg gestoert, Karte und Halter sind in Ordnung
 *   - SPI geht nicht -> es liegt an Karte/Halter/Verkabelung
 *
 * Wichtig: der eigentliche SD-Fehler war der Flash-Takt (siehe oben). Als der
 * noch auf 80 MHz stand, scheiterte auch der SPI-Weg - mit
 *   sdmmc_init_sd_if_cond: send_if_cond (1) returned 0x108
 * also CMD8 ohne Antwort. Mit 40 MHz Flash wird dieser Weg gar nicht erst
 * gebraucht, SDMMC mountet sofort. Der Fallback bleibt als Absicherung.
 */
static esp_err_t sd_mount_try_spi(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = SD_SPI_MAX_FREQ_KHZ;
    host.unaligned_multi_block_rw_max_chunk_size = 8;

    /*
     * MOSI = CMD (GPIO15), MISO = D0 (GPIO2), CLK = GPIO14,
     * CS = eigener Ausgang (SD_PIN_CS). max_transfer_sz wie im offiziellen
     * Beispiel (examples/storage/sd_card/sdspi/main/sd_card_example_main.c:144-151).
     */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_CMD,
        .miso_io_num = SD_PIN_D0,
        .sclk_io_num = SD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t err = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI-Bus liess sich nicht anlegen: %s", esp_err_to_name(err));
        return err;
    }
    s_spi_bus = true;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = host.slot;
    slot.gpio_cs = SD_PIN_CS;
    slot.gpio_cd = SD_PIN_CD;
    slot.gpio_wp = SDSPI_SLOT_NO_WP;
    ESP_LOGI(TAG, "SPI: CLK=GPIO%d MOSI=GPIO%d MISO=GPIO%d CS=GPIO%d",
             SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0, SD_PIN_CS);

    esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = SD_MAX_FILE_HANDLES,
        .allocation_unit_size = 16 * 1024,
    };

    s_card = NULL;
    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        spi_bus_free(host.slot);
        s_spi_bus = false;
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

    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= SD_MOUNT_ATTEMPTS; attempt++) {
        ESP_LOGI(TAG, "SDMMC-Versuch %d/%d (max %d kHz)", attempt, SD_MOUNT_ATTEMPTS, SD_MAX_FREQ_KHZ);
        err = sd_mount_try_sdmmc();
        if (err == ESP_OK) {
            s_over_spi = false;
            break;
        }
        ESP_LOGW(TAG, "SDMMC-Versuch %d fehlgeschlagen: %s", attempt, esp_err_to_name(err));
        if (attempt < SD_MOUNT_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(SD_RETRY_DELAY_MS));
        }
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SDMMC geht nicht - versuche SPI auf denselben Leitungen");
        for (int attempt = 1; attempt <= SD_MOUNT_ATTEMPTS; attempt++) {
            ESP_LOGI(TAG, "SPI-Versuch %d/%d", attempt, SD_MOUNT_ATTEMPTS);
            err = sd_mount_try_spi();
            if (err == ESP_OK) {
                s_over_spi = true;
                break;
            }
            ESP_LOGW(TAG, "SPI-Versuch %d fehlgeschlagen: %s", attempt, esp_err_to_name(err));
            if (attempt < SD_MOUNT_ATTEMPTS) {
                vTaskDelay(pdMS_TO_TICKS(SD_RETRY_DELAY_MS));
            }
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s (SDMMC und SPI versucht - Karte richtig gesteckt? Kontakte?)",
                 esp_err_to_name(err));
        s_mounted = false;
        return err;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "mounted at %s (%s)", SD_MOUNT_POINT, s_over_spi ? "SPI" : "SDMMC 1 Bit");
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
    if (s_spi_bus) {
        spi_bus_free(SPI2_HOST);
        s_spi_bus = false;
    }
    s_over_spi = false;
    return err;
}

bool sd_card_is_mounted(void)
{
    return s_mounted;
}

const char *sd_card_transport(void)
{
    if (!s_mounted) {
        return "nicht verbunden";
    }
    return s_over_spi ? "SPI" : "SDMMC 1 Bit";
}
