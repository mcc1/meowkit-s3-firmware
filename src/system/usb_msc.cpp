/**
 * @file usb_msc.cpp
 * @brief USB MSC U-disk — exposes SD card raw sectors to a host PC.
 *
 * Architecture
 * ────────────
 * static USBMSC s_msc  ← file-scope; constructor calls tinyusb_enable_interface()
 *                          during C++ static-init, BEFORE initArduino()/USB.begin().
 * USB.begin() (ARDUINO_USB_CDC_ON_BOOT=1) starts a CDC+MSC composite device.
 * The MSC LUN has no media initially (mediaPresent=false, block_count=0).
 *
 * On usb_msc_enable():
 *   SD_MMC.end() → sdmmc raw re-init → set callbacks + begin(sectors,512)
 *   → mediaPresent(true)   host sees drive appear (like inserting a card)
 *
 * On usb_msc_disable():
 *   mediaPresent(false) → USBMSC.end() → sdmmc_host_deinit → SD_MMC.begin()
 *
 * Core data-path:
 *   Host USB (GPIO20/19) ↕ TinyUSB USBMSC
 *   ↕ _msc_read_cb / _msc_write_cb
 *   ↕ sdmmc_read/write_sectors()  [ESP-IDF]
 *   ↕ SDMMC hw slot 1, 1-bit  CLK=47 CMD=48 D0=21
 */
#include "usb_msc.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <USB.h>
#include <USBMSC.h>

extern "C" {
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
}

#include "../bsp/config.h"

/* File-scope global: constructor calls tinyusb_enable_interface() at static-init
 * time, before USB.begin() — so MSC is included in the composite descriptor. */
static USBMSC                 s_msc;
static sdmmc_card_t *         s_card    = nullptr;
static volatile int           s_running = 0;
static volatile unsigned long s_bytes   = 0;

/* ── Sector callbacks — prefixed to avoid USBMSC.h typedef-name clash ──
 * A single transient SD error used to abort the whole USB transfer (the host
 * then retries, and after a few of those Windows marks the drive unstable).
 * Retry the sector operation a few times before reporting failure. */
static constexpr int      kMscRetries    = 3;
static constexpr uint32_t kMscRetryDelay = 5;   /* ms between attempts */
static volatile unsigned  s_retries      = 0;   /* diagnostics: retried ops */
static volatile unsigned  s_errors       = 0;   /* diagnostics: failed ops */
static volatile uint32_t  s_last_err_lba = 0;   /* diagnostics: last failing LBA */
/* Shutdown handshake between the app task (usb_msc_disable) and the TinyUSB
 * task (sector callbacks). Deinitialising the SDMMC host or freeing s_card
 * while a read is in flight on the other task crashed the device when the
 * user left MSC mode mid-transfer. */
static volatile int       s_stopping     = 0;   /* refuse new sector I/O */
static volatile unsigned long s_last_cb_ms = 0; /* millis() at the last sector callback */
static volatile unsigned long s_cb_count   = 0; /* sector callbacks since enable() */
static volatile unsigned long long s_sd_us = 0; /* time spent inside sdmmc_* calls */
static volatile unsigned  s_last_bufsize   = 0; /* bytes requested by the last callback */
static volatile int       s_cb_active    = 0;   /* callbacks currently inside sdmmc_* */

static int32_t _msc_read_cb(uint32_t lba, uint32_t /*offset*/,
                              void * buf, uint32_t bufsize)
{
    if (!s_card || s_stopping) return -1;
    s_last_cb_ms = millis();
    s_cb_count++;
    s_last_bufsize = bufsize;
    s_cb_active++;
    for (int attempt = 1; attempt <= kMscRetries && !s_stopping; attempt++) {
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t rc = sdmmc_read_sectors(s_card, buf, lba, bufsize / 512);
        s_sd_us += (unsigned long long)(esp_timer_get_time() - t0);
        if (rc == ESP_OK) {
            s_bytes += bufsize;
            s_cb_active--;
            return (int32_t)bufsize;
        }
        s_retries++;
        if (attempt < kMscRetries) delay(kMscRetryDelay);
    }
    s_cb_active--;
    if (s_stopping) return -1;
    /* No Serial output here: this runs on the TinyUSB task, and a USB CDC
     * write spins until tud_task() drains the FIFO -- which is us. The
     * counters are reported from usb_msc_disable() on the app task. */
    s_errors++;
    s_last_err_lba = lba;
    return -1;
}

