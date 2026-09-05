/**
 * @file usb_msc.h
 * @brief USB Mass Storage Class — exposes SD card as U-disk to a host PC.
 *
 * Enable:   SD_MMC.end() → sdmmc raw init → USBMSC.begin() → USB re-enumerate
 * Disable:  USBMSC.end() → sdmmc deinit → SD_MMC.begin() (FAT remount)
 * Active:   FAT-FS is unmounted; do NOT call SD_MMC file APIs while active.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start USB MSC — dismount SD FAT-FS and expose raw sectors to host.
 *  Returns 1 on success, 0 on failure (FAT-FS is remounted on failure). */
int  usb_msc_enable(void);

/** Stop USB MSC — detach from host and remount SD FAT-FS. */
void usb_msc_disable(void);

/** Returns 1 while MSC session is active (between enable and disable). */
int  usb_msc_is_active(void);

/** Cumulative bytes transferred (read + write) since last enable(). */
unsigned long usb_msc_bytes_transferred(void);
/** Diagnostics: retried sector ops, failed sector ops, millis() of the last
 *  sector callback (0 = none yet). Any pointer may be NULL. */
void usb_msc_stats(unsigned* retries, unsigned* errors, unsigned long* last_cb_ms);
/** Throughput diagnostics: number of sector callbacks, microseconds spent
 *  inside sdmmc_read/write_sectors() in total, and the byte size of the last
 *  callback (tells how TinyUSB chunks host requests). Any pointer may be NULL. */
void usb_msc_perf(unsigned long* callbacks, unsigned long long* sd_us_total, unsigned* last_bufsize);

#ifdef __cplusplus
}
#endif