static int32_t _msc_write_cb(uint32_t lba, uint32_t /*offset*/,
                               uint8_t * buf, uint32_t bufsize)
{
    if (!s_card || s_stopping) return -1;
    s_last_cb_ms = millis();
    s_cb_count++;
    s_last_bufsize = bufsize;
    s_cb_active++;
    for (int attempt = 1; attempt <= kMscRetries && !s_stopping; attempt++) {
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t rc = sdmmc_write_sectors(s_card, buf, lba, bufsize / 512);
        s_sd_us += (unsigned long long)(esp_timer_get_time() - t0);
        if (rc == ESP_OK) {
            s_bytes += bufsize;
            s_cb_active--;
            return (int32_t)bufsize;
        }
        s_retries++;
        if (attempt < kMscRetries) delay(kMscRetryDelay);
    }
    s_cb_active--;
    if (s_stopping) return -1;
    /* No Serial output here: this runs on the TinyUSB task, and a USB CDC
     * write spins until tud_task() drains the FIFO -- which is us. The
     * counters are reported from usb_msc_disable() on the app task. */
    s_errors++;
    s_last_err_lba = lba;
    return -1;
}

static volatile int s_host_ejected = 0;   /* host issued an eject */

static bool _msc_startstop_cb(uint8_t /*pwr*/, bool /*start*/, bool load_eject)
{
    if (load_eject) s_host_ejected = 1;   /* TinyUSB task: no Serial here */
    return true;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int usb_msc_enable(void)
{
    if (s_running) return 1;

    s_bytes = 0;
    s_retries = 0;
    s_errors  = 0;
    s_last_err_lba = 0;
    s_host_ejected = 0;
    s_stopping     = 0;
    s_cb_active    = 0;
    s_last_cb_ms   = 0;
    s_cb_count     = 0;
    s_sd_us        = 0;
    s_last_bufsize = 0;

    /* 1. Dismount FAT-FS — release exclusive SDMMC bus ownership */
    SD_MMC.end();

    /* 2. Re-init SDMMC in 1-bit raw mode at SDMMC_FREQ_DEFAULT (20 MHz).
     *
     *    Do NOT "match" the FAT mount's 10000 kHz here. On the ESP-IDF 4.4
     *    that this Arduino core ships, sdmmc_init_host_frequency() only
     *    applies the standard rates (20/26/40/52 MHz); any other value leaves
     *    the card at the 400 kHz probing clock. Measured on 2026-09-06 with
     *    max_freq_khz = 10000: every 4 KB sector read took ~84 ms (= 4096 x 8
     *    bits / 400 kbit/s), MSC throughput 41 KB/s, Windows needed ~8 min to
     *    mount a 128 GB FAT32 card. 20 MHz is what the vendor firmware used
     *    for MSC and is the lowest rate the driver actually honours. */
    sdmmc_host_t host  = SDMMC_HOST_DEFAULT();
    host.max_freq_khz  = SDMMC_FREQ_DEFAULT;    /* 20 MHz (see note above) */
    host.slot          = SDMMC_HOST_SLOT_1;
    host.flags         = SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk   = (gpio_num_t)HAL_PIN_SD_CLK;
    slot.cmd   = (gpio_num_t)HAL_PIN_SD_CMD;
    slot.d0    = (gpio_num_t)HAL_PIN_SD_D0;
    slot.width = 1;
    slot.flags = 0;

    if (sdmmc_host_init() != ESP_OK) {
        Serial.println("[MSC] sdmmc_host_init failed");
        goto fail_remount;
    }
    if (sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot) != ESP_OK) {
        Serial.println("[MSC] sdmmc_host_init_slot failed");
        sdmmc_host_deinit();
        goto fail_remount;
    }
    s_card = (sdmmc_card_t *)heap_caps_malloc(sizeof(sdmmc_card_t),
                                               MALLOC_CAP_DEFAULT);
    if (!s_card) {
        sdmmc_host_deinit();
        goto fail_remount;
    }
    if (sdmmc_card_init(&host, s_card) != ESP_OK) {
        Serial.println("[MSC] sdmmc_card_init failed");
        free(s_card); s_card = nullptr;
        sdmmc_host_deinit();
        goto fail_remount;
    }

    /* 3. Wire callbacks, set LUN geometry, expose media to host.
     *    USB is already a CDC+MSC composite — host polls test_unit_ready
     *    every few seconds; mediaPresent(true) makes the drive appear. */
    s_msc.vendorID("MeowKit");
    s_msc.productID("SDCard");
    s_msc.productRevision("1.0");
    s_msc.onRead(_msc_read_cb);
    s_msc.onWrite(_msc_write_cb);
    s_msc.onStartStop(_msc_startstop_cb);
    s_msc.mediaPresent(true);
    if (!s_msc.begin((uint32_t)s_card->csd.capacity,
                     (uint16_t)s_card->csd.sector_size)) {
        Serial.println("[MSC] USBMSC.begin() failed");
        free(s_card); s_card = nullptr;
        sdmmc_host_deinit();
        goto fail_remount;
    }

    s_running = 1;
    Serial.printf("[MSC] enabled — %lu sectors × %u B\n",
                  (unsigned long)s_card->csd.capacity,
                  (unsigned)s_card->csd.sector_size);
    return 1;

fail_remount:
    SD_MMC.setPins(HAL_PIN_SD_CLK, HAL_PIN_SD_CMD, HAL_PIN_SD_D0);
    SD_MMC.begin("/sdcard", true, false, HAL_SD_FREQ_KHZ);
    return 0;
}

void usb_msc_disable(void)
{
    if (s_running) {
        Serial.printf("[MSC] session end: %lu bytes, %u retried ops, %u failed ops\n",
                      (unsigned long)s_bytes, (unsigned)s_retries, (unsigned)s_errors);
        if (s_host_ejected) Serial.println("[MSC] host had ejected the drive");
        if (s_errors)
            Serial.printf("[MSC] last failing LBA %lu\n", (unsigned long)s_last_err_lba);
    }
    if (!s_running) return;

    /* 1. Refuse new sector I/O, tell the host the media is gone, then wait for
     *    any callback still inside sdmmc_read/write_sectors() on the TinyUSB
     *    task to return. The host keeps issuing reads for a moment after the
     *    media-removed notification; tearing the host down under one of them
     *    rebooted the device. */
    s_stopping = 1;
    s_msc.mediaPresent(false);
    for (int waited = 0; s_cb_active > 0 && waited < 2000; waited += 10) delay(10);
    if (s_cb_active > 0) Serial.println("[MSC] warning: sector I/O still in flight at shutdown");
    delay(50);              /* let the host see the media change before we vanish */
    s_msc.end();            /* clear callbacks + block info */

    /* 2. Release raw SDMMC resources */
    if (s_card) {
        sdmmc_host_deinit();
        free(s_card);
        s_card = nullptr;
    }
    s_running  = 0;
    s_stopping = 0;

    /* Remount FAT-FS for the rest of the firmware */
    SD_MMC.setPins(HAL_PIN_SD_CLK, HAL_PIN_SD_CMD, HAL_PIN_SD_D0);
    if (!SD_MMC.begin("/sdcard", true, false, HAL_SD_FREQ_KHZ)) {
        Serial.println("[MSC] FS remount failed");
    } else {
        Serial.println("[MSC] FS remounted");
    }
}

int usb_msc_is_active(void)
{
    return s_running;
}

unsigned long usb_msc_bytes_transferred(void)
{
    return s_bytes;
}

void usb_msc_perf(unsigned long* callbacks, unsigned long long* sd_us_total, unsigned* last_bufsize)
{
    if (callbacks)    *callbacks    = s_cb_count;
    if (sd_us_total)  *sd_us_total  = s_sd_us;
    if (last_bufsize) *last_bufsize = s_last_bufsize;
}

void usb_msc_stats(unsigned* retries, unsigned* errors, unsigned long* last_cb_ms)
{
    if (retries)    *retries    = s_retries;
    if (errors)     *errors     = s_errors;
    if (last_cb_ms) *last_cb_ms = s_last_cb_ms;
}
